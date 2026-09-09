#include "http_client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp (POSIX) */

/* Deterministic, finite timeouts: this tool is meant to run unattended in
 * automation flows, so a stuck TCP connection must never hang the caller. */
#define HTTP_CONNECT_TIMEOUT_SECONDS 15L
#define HTTP_TOTAL_TIMEOUT_SECONDS   30L

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_buffer_t *buf = (http_buffer_t *)userdata;
    const size_t add = size * nmemb;
    char *grown;

    if (add == 0) {
        return 0;
    }

    grown = (char *)realloc(buf->data, buf->len + add + 1);
    if (grown == NULL) {
        return 0; /* signals an error to libcurl, aborts the transfer */
    }

    memcpy(grown + buf->len, ptr, add);
    buf->len += add;
    grown[buf->len] = '\0';
    buf->data = grown;

    return add;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    http_response_t *resp = (http_response_t *)userdata;
    const size_t total = size * nitems;
    static const char prefix[] = "Retry-After:";

    if (total >= sizeof(prefix) - 1 &&
        strncasecmp(buffer, prefix, sizeof(prefix) - 1) == 0) {
        const char *value = buffer + sizeof(prefix) - 1;
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        resp->retry_after_seconds = strtol(value, NULL, 10);
    }

    return total;
}

int http_global_init(void)
{
    return (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) ? 0 : -1;
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

static int perform_request(CURL *curl, struct curl_slist *headers,
                            http_response_t *resp, char *err, size_t err_size)
{
    CURLcode rc;

    memset(resp, 0, sizeof(*resp));
    resp->retry_after_seconds = -1;

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp->body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, HTTP_CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_TOTAL_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    /* Never relax TLS verification: this ships credentials in headers. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "blikk-attest-check/1.0");

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "%s", curl_easy_strerror(rc));
        }
        free(resp->body.data);
        resp->body.data = NULL;
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
    return 0;
}

int http_get(const char *url, const char *bearer_token,
             http_response_t *resp, char *err, size_t err_size)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    char auth_header[2048];
    int result;

    curl = curl_easy_init();
    if (curl == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "curl_easy_init() failed");
        }
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    if (bearer_token != NULL) {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", bearer_token);
        headers = curl_slist_append(headers, auth_header);
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    result = perform_request(curl, headers, resp, err, err_size);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

int http_post_basic_auth(const char *url, const char *basic_b64,
                          http_response_t *resp, char *err, size_t err_size)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    char auth_header[2048];
    int result;

    curl = curl_easy_init();
    if (curl == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "curl_easy_init() failed");
        }
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);

    snprintf(auth_header, sizeof(auth_header), "Authorization: Basic %s", basic_b64);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Accept: application/json");

    result = perform_request(curl, headers, resp, err, err_size);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

void http_response_free(http_response_t *resp)
{
    if (resp == NULL) {
        return;
    }
    free(resp->body.data);
    resp->body.data = NULL;
    resp->body.len = 0;
}
