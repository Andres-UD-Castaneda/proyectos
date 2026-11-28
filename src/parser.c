#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

static void trim_copy(char *dst, size_t dst_size, const char *src, size_t len) {
    if (!dst || dst_size == 0) {
        return;
    }
    while (len > 0 && isspace((unsigned char)*src)) {
        src++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void strip_query_string(char *url) {
    if (!url) {
        return;
    }
    char *q = strchr(url, '?');
    if (q) {
        *q = '\0';
    }
}

void parse_log_line(const char *line, LogEntry *entry) {
    if (!entry) {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->response_time = -1.0;
    entry->valid = false;

    if (!line || *line == '\0') {
        return;
    }

    /* 1. IP al inicio */
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    const char *ip_start = p;
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    size_t ip_len = (size_t)(p - ip_start);
    if (ip_len == 0 || ip_len >= MAX_IP_LEN) {
        return;
    }
    trim_copy(entry->ip, sizeof(entry->ip), ip_start, ip_len);

    /* 2. Timestamp entre '[' y ']' */
    const char *ts_open = strchr(p, '[');
    if (!ts_open) {
        return;
    }
    const char *ts_close = strchr(ts_open + 1, ']');
    if (!ts_close || ts_close == ts_open + 1) {
        return;
    }
    size_t ts_len = (size_t)(ts_close - (ts_open + 1));
    if (ts_len >= sizeof(entry->timestamp)) {
        return;
    }
    trim_copy(entry->timestamp, sizeof(entry->timestamp), ts_open + 1, ts_len);

    /* 3. Petición entre comillas "..." -> METHOD URL [PROTOCOL] */
    const char *req_open = strchr(ts_close + 1, '"');
    if (!req_open) {
        return;
    }
    const char *req_close = strchr(req_open + 1, '"');
    if (!req_close || req_close == req_open + 1) {
        return;
    }

    size_t req_len = (size_t)(req_close - (req_open + 1));
    char req_buf[MAX_LINE_LEN];
    if (req_len >= sizeof(req_buf)) {
        return;
    }
    memcpy(req_buf, req_open + 1, req_len);
    req_buf[req_len] = '\0';

    char method_tmp[16] = {0};
    char url_tmp[MAX_URL_LEN] = {0};
    char proto_tmp[32] = {0};

    int req_parts = sscanf(req_buf, "%15s %255s %31s", method_tmp, url_tmp, proto_tmp);
    if (req_parts < 2 || method_tmp[0] == '\0' || url_tmp[0] == '\0') {
        return;
    }

    trim_copy(entry->method, sizeof(entry->method), method_tmp, strlen(method_tmp));
    trim_copy(entry->url, sizeof(entry->url), url_tmp, strlen(url_tmp));
    strip_query_string(entry->url);

    /* 4. Después de la petición: status_code y bytes */
    const char *after_req = req_close + 1;
    char bytes_tok[32] = {0};
    int status_val = 0;
    int consumed = 0;

    if (sscanf(after_req, " %d %31s%n", &status_val, bytes_tok, &consumed) < 2) {
        return;
    }
    if (status_val < 100 || status_val > 599) {
        return;
    }
    entry->status_code = status_val;

    if (strcmp(bytes_tok, "-") == 0) {
        entry->bytes = 0;
    } else {
        char *end_b = NULL;
        errno = 0;
        unsigned long long b_val = strtoull(bytes_tok, &end_b, 10);
        if (errno != 0 || end_b == bytes_tok || *end_b != '\0') {
            return;
        }
        entry->bytes = (uint64_t)b_val;
    }

    /* 5. Campos opcionales al final: referer, user-agent y response_time opcional */
    const char *tail = after_req + consumed;
    /* Saltar referer entre comillas si existe */
    const char *ref_q1 = strchr(tail, '"');
    if (ref_q1) {
        const char *ref_q2 = strchr(ref_q1 + 1, '"');
        if (ref_q2) {
            /* Saltar user-agent entre comillas si existe */
            const char *ua_q1 = strchr(ref_q2 + 1, '"');
            if (ua_q1) {
                const char *ua_q2 = strchr(ua_q1 + 1, '"');
                if (ua_q2) {
                    const char *rt_ptr = ua_q2 + 1;
                    while (*rt_ptr && isspace((unsigned char)*rt_ptr)) {
                        rt_ptr++;
                    }
                    if (*rt_ptr != '\0') {
                        char *end_rt = NULL;
                        errno = 0;
                        double rt_val = strtod(rt_ptr, &end_rt);
                        if (errno == 0 && end_rt != rt_ptr && rt_val >= 0.0) {
                            entry->response_time = rt_val;
                        }
                    }
                }
            }
        }
    }

    entry->valid = true;
}
