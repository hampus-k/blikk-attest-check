#include "blikk_api.h"
#include "http_client.h"
#include "base64.h"

#include "../third_party/cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* sleep() */

#define BLIKK_PAGE_SIZE       100
#define BLIKK_MAX_PAGES       500  /* safety cap: 50000 reports is far beyond one month */
#define BLIKK_MAX_429_RETRIES 5
#define BLIKK_DEFAULT_RETRY_AFTER_SECONDS 2

static void set_err(char *err, size_t err_size, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || err_size == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

/* Copies up to (n-1) chars of a cJSON string field into dst, NUL-terminated.
 * Leaves dst untouched (caller must have zero-initialised it) if the field
 * is absent, null, or not a string. */
static void copy_string_field(const cJSON *obj, const char *key, char *dst, size_t n)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(field) && field->valuestring != NULL) {
        snprintf(dst, n, "%s", field->valuestring);
    }
}

static int is_string_field_present(const cJSON *obj, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(field) && field->valuestring != NULL && field->valuestring[0] != '\0';
}

static double number_field(const cJSON *obj, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(field) ? field->valuedouble : 0.0;
}

int blikk_authenticate(const char *base_url, const char *app_id, const char *app_secret,
                        char *token_out, size_t token_out_size,
                        char *err, size_t err_size)
{
    char url[512];
    char raw[512];
    char b64[BASE64_ENCODED_SIZE(sizeof(raw))];
    http_response_t resp;
    cJSON *root = NULL;
    const cJSON *token_field;
    int rc = -1;

    snprintf(url, sizeof(url), "%s/v1/Auth/Token", base_url);
    snprintf(raw, sizeof(raw), "%s:%s", app_id, app_secret);

    if (base64_encode((const unsigned char *)raw, strlen(raw), b64, sizeof(b64)) == (size_t)-1) {
        set_err(err, err_size, "application id/secret too long to encode");
        return -1;
    }

    if (http_post_basic_auth(url, b64, &resp, err, err_size) != 0) {
        return -1;
    }

    if (resp.status_code == 401) {
        set_err(err, err_size, "authentication rejected (401) - check BLIKK_APP_ID / BLIKK_APP_SECRET");
        goto done;
    }
    if (resp.status_code != 200) {
        set_err(err, err_size, "POST /v1/Auth/Token failed with HTTP %ld: %s",
                resp.status_code, resp.body.data != NULL ? resp.body.data : "(empty body)");
        goto done;
    }

    root = cJSON_Parse(resp.body.data);
    if (root == NULL) {
        set_err(err, err_size, "could not parse auth response as JSON");
        goto done;
    }

    token_field = cJSON_GetObjectItemCaseSensitive(root, "accessToken");
    if (!cJSON_IsString(token_field) || token_field->valuestring == NULL) {
        set_err(err, err_size, "auth response did not contain an accessToken");
        goto done;
    }

    snprintf(token_out, token_out_size, "%s", token_field->valuestring);
    rc = 0;

done:
    cJSON_Delete(root);
    http_response_free(&resp);
    return rc;
}

