#include "report.h"

#include <stdlib.h>
#include <string.h>

static report_user_summary_t *find_or_create_user(report_summary_t *s, long user_id,
                                                    const char *user_name)
{
    size_t i;

    for (i = 0; i < s->user_count; i++) {
        if (s->users[i].user_id == user_id) {
            return &s->users[i];
        }
    }

    if (s->user_count == s->user_capacity) {
        const size_t new_capacity = (s->user_capacity == 0) ? 8 : s->user_capacity * 2;
        report_user_summary_t *grown =
            (report_user_summary_t *)realloc(s->users, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        s->users = grown;
        s->user_capacity = new_capacity;
    }

    report_user_summary_t *u = &s->users[s->user_count++];
    memset(u, 0, sizeof(*u));
    u->user_id = user_id;
    snprintf(u->user_name, sizeof(u->user_name), "%s", user_name);
    return u;
}

int report_build_summary(const blikk_timereport_list_t *list, report_summary_t *out)
{
    size_t i;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < list->count; i++) {
        const blikk_timereport_t *tr = &list->items[i];
        report_user_summary_t *u = find_or_create_user(out, tr->user_id, tr->user_name);
        if (u == NULL) {
            report_summary_free(out);
            return -1;
        }

        u->total_reports++;
        u->total_hours += tr->hours;
        out->total_reports++;
        out->total_hours += tr->hours;

        if (tr->attested) {
            u->attested++;
            out->attested++;
        } else if (tr->sent_to_attest) {
            u->sent_awaiting_attest++;
            out->sent_awaiting_attest++;
        } else {
            u->not_sent_to_attest++;
            out->not_sent_to_attest++;
        }
    }

    return 0;
}

void report_summary_free(report_summary_t *s)
{
    if (s == NULL) {
        return;
    }
    free(s->users);
    s->users = NULL;
    s->user_count = 0;
    s->user_capacity = 0;
}

int report_all_attested(const report_summary_t *s)
{
    return s->total_reports > 0 && s->attested == s->total_reports;
}

static const char *pending_status(const blikk_timereport_t *tr)
{
    if (tr->attested) {
        return "attested";
    }
    if (tr->sent_to_attest) {
        return "sentAwaitingAttest";
    }
    return "notSentToAttest";
}

static cJSON *user_summary_to_json(const report_user_summary_t *u)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "userId", (double)u->user_id);
    cJSON_AddStringToObject(obj, "userName", u->user_name);
    cJSON_AddNumberToObject(obj, "totalReports", (double)u->total_reports);
    cJSON_AddNumberToObject(obj, "totalHours", u->total_hours);
    cJSON_AddNumberToObject(obj, "notSentToAttest", (double)u->not_sent_to_attest);
    cJSON_AddNumberToObject(obj, "sentAwaitingAttest", (double)u->sent_awaiting_attest);
    cJSON_AddNumberToObject(obj, "attested", (double)u->attested);
    cJSON_AddBoolToObject(obj, "allAttested",
                           u->total_reports > 0 && u->attested == u->total_reports);
    return obj;
}

cJSON *report_to_json(const report_summary_t *s, const blikk_timereport_list_t *list,
                       const char *month, const char *from_date, const char *to_date,
                       const char *generated_at)
{
    size_t i;
    cJSON *root = cJSON_CreateObject();
    cJSON *users_arr = cJSON_CreateArray();
    cJSON *pending_arr = cJSON_CreateArray();

    cJSON_AddStringToObject(root, "objectName", "blikkAttestCheck.summary");
    cJSON_AddStringToObject(root, "generatedAt", generated_at);
    cJSON_AddStringToObject(root, "month", month);
    cJSON_AddStringToObject(root, "from", from_date);
    cJSON_AddStringToObject(root, "to", to_date);
    cJSON_AddNumberToObject(root, "totalReports", (double)s->total_reports);
    cJSON_AddNumberToObject(root, "totalHours", s->total_hours);
    cJSON_AddNumberToObject(root, "notSentToAttest", (double)s->not_sent_to_attest);
    cJSON_AddNumberToObject(root, "sentAwaitingAttest", (double)s->sent_awaiting_attest);
    cJSON_AddNumberToObject(root, "attested", (double)s->attested);
    cJSON_AddBoolToObject(root, "allAttested", report_all_attested(s));

    for (i = 0; i < s->user_count; i++) {
        cJSON_AddItemToArray(users_arr, user_summary_to_json(&s->users[i]));
    }
    cJSON_AddItemToObject(root, "users", users_arr);

    for (i = 0; i < list->count; i++) {
        const blikk_timereport_t *tr = &list->items[i];
        if (tr->attested) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (double)tr->id);
        cJSON_AddStringToObject(item, "date", tr->date);
        cJSON_AddNumberToObject(item, "userId", (double)tr->user_id);
        cJSON_AddStringToObject(item, "userName", tr->user_name);
        cJSON_AddNumberToObject(item, "hours", tr->hours);
        cJSON_AddStringToObject(item, "status", pending_status(tr));
        cJSON_AddItemToArray(pending_arr, item);
    }
    cJSON_AddItemToObject(root, "pendingReports", pending_arr);

    return root;
}

void report_print_text(FILE *f, const report_summary_t *s, const blikk_timereport_list_t *list,
                        const char *month, const char *from_date, const char *to_date)
{
    size_t i;

    fprintf(f, "Blikk attestation check for %s (%s - %s)\n", month, from_date, to_date);
    fprintf(f, "  Total time reports : %ld (%.2f h)\n", s->total_reports, s->total_hours);
    fprintf(f, "  Attested           : %ld\n", s->attested);
    fprintf(f, "  Awaiting attest    : %ld\n", s->sent_awaiting_attest);
    fprintf(f, "  Not sent to attest : %ld\n", s->not_sent_to_attest);
    fprintf(f, "  All attested       : %s\n", report_all_attested(s) ? "yes" : "no");

    if (s->user_count > 1) {
        fprintf(f, "\nPer user:\n");
        for (i = 0; i < s->user_count; i++) {
            const report_user_summary_t *u = &s->users[i];
            fprintf(f, "  - %s (id %ld): %ld reports, %ld attested, %ld awaiting, %ld not sent\n",
                    u->user_name, u->user_id, u->total_reports, u->attested,
                    u->sent_awaiting_attest, u->not_sent_to_attest);
        }
    }

    if (s->not_sent_to_attest + s->sent_awaiting_attest > 0) {
        fprintf(f, "\nNot yet attested:\n");
        for (i = 0; i < list->count; i++) {
            const blikk_timereport_t *tr = &list->items[i];
            if (tr->attested) {
                continue;
            }
            fprintf(f, "  - %s  %-24s  %5.2f h  [%s]\n",
                    tr->date, tr->user_name, tr->hours, pending_status(tr));
        }
    }
}
