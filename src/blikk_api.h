#ifndef BLIKK_API_H
#define BLIKK_API_H

#include <stddef.h>

#define BLIKK_DEFAULT_BASE_URL "https://publicapi.blikk.com"

/* One row of GET /v1/Core/TimeReports, reduced to the fields this tool
 * needs. See https://publicapidocs.blikk.com/#core-resources-timereports-list
 */
typedef struct {
    long id;
    char date[11];        /* "YYYY-MM-DD" */
    double hours;
    long user_id;
    char user_name[128];
    /* Derived from sentToAttestDate / attestedDate being non-null: the
     * public API exposes filter.isSentToAttest / filter.isAttested as
     * booleans but the list response itself only carries the two dates. */
    int sent_to_attest;
    int attested;
} blikk_timereport_t;

typedef struct {
    blikk_timereport_t *items;
    size_t count;
    size_t capacity;
} blikk_timereport_list_t;

/*
 * Exchange an application id/secret for a bearer access token (POST
 * /v1/Auth/Token, Basic auth). On success returns 0 and writes a
 * NUL-terminated token into token_out. On failure returns -1 and writes a
 * human-readable message into err.
 */
int blikk_authenticate(const char *base_url, const char *app_id, const char *app_secret,
                        char *token_out, size_t token_out_size,
                        char *err, size_t err_size);

/*
 * Fetch every time report in [from_date, to_date] (inclusive, "YYYY-MM-DD"),
 * optionally restricted to user_ids_csv (comma-separated Blikk user ids, or
 * NULL/"" for every user the token can see). Paginates internally and
 * retries once on HTTP 429 per page, honouring Retry-After.
 *
 * On success returns 0 and *out is populated (caller must call
 * blikk_timereport_list_free). On failure returns -1 and writes a message
 * into err.
 */
int blikk_fetch_month_timereports(const char *base_url, const char *token,
                                   const char *from_date, const char *to_date,
                                   const char *user_ids_csv,
                                   blikk_timereport_list_t *out,
                                   char *err, size_t err_size);

void blikk_timereport_list_free(blikk_timereport_list_t *list);

#endif /* BLIKK_API_H */