static int list_push(blikk_timereport_list_t *list, const blikk_timereport_t *item)
{
    if (list->count == list->capacity) {
        const size_t new_capacity = (list->capacity == 0) ? 32 : list->capacity * 2;
        blikk_timereport_t *grown =
            (blikk_timereport_t *)realloc(list->items, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = *item;
    return 0;
}

/* Appends "&filter.userIds=<id>" once per id in a comma-separated list, the
 * query-array convention used by ASP.NET Web API model binding (Blikk's
 * public API is not explicit about this in its docs). */
static void append_user_id_filters(char *url, size_t url_size, const char *user_ids_csv)
{
    char buf[1024];
    char *save = NULL;
    char *tok;

    if (user_ids_csv == NULL || user_ids_csv[0] == '\0') {
        return;
    }

    snprintf(buf, sizeof(buf), "%s", user_ids_csv);
    for (tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') {
            tok++;
        }
        if (*tok == '\0') {
            continue;
        }
        const size_t len = strlen(url);
        snprintf(url + len, (url_size > len) ? url_size - len : 0, "&filter.userIds=%s", tok);
    }
}

int blikk_fetch_month_timereports(const char *base_url, const char *token,
                                   const char *from_date, const char *to_date,
                                   const char *user_ids_csv,
                                   blikk_timereport_list_t *out,
                                   char *err, size_t err_size)
{
    int page;
    int total_pages = 1;

    memset(out, 0, sizeof(*out));

    for (page = 1; page <= total_pages; page++) {
        char url[1536];
        http_response_t resp;
        cJSON *root = NULL;
        const cJSON *items;
        const cJSON *total_pages_field;
        const cJSON *item;
        int retries;

        if (page > BLIKK_MAX_PAGES) {
            set_err(err, err_size, "aborting after %d pages - unexpectedly large result set",
                    BLIKK_MAX_PAGES);
            blikk_timereport_list_free(out);
            return -1;
        }

        snprintf(url, sizeof(url),
                 "%s/v1/Core/TimeReports?page=%d&pageSize=%d&filter.from=%s&filter.to=%s",
                 base_url, page, BLIKK_PAGE_SIZE, from_date, to_date);
        append_user_id_filters(url, sizeof(url), user_ids_csv);

        for (retries = 0; ; retries++) {
            if (http_get(url, token, &resp, err, err_size) != 0) {
                blikk_timereport_list_free(out);
                return -1;
            }

            if (resp.status_code == 429 && retries < BLIKK_MAX_429_RETRIES) {
                const long wait = (resp.retry_after_seconds > 0)
                                       ? resp.retry_after_seconds
                                       : BLIKK_DEFAULT_RETRY_AFTER_SECONDS;
                http_response_free(&resp);
                sleep((unsigned int)wait);
                continue;
            }
            break;
        }

        if (resp.status_code != 200) {
            set_err(err, err_size, "GET /v1/Core/TimeReports (page %d) failed with HTTP %ld: %s",
                    page, resp.status_code,
                    resp.body.data != NULL ? resp.body.data : "(empty body)");
            http_response_free(&resp);
            blikk_timereport_list_free(out);
            return -1;
        }

        root = cJSON_Parse(resp.body.data);
        if (root == NULL) {
            set_err(err, err_size, "could not parse time reports response as JSON (page %d)", page);
            http_response_free(&resp);
            blikk_timereport_list_free(out);
            return -1;
        }

        total_pages_field = cJSON_GetObjectItemCaseSensitive(root, "totalPages");
        if (cJSON_IsNumber(total_pages_field) && total_pages_field->valueint > 0) {
            total_pages = total_pages_field->valueint;
        }

        items = cJSON_GetObjectItemCaseSensitive(root, "items");
        cJSON_ArrayForEach(item, items) {
            blikk_timereport_t tr;
            const cJSON *user;

            memset(&tr, 0, sizeof(tr));
            tr.id = (long)number_field(item, "id");
            copy_string_field(item, "date", tr.date, sizeof(tr.date));
            tr.hours = number_field(item, "hours");
            tr.sent_to_attest = is_string_field_present(item, "sentToAttestDate");
            tr.attested = is_string_field_present(item, "attestedDate");

            user = cJSON_GetObjectItemCaseSensitive(item, "user");
            if (cJSON_IsObject(user)) {
                tr.user_id = (long)number_field(user, "id");
                copy_string_field(user, "name", tr.user_name, sizeof(tr.user_name));
            }

            if (list_push(out, &tr) != 0) {
                set_err(err, err_size, "out of memory while collecting time reports");
                cJSON_Delete(root);
                http_response_free(&resp);
                blikk_timereport_list_free(out);
                return -1;
            }
        }

        cJSON_Delete(root);
        http_response_free(&resp);
    }

    return 0;
}

void blikk_timereport_list_free(blikk_timereport_list_t *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}
