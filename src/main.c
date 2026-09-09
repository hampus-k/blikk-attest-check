/*
 * blikk-attest-check
 *
 * Logs in to the Blikk REST API (https://publicapidocs.blikk.com) and
 * checks the attestation ("attest") flags of the current month's time
 * reports for one or more users. Meant to be called as a step in an
 * automation flow (e.g. Power Automate, a scheduled task, a Google Apps
 * Script shelling out to it, ...): it prints a machine-readable JSON
 * summary to stdout and signals the result via its exit code.
 *
 * Exit codes:
 *   0  OK      - request succeeded, every time report in the period is attested
 *   1  PENDING - request succeeded, but some reports are not (yet) attested
 *   2  USAGE   - bad arguments / missing configuration
 *   3  ERROR   - authentication or API request failed
 *
 * See README.md for configuration and usage examples.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blikk_api.h"
#include "http_client.h"
#include "report.h"
#include "../third_party/cJSON.h"

#define EXIT_OK      0
#define EXIT_PENDING 1
#define EXIT_USAGE   2
#define EXIT_ERROR   3

typedef struct {
    const char *app_id;
    const char *app_secret;
    const char *base_url;
    char user_ids_csv[1024]; /* built up from repeated --user-id */
    int month_year;          /* 0 = use current month */
    int month_month;         /* 1-12 */
    int text_format;         /* 0 = json (default), 1 = text */
} cli_options_t;

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month /* 1-12 */)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static void print_usage(FILE *f, const char *argv0)
{
    fprintf(f,
        "Usage: %s [options]\n"
        "\n"
        "Checks the Blikk attestation flags for the current month's time reports.\n"
        "\n"
        "Options:\n"
        "  --app-id <id>        Blikk API application id     (or env BLIKK_APP_ID)\n"
        "  --app-secret <s>     Blikk API application secret (or env BLIKK_APP_SECRET)\n"
        "  --user-id <id>       Restrict to this Blikk user id. Repeatable.\n"
        "                       (or env BLIKK_USER_IDS, comma-separated)\n"
        "  --month <YYYY-MM>    Check this month instead of the current one.\n"
        "  --base-url <url>     Override the API base URL   (or env BLIKK_BASE_URL)\n"
        "                       Default: %s\n"
        "  --format json|text   Output format. Default: json\n"
        "  -h, --help           Show this help and exit\n"
        "\n"
        "Exit codes:\n"
        "  0  all time reports in the period are attested\n"
        "  1  request succeeded, but some reports are not (yet) attested\n"
        "  2  bad arguments / missing configuration\n"
        "  3  authentication or API request failed\n",
        argv0, BLIKK_DEFAULT_BASE_URL);
}

static void append_user_id(cli_options_t *opts, const char *id)
{
    const size_t len = strlen(opts->user_ids_csv);
    const size_t avail = sizeof(opts->user_ids_csv) - len;
    snprintf(opts->user_ids_csv + len, avail, "%s%s", (len > 0) ? "," : "", id);
}

