#ifndef BLIKK_REPORT_H
#define BLIKK_REPORT_H

#include <stddef.h>
#include <stdio.h>

#include "blikk_api.h"
#include "../third_party/cJSON.h"

typedef struct {
    long user_id;
    char user_name[128];
    long total_reports;
    double total_hours;
    long not_sent_to_attest;   /* never submitted for attestation */
    long sent_awaiting_attest; /* submitted, waiting on the approver */
    long attested;             /* approved */
} report_user_summary_t;

typedef struct {
    report_user_summary_t *users;
    size_t user_count;
    size_t user_capacity;

    long total_reports;
    double total_hours;
    long not_sent_to_attest;
    long sent_awaiting_attest;
    long attested;
} report_summary_t;

/* Aggregates `list` per user and overall. Returns 0, or -1 on OOM. */
int report_build_summary(const blikk_timereport_list_t *list, report_summary_t *out);
void report_summary_free(report_summary_t *s);

/* true if every time report in the period has been attested (and there is
 * at least one report - an empty period is not reported as "all attested"). */
int report_all_attested(const report_summary_t *s);

/*
 * Builds the JSON representation of the result (summary + per-user
 * breakdown + the list of not-yet-attested reports). Caller owns the
 * returned cJSON tree (cJSON_Delete).
 */
cJSON *report_to_json(const report_summary_t *s, const blikk_timereport_list_t *list,
                       const char *month, const char *from_date, const char *to_date,
                       const char *generated_at);

void report_print_text(FILE *f, const report_summary_t *s, const blikk_timereport_list_t *list,
                        const char *month, const char *from_date, const char *to_date);

#endif /* BLIKK_REPORT_H */