static int parse_args(int argc, char **argv, cli_options_t *opts)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout, argv[0]);
            exit(EXIT_OK);
        } else if (strcmp(arg, "--app-id") == 0 && i + 1 < argc) {
            opts->app_id = argv[++i];
        } else if (strcmp(arg, "--app-secret") == 0 && i + 1 < argc) {
            opts->app_secret = argv[++i];
        } else if (strcmp(arg, "--base-url") == 0 && i + 1 < argc) {
            opts->base_url = argv[++i];
        } else if (strcmp(arg, "--user-id") == 0 && i + 1 < argc) {
            append_user_id(opts, argv[++i]);
        } else if (strcmp(arg, "--month") == 0 && i + 1 < argc) {
            int year, month;
            if (sscanf(argv[++i], "%d-%d", &year, &month) != 2 || month < 1 || month > 12) {
                fprintf(stderr, "error: --month expects YYYY-MM, got \"%s\"\n", argv[i]);
                return -1;
            }
            opts->month_year = year;
            opts->month_month = month;
        } else if (strcmp(arg, "--format") == 0 && i + 1 < argc) {
            const char *fmt = argv[++i];
            if (strcmp(fmt, "json") == 0) {
                opts->text_format = 0;
            } else if (strcmp(fmt, "text") == 0) {
                opts->text_format = 1;
            } else {
                fprintf(stderr, "error: --format expects \"json\" or \"text\", got \"%s\"\n", fmt);
                return -1;
            }
        } else {
            fprintf(stderr, "error: unrecognised option \"%s\"\n", arg);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    cli_options_t opts;
    char from_date[11];
    char to_date[11];
    char month_str[8];
    char token[512];
    char err[1024];
    time_t now;
    struct tm tm_now;
    blikk_timereport_list_t list;
    report_summary_t summary;
    int exit_code = EXIT_ERROR;

    memset(&opts, 0, sizeof(opts));
    opts.base_url = getenv("BLIKK_BASE_URL");
    if (opts.base_url == NULL || opts.base_url[0] == '\0') {
        opts.base_url = BLIKK_DEFAULT_BASE_URL;
    }
    opts.app_id = getenv("BLIKK_APP_ID");
    opts.app_secret = getenv("BLIKK_APP_SECRET");
    {
        const char *env_users = getenv("BLIKK_USER_IDS");
        if (env_users != NULL) {
            snprintf(opts.user_ids_csv, sizeof(opts.user_ids_csv), "%s", env_users);
        }
    }

    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(stderr, argv[0]);
        return EXIT_USAGE;
    }

    if (opts.app_id == NULL || opts.app_id[0] == '\0' ||
        opts.app_secret == NULL || opts.app_secret[0] == '\0') {
        fprintf(stderr, "error: missing Blikk API credentials "
                        "(set --app-id/--app-secret or BLIKK_APP_ID/BLIKK_APP_SECRET)\n");
        print_usage(stderr, argv[0]);
        return EXIT_USAGE;
    }

    if (opts.month_year == 0) {
        now = time(NULL);
        if (localtime_r(&now, &tm_now) == NULL) {
            fprintf(stderr, "error: could not determine current date\n");
            return EXIT_ERROR;
        }
        opts.month_year = tm_now.tm_year + 1900;
        opts.month_month = tm_now.tm_mon + 1;
    }

    snprintf(month_str, sizeof(month_str), "%04d-%02d", opts.month_year, opts.month_month);
    snprintf(from_date, sizeof(from_date), "%04d-%02d-01", opts.month_year, opts.month_month);
    snprintf(to_date, sizeof(to_date), "%04d-%02d-%02d", opts.month_year, opts.month_month,
             days_in_month(opts.month_year, opts.month_month));

    if (http_global_init() != 0) {
        fprintf(stderr, "error: could not initialise HTTP client\n");
        return EXIT_ERROR;
    }

    if (blikk_authenticate(opts.base_url, opts.app_id, opts.app_secret,
                            token, sizeof(token), err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        http_global_cleanup();
        return EXIT_ERROR;
    }

    if (blikk_fetch_month_timereports(opts.base_url, token, from_date, to_date,
                                       opts.user_ids_csv[0] != '\0' ? opts.user_ids_csv : NULL,
                                       &list, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        http_global_cleanup();
        return EXIT_ERROR;
    }

    http_global_cleanup();

    if (report_build_summary(&list, &summary) != 0) {
        fprintf(stderr, "error: out of memory while summarising results\n");
        blikk_timereport_list_free(&list);
        return EXIT_ERROR;
    }

    if (opts.text_format) {
        report_print_text(stdout, &summary, &list, month_str, from_date, to_date);
    } else {
        char generated_at[32];
        struct tm tm_utc;
        time_t t = time(NULL);
        gmtime_r(&t, &tm_utc);
        strftime(generated_at, sizeof(generated_at), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

        cJSON *json = report_to_json(&summary, &list, month_str, from_date, to_date, generated_at);
        char *printed = cJSON_Print(json);
        if (printed != NULL) {
            printf("%s\n", printed);
            free(printed);
        }
        cJSON_Delete(json);
    }

    exit_code = report_all_attested(&summary) ? EXIT_OK : EXIT_PENDING;

    report_summary_free(&summary);
    blikk_timereport_list_free(&list);
    return exit_code;
}
