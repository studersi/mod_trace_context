/*
 * Copyright 2026 Your Name
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <strings.h>

#include "apr_general.h"
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_tables.h"

#include "ap_expr.h"
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"

module AP_MODULE_DECLARE_DATA trace_context_module;

#define TRACE_CONTEXT_TRACEPARENT_LEN 55
#define TRACE_CONTEXT_TRACE_ID_LEN 32
#define TRACE_CONTEXT_PARENT_ID_LEN 16
#define TRACE_CONTEXT_TRACE_FLAG_SAMPLED 0x01
#define TRACE_CONTEXT_SUPPORTED_VERSION 0x00
#define TRACE_CONTEXT_TRACESTATE_MAX_LEN 512
#define TRACE_CONTEXT_TRACESTATE_MAX_MEMBERS 32
#define TRACE_CONTEXT_TRACESTATE_MEMBER_MAX_LEN 256
#define TRACE_CONTEXT_TRACESTATE_MEMBER_NAME_MAX_LEN (TRACE_CONTEXT_TRACESTATE_MEMBER_MAX_LEN - 1)
#define TRACE_CONTEXT_DEFAULT_TRACESTATE_MEMBER_NAME "apache"
#define TRACE_CONTEXT_NOTE_TRACE_ID "tc-trace-id"
#define TRACE_CONTEXT_NOTE_PARENT_ID "tc-parent-id"
#define TRACE_CONTEXT_NOTE_TRACEPARENT "tc-traceparent"
#define TRACE_CONTEXT_NOTE_TRACESTATE "tc-tracestate"
#define TRACE_CONTEXT_NOTE_SAMPLED "tc-sampled"
#define TRACE_CONTEXT_ENV_UNIQUE_ID "UNIQUE_ID"
#define TRACE_CONTEXT_ENV_UNIQUE_ID_ORIG "UNIQUE_ID_ORIG"

enum trace_context_header_mask {
    TRACE_CONTEXT_HEADER_NONE = 0,
    TRACE_CONTEXT_HEADER_TRACEPARENT = 1 << 0,
    TRACE_CONTEXT_HEADER_TRACESTATE = 1 << 1,
    TRACE_CONTEXT_HEADER_BOTH = TRACE_CONTEXT_HEADER_TRACEPARENT | TRACE_CONTEXT_HEADER_TRACESTATE
};

enum trace_context_replace_unique_id_mode {
    TRACE_CONTEXT_REPLACE_UNIQUE_ID_OFF = 0,
    TRACE_CONTEXT_REPLACE_UNIQUE_ID_TRACE_ID,
    TRACE_CONTEXT_REPLACE_UNIQUE_ID_PARENT_ID,
    TRACE_CONTEXT_REPLACE_UNIQUE_ID_BOTH
};

typedef struct trace_context_dir_conf_t {
    int enabled;
    int enabled_set;

    int request_headers;
    int request_headers_set;

    int response_headers;
    int response_headers_set;

    ap_expr_info_t *sample;
    int sample_all;
    int sample_set;

    ap_expr_info_t *sample_allow;
    int sample_allow_all;
    int sample_allow_set;

    ap_expr_info_t *continue_trace_allow;
    int continue_trace_allow_all;
    int continue_trace_allow_set;

    const char *tracestate_member_name;
    int tracestate_member_name_set;

    int replace_unique_id;
    int replace_unique_id_set;
} trace_context_dir_conf_t;

typedef struct trace_context_request_ctx_t {
    char trace_id[TRACE_CONTEXT_TRACE_ID_LEN + 1];
    char parent_id[TRACE_CONTEXT_PARENT_ID_LEN + 1];
    unsigned int trace_flags;
    const char *traceparent;
    const char *tracestate;
} trace_context_request_ctx_t;

/* Create per-directory configuration with module defaults. */
static void *trace_context_create_dir_config(apr_pool_t *p, char *dir)
{
    trace_context_dir_conf_t *conf = apr_pcalloc(p, sizeof(*conf));

    (void)dir;

    conf->enabled = 1;
    conf->enabled_set = 0;

    conf->request_headers = TRACE_CONTEXT_HEADER_BOTH;
    conf->request_headers_set = 0;

    conf->response_headers = TRACE_CONTEXT_HEADER_NONE;
    conf->response_headers_set = 0;

    conf->sample = NULL;
    conf->sample_all = 0;
    conf->sample_set = 0;

    conf->sample_allow = NULL;
    conf->sample_allow_all = 0;
    conf->sample_allow_set = 0;

    conf->continue_trace_allow = NULL;
    conf->continue_trace_allow_all = 0;
    conf->continue_trace_allow_set = 0;

    conf->tracestate_member_name = TRACE_CONTEXT_DEFAULT_TRACESTATE_MEMBER_NAME;
    conf->tracestate_member_name_set = 0;

    conf->replace_unique_id = TRACE_CONTEXT_REPLACE_UNIQUE_ID_OFF;
    conf->replace_unique_id_set = 0;

    return conf;
}

/* Merge parent and child per-directory configuration values. */
static void *trace_context_merge_dir_config(apr_pool_t *p, void *basev,
                                            void *addv)
{
    trace_context_dir_conf_t *base = basev;
    trace_context_dir_conf_t *add = addv;
    trace_context_dir_conf_t *conf = apr_pcalloc(p, sizeof(*conf));

    conf->enabled = add->enabled_set ? add->enabled : base->enabled;
    conf->enabled_set = add->enabled_set || base->enabled_set;

    conf->request_headers = add->request_headers_set ? add->request_headers
                                                     : base->request_headers;
    conf->request_headers_set = add->request_headers_set || base->request_headers_set;

    conf->response_headers = add->response_headers_set ? add->response_headers
                                                       : base->response_headers;
    conf->response_headers_set = add->response_headers_set || base->response_headers_set;

    conf->sample = add->sample_set ? add->sample : base->sample;
    conf->sample_all = add->sample_set ? add->sample_all : base->sample_all;
    conf->sample_set = add->sample_set || base->sample_set;

    conf->sample_allow = add->sample_allow_set ? add->sample_allow
                                               : base->sample_allow;
    conf->sample_allow_all = add->sample_allow_set ? add->sample_allow_all
                                                   : base->sample_allow_all;
    conf->sample_allow_set = add->sample_allow_set || base->sample_allow_set;

    conf->continue_trace_allow = add->continue_trace_allow_set
                                     ? add->continue_trace_allow
                                     : base->continue_trace_allow;
    conf->continue_trace_allow_all = add->continue_trace_allow_set
                                         ? add->continue_trace_allow_all
                                         : base->continue_trace_allow_all;
    conf->continue_trace_allow_set = add->continue_trace_allow_set || base->continue_trace_allow_set;

    conf->tracestate_member_name = add->tracestate_member_name_set
                                       ? add->tracestate_member_name
                                       : base->tracestate_member_name;
    conf->tracestate_member_name_set = add->tracestate_member_name_set || base->tracestate_member_name_set;

    conf->replace_unique_id = add->replace_unique_id_set
                                  ? add->replace_unique_id
                                  : base->replace_unique_id;
    conf->replace_unique_id_set = add->replace_unique_id_set || base->replace_unique_id_set;

    return conf;
}

/* Convert a single hexadecimal character to its numeric nibble value. */
static int trace_context_hex_nibble(int ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    ch = apr_tolower(ch);
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }

    return -1;
}

/* Parse two hexadecimal characters into a single byte value. */
static int trace_context_hex_byte(const char *hex, unsigned int *value)
{
    int high = trace_context_hex_nibble((unsigned char)hex[0]);
    int low = trace_context_hex_nibble((unsigned char)hex[1]);

    if (high < 0 || low < 0) {
        return 0;
    }

    *value = ((unsigned int)high << 4) | (unsigned int)low;

    return 1;
}

/* Return non-zero if the character is a lowercase hexadecimal digit. */
static int trace_context_is_hex_lower(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

/* Return non-zero if all characters in a buffer are the digit '0'. */
static int trace_context_all_zeros(const char *value, apr_size_t len)
{
    apr_size_t i;

    for (i = 0; i < len; ++i) {
        if (value[i] != '0') {
            return 0;
        }
    }

    return 1;
}

/* Encode binary bytes into lowercase hexadecimal text. */
static void trace_context_hex_encode(char *dst, const unsigned char *src,
                                     apr_size_t len)
{
    static const char hex[] = "0123456789abcdef";
    apr_size_t i;

    for (i = 0; i < len; ++i) {
        dst[2 * i] = hex[(src[i] >> 4) & 0x0f];
        dst[2 * i + 1] = hex[src[i] & 0x0f];
    }

    dst[2 * len] = '\0';
}

/* Generate a non-zero random W3C trace-id. */
static int trace_context_random_trace_id(char trace_id[TRACE_CONTEXT_TRACE_ID_LEN + 1])
{
    unsigned char raw[16];
    apr_status_t rv;

    do {
        rv = apr_generate_random_bytes(raw, sizeof(raw));
        if (rv != APR_SUCCESS) {
            return 0;
        }
        trace_context_hex_encode(trace_id, raw, sizeof(raw));
    } while (trace_context_all_zeros(trace_id, TRACE_CONTEXT_TRACE_ID_LEN));

    return 1;
}

/* Generate a non-zero random W3C parent-id. */
static int trace_context_random_parent_id(char parent_id[TRACE_CONTEXT_PARENT_ID_LEN + 1])
{
    unsigned char raw[8];
    apr_status_t rv;

    do {
        rv = apr_generate_random_bytes(raw, sizeof(raw));
        if (rv != APR_SUCCESS) {
            return 0;
        }
        trace_context_hex_encode(parent_id, raw, sizeof(raw));
    } while (trace_context_all_zeros(parent_id, TRACE_CONTEXT_PARENT_ID_LEN));

    return 1;
}

/* Validate and parse a W3C traceparent header into context fields. */
static int trace_context_parse_traceparent(const char *value,
                                           char trace_id[TRACE_CONTEXT_TRACE_ID_LEN + 1],
                                           char parent_id[TRACE_CONTEXT_PARENT_ID_LEN + 1],
                                           unsigned int *trace_flags)
{
    apr_size_t i;
    unsigned int version;

    if (value == NULL || strlen(value) != TRACE_CONTEXT_TRACEPARENT_LEN) {
        return 0;
    }

    if (value[2] != '-' || value[35] != '-' || value[52] != '-') {
        return 0;
    }

    if (!trace_context_is_hex_lower((unsigned char)value[0]) ||
        !trace_context_is_hex_lower((unsigned char)value[1]) ||
        !trace_context_is_hex_lower((unsigned char)value[53]) ||
        !trace_context_is_hex_lower((unsigned char)value[54])) {
        return 0;
    }

    if (!trace_context_hex_byte(value, &version) ||
        version != TRACE_CONTEXT_SUPPORTED_VERSION) {
        return 0;
    }

    for (i = 3; i < 35; ++i) {
        if (!trace_context_is_hex_lower((unsigned char)value[i])) {
            return 0;
        }
        trace_id[i - 3] = value[i];
    }
    trace_id[TRACE_CONTEXT_TRACE_ID_LEN] = '\0';

    for (i = 36; i < 52; ++i) {
        if (!trace_context_is_hex_lower((unsigned char)value[i])) {
            return 0;
        }
        parent_id[i - 36] = value[i];
    }
    parent_id[TRACE_CONTEXT_PARENT_ID_LEN] = '\0';

    if (trace_context_all_zeros(trace_id, TRACE_CONTEXT_TRACE_ID_LEN) ||
        trace_context_all_zeros(parent_id, TRACE_CONTEXT_PARENT_ID_LEN)) {
        return 0;
    }

    if (!trace_context_hex_byte(value + 53, trace_flags)) {
        return 0;
    }

    return 1;
}

/* Build a W3C traceparent header value from context fields. */
static const char *trace_context_build_traceparent(apr_pool_t *p,
                                                   const char *trace_id,
                                                   const char *parent_id,
                                                   unsigned int trace_flags)
{
    return apr_psprintf(p, "00-%s-%s-%02x", trace_id, parent_id,
                        trace_flags & 0xff);
}

/* Return non-zero if the character is optional whitespace (OWS). */
static int trace_context_is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/* Return non-zero if a character is valid in a tracestate key token. */
static int trace_context_is_tracestate_member_name_char(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
           ch == '-' || ch == '*' || ch == '/';
}

/* Validate tracestate member key grammar for a bounded key token. */
static int trace_context_is_valid_tracestate_member_name_range(const char *name,
                                                               apr_size_t name_len)
{
    const char *at;
    apr_size_t i;

    if (name == NULL || name_len == 0) {
        return 0;
    }

    if (name_len > TRACE_CONTEXT_TRACESTATE_MEMBER_NAME_MAX_LEN) {
        return 0;
    }

    at = NULL;
    for (i = 0; i < name_len; ++i) {
        if (name[i] == '@') {
            if (at != NULL) {
                return 0;
            }
            at = name + i;
        }
    }

    if (at != NULL) {
        apr_size_t tenant_len;
        apr_size_t system_len;

        tenant_len = (apr_size_t)(at - name);
        system_len = name_len - tenant_len - 1;
        if (tenant_len == 0 || tenant_len > 241 || system_len == 0 ||
            system_len > 14) {
            return 0;
        }
    }

    if (name[0] < 'a' || name[0] > 'z') {
        return 0;
    }

    for (i = 1; i < name_len; ++i) {
        unsigned char ch = (unsigned char)name[i];

        if (ch == '@') {
            if ((i + 1) >= name_len || name[i + 1] < 'a' || name[i + 1] > 'z') {
                return 0;
            }
            continue;
        }

        if (!trace_context_is_tracestate_member_name_char(ch)) {
            return 0;
        }
    }

    return 1;
}

/* Validate a configured tracestate member key. */
static int trace_context_is_valid_tracestate_member_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }

    return trace_context_is_valid_tracestate_member_name_range(name,
                                                               strlen(name));
}

/* Return non-zero if a character is valid in a tracestate value. */
static int trace_context_is_tracestate_member_value_char(unsigned char ch)
{
    return ch >= 0x20 && ch <= 0x7e && ch != ',' && ch != '=';
}

/* Validate tracestate member value grammar for a bounded value token. */
static int trace_context_is_valid_tracestate_member_value(const char *value,
                                                          apr_size_t value_len)
{
    apr_size_t i;

    if (value == NULL || value_len == 0) {
        return 0;
    }

    if (value[0] == ' ' || value[value_len - 1] == ' ') {
        return 0;
    }

    for (i = 0; i < value_len; ++i) {
        if (!trace_context_is_tracestate_member_value_char((unsigned char)value[i])) {
            return 0;
        }
    }

    return 1;
}

/* Build a single tracestate member value (`key=value`) with length limits. */
static const char *trace_context_build_tracestate_member(apr_pool_t *p,
                                                          const char *member_name,
                                                          const char *unique_id)
{
    const char *member_value = unique_id != NULL ? unique_id : "";
    apr_size_t member_name_len = strlen(member_name);
    apr_size_t member_value_len = strlen(member_value);
    apr_size_t member_value_max_len =
        TRACE_CONTEXT_TRACESTATE_MEMBER_MAX_LEN - (member_name_len + 1);

    if (member_value_len > member_value_max_len) {
        member_value_len = member_value_max_len;
    }

    return apr_pstrcat(p, member_name, "=", apr_pstrndup(p, member_value, member_value_len), NULL);
}

/* Check whether another tracestate member can be appended within limits. */
static int trace_context_can_append_tracestate_member(apr_size_t current_len,
                                                      apr_size_t member_count,
                                                      apr_size_t member_len)
{
    if (member_count >= TRACE_CONTEXT_TRACESTATE_MAX_MEMBERS) {
        return 0;
    }

    if (member_len == 0 || member_len > TRACE_CONTEXT_TRACESTATE_MEMBER_MAX_LEN) {
        return 0;
    }

    if (current_len == 0) {
        return member_len <= TRACE_CONTEXT_TRACESTATE_MAX_LEN;
    }

    return current_len + 1 + member_len <= TRACE_CONTEXT_TRACESTATE_MAX_LEN;
}

/* Locate tracestate key and value bounds inside a `key=value` member. */
static int trace_context_tracestate_member_bounds(const char *member_start,
                                                  const char *member_end,
                                                  const char **key_start,
                                                  const char **key_end,
                                                  const char **value_start,
                                                  const char **value_end)
{
    const char *equals;

    equals = memchr(member_start,
                    '=',
                    (apr_size_t)(member_end - member_start));
    if (equals == NULL || equals == member_start || (equals + 1) == member_end) {
        return 0;
    }

    *key_start = member_start;
    *key_end = equals;
    *value_start = equals + 1;
    *value_end = member_end;

    return 1;
}

/* Return non-zero when the tracestate key already exists in the output list. */
static int trace_context_tracestate_has_member_key(const apr_array_header_t *keys,
                                                   const char *key,
                                                   apr_size_t key_len)
{
    const char *const *elts = (const char *const *)keys->elts;
    int i;

    for (i = 0; i < keys->nelts; ++i) {
        const char *existing_key = elts[i];

        if (strlen(existing_key) == key_len &&
            strncmp(existing_key, key, key_len) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Remember a tracestate member key for duplicate suppression. */
static void trace_context_tracestate_add_member_key(apr_pool_t *p,
                                                    apr_array_header_t *keys,
                                                    const char *key,
                                                    apr_size_t key_len)
{
    *(const char **)apr_array_push(keys) = apr_pstrndup(p, key, key_len);
}

/* Ensure the module's tracestate member is prepended and preserve valid peers. */
static const char *trace_context_ensure_apache_tracestate(apr_pool_t *p,
                                                          const char *tracestate,
                                                          const char *member_name,
                                                          const char *unique_id)
{
    const char *apache_member;
    apr_array_header_t *members;
    apr_array_header_t *member_keys;
    const char *cursor;
    apr_size_t tracestate_len;
    apr_size_t member_count;

    apache_member =
        trace_context_build_tracestate_member(p, member_name, unique_id);
    tracestate_len = strlen(apache_member);
    member_count = 1;

    if (tracestate == NULL || tracestate[0] == '\0') {
        return apache_member;
    }

    members = apr_array_make(p, 4, sizeof(const char *));
    member_keys = apr_array_make(p, 4, sizeof(const char *));
    trace_context_tracestate_add_member_key(p, member_keys, member_name, strlen(member_name));
    cursor = tracestate;

    while (*cursor != '\0') {
        const char *member_start;
        const char *member_end;
        const char *key_start;
        const char *key_end;
        const char *value_start;
        const char *value_end;
        apr_size_t member_len;
        apr_size_t key_len;
        apr_size_t value_len;

        while (trace_context_is_ows(*cursor)) {
            ++cursor;
        }

        member_start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            ++cursor;
        }

        member_end = cursor;
        while (member_end > member_start && trace_context_is_ows(member_end[-1])) {
            --member_end;
        }

        member_len = (apr_size_t)(member_end - member_start);
        if (!trace_context_tracestate_member_bounds(member_start,
                                                    member_end,
                                                    &key_start,
                                                    &key_end,
                                                    &value_start,
                                                    &value_end)) {
            if (*cursor == ',') {
                ++cursor;
            }
            continue;
        }

        key_len = (apr_size_t)(key_end - key_start);
        value_len = (apr_size_t)(value_end - value_start);
        if (!trace_context_is_valid_tracestate_member_name_range(key_start, key_len) ||
            !trace_context_is_valid_tracestate_member_value(value_start, value_len)) {
            if (*cursor == ',') {
                ++cursor;
            }
            continue;
        }

        if (trace_context_tracestate_has_member_key(member_keys, key_start, key_len)) {
            if (*cursor == ',') {
                ++cursor;
            }
            continue;
        }

        if (member_len > 0 && trace_context_can_append_tracestate_member(tracestate_len, member_count, member_len)) {
            *(const char **)apr_array_push(members) =
                apr_pstrndup(p, member_start, member_len);
            trace_context_tracestate_add_member_key(p, member_keys, key_start, key_len);
            tracestate_len += 1 + member_len;
            ++member_count;
        }

        if (*cursor == ',') {
            ++cursor;
        }
    }

    if (members->nelts == 0) {
        return apache_member;
    }

    return apr_pstrcat(p, apache_member, ",", apr_array_pstrcat(p, members, ','),
                       NULL);
}

/* Parse header propagation mode directives into bitmask flags. */
static const char *trace_context_parse_header_mode(const char *value, int *mode)
{
    if (value == NULL) {
        return "value must be one of: none, traceparent, tracestate, both";
    }

    if (!strcasecmp(value, "none")) {
        *mode = TRACE_CONTEXT_HEADER_NONE;
        return NULL;
    }
    if (!strcasecmp(value, "traceparent")) {
        *mode = TRACE_CONTEXT_HEADER_TRACEPARENT;
        return NULL;
    }
    if (!strcasecmp(value, "tracestate")) {
        *mode = TRACE_CONTEXT_HEADER_TRACESTATE;
        return NULL;
    }
    if (!strcasecmp(value, "both")) {
        *mode = TRACE_CONTEXT_HEADER_BOTH;
        return NULL;
    }

    return "value must be one of: none, traceparent, tracestate, both";
}

/* Parse UNIQUE_ID replacement mode. */
static const char *trace_context_parse_replace_unique_id_mode(const char *value,
                                                              int *mode)
{
    if (value == NULL) {
        return "value must be one of: off, on, trace-id, parent-id, both";
    }

    if (!strcasecmp(value, "off")) {
        *mode = TRACE_CONTEXT_REPLACE_UNIQUE_ID_OFF;
        return NULL;
    }
    if (!strcasecmp(value, "on") || !strcasecmp(value, "both")) {
        *mode = TRACE_CONTEXT_REPLACE_UNIQUE_ID_BOTH;
        return NULL;
    }
    if (!strcasecmp(value, "trace-id")) {
        *mode = TRACE_CONTEXT_REPLACE_UNIQUE_ID_TRACE_ID;
        return NULL;
    }
    if (!strcasecmp(value, "parent-id")) {
        *mode = TRACE_CONTEXT_REPLACE_UNIQUE_ID_PARENT_ID;
        return NULL;
    }

    return "value must be one of: off, on, trace-id, parent-id, both";
}

/* Convert request/response header mode bitmask to human-readable name. */
static const char *trace_context_header_mode_name(int mode)
{
    switch (mode) {
    case TRACE_CONTEXT_HEADER_NONE:
        return "none";
    case TRACE_CONTEXT_HEADER_TRACEPARENT:
        return "traceparent";
    case TRACE_CONTEXT_HEADER_TRACESTATE:
        return "tracestate";
    case TRACE_CONTEXT_HEADER_BOTH:
        return "both";
    default:
        return "unknown";
    }
}

/* Convert UNIQUE_ID replacement mode to human-readable name. */
static const char *trace_context_replace_unique_id_mode_name(int mode)
{
    switch (mode) {
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_OFF:
        return "off";
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_TRACE_ID:
        return "trace-id";
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_PARENT_ID:
        return "parent-id";
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_BOTH:
        return "both";
    default:
        return "unknown";
    }
}

/* Evaluate whether incoming sampled traces are allowed to remain sampled. */
static int trace_context_is_sampling_allowed(request_rec *r,
                                             const trace_context_dir_conf_t *conf)
{
    const char *err = NULL;
    int rc;

    if (conf->sample_allow_all) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: sampling allowed by TraceContextSampleAllowExpr=all");
        return 1;
    }

    if (conf->sample_allow == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: sampling denied (TraceContextSampleAllowExpr=none)");
        return 0;
    }

    rc = ap_expr_exec(r, conf->sample_allow, &err);
    if (err != NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "mod_trace_context: failed to evaluate TraceContextSampleAllowExpr: %s",
                      err);
        return 0;
    }

    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                  "mod_trace_context: TraceContextSampleAllowExpr evaluated to %d",
                  rc ? 1 : 0);

    return rc ? 1 : 0;
}

/* Evaluate whether this request should be proactively marked as sampled. */
static int trace_context_is_sampling_requested(request_rec *r,
                                               const trace_context_dir_conf_t *conf)
{
    const char *err = NULL;
    int rc;

    if (conf->sample_all) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: sampling requested by TraceContextSampleExpr=all");
        return 1;
    }

    if (conf->sample == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: sampling not requested (TraceContextSampleExpr=none)");
        return 0;
    }

    rc = ap_expr_exec(r, conf->sample, &err);
    if (err != NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
                      "mod_trace_context: failed to evaluate TraceContextSampleExpr: %s",
                      err);
        return 0;
    }

    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                  "mod_trace_context: TraceContextSampleExpr evaluated to %d",
                  rc ? 1 : 0);

    return rc ? 1 : 0;
}

/* Evaluate whether an incoming trace context may be continued. */
static int trace_context_is_trace_continuation_allowed(
    request_rec *r,
    const trace_context_dir_conf_t *conf)
{
    const char *err = NULL;
    int rc;

    if (conf->continue_trace_allow == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
            "mod_trace_context: trace continuation %s (TraceContextContinueTraceAllowExpr=%s)",
            conf->continue_trace_allow_all ? "allowed" : "denied",
            conf->continue_trace_allow_all ? "all" : "none");
        return conf->continue_trace_allow_all ? 1 : 0;
    }

    rc = ap_expr_exec(r, conf->continue_trace_allow, &err);
    if (err != NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
            "mod_trace_context: failed to evaluate TraceContextContinueTraceAllowExpr: %s",
            err);
        return 0;
    }

    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                  "mod_trace_context: TraceContextContinueTraceAllowExpr evaluated to %d",
                  rc ? 1 : 0);

    return rc ? 1 : 0;
}

/* Fetch trace context from parent (subrequest or internal redirect) request. */
static trace_context_request_ctx_t *trace_context_get_parent_ctx(request_rec *r)
{
    trace_context_request_ctx_t *ctx = NULL;

    if (r->main != NULL) {
        ctx = ap_get_module_config(r->main->request_config, &trace_context_module);
        if (ctx != NULL) {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: found parent context on r->main");
        }
    }
    if (ctx == NULL && r->prev != NULL) {
        ctx = ap_get_module_config(r->prev->request_config, &trace_context_module);
        if (ctx != NULL) {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: found parent context on r->prev");
        }
    }

    if (ctx == NULL && (r->main != NULL || r->prev != NULL)) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: no parent context found on parent request chain");
    }

    return ctx;
}

/* Copy a request trace context, duplicating pooled strings into target pool. */
static void trace_context_copy_request_ctx(apr_pool_t *p,
                                           trace_context_request_ctx_t *dst,
                                           const trace_context_request_ctx_t *src)
{
    memcpy(dst->trace_id, src->trace_id, sizeof(dst->trace_id));
    memcpy(dst->parent_id, src->parent_id, sizeof(dst->parent_id));
    dst->trace_flags = src->trace_flags;
    dst->traceparent = src->traceparent ? apr_pstrdup(p, src->traceparent) : NULL;
    dst->tracestate = src->tracestate ? apr_pstrdup(p, src->tracestate) : NULL;
}

/* Initialize a brand-new request context from inbound headers or new IDs. */
static int trace_context_init_new_ctx(request_rec *r,
                                      const trace_context_dir_conf_t *conf,
                                      trace_context_request_ctx_t *ctx)
{
    const char *traceparent = apr_table_get(r->headers_in, "traceparent");
    const char *tracestate = apr_table_get(r->headers_in, "tracestate");
    int continue_trace = trace_context_is_trace_continuation_allowed(r, conf);
    int has_incoming_traceparent = traceparent != NULL && traceparent[0] != '\0';
    int parsed_traceparent = 0;
    int incoming_sampled;
    int sampling_requested;
    int sampled;

    if (has_incoming_traceparent) {
        parsed_traceparent = trace_context_parse_traceparent(traceparent, ctx->trace_id, ctx->parent_id, &ctx->trace_flags);
    }

    if (continue_trace && parsed_traceparent) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "mod_trace_context: continuing incoming traceparent=%s",
                      traceparent);
        if (tracestate != NULL && tracestate[0] != '\0') {
            ctx->tracestate = apr_pstrdup(r->pool, tracestate);
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: accepted incoming tracestate");
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                          "mod_trace_context: no incoming tracestate to continue");
        }
    }
    else {
        if (!continue_trace && has_incoming_traceparent) {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: ignored incoming traceparent because continuation is denied");
        }
        else if (continue_trace && has_incoming_traceparent && !parsed_traceparent) {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: ignored invalid incoming traceparent");
        }
        else if (!has_incoming_traceparent) {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                          "mod_trace_context: no incoming traceparent provided");
        }

        if (tracestate != NULL && tracestate[0] != '\0') {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                          "mod_trace_context: ignoring incoming tracestate because a new trace is started");
        }

        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "mod_trace_context: starting new trace (continuation=%d, traceparent_valid=%d)",
                      continue_trace,
                      parsed_traceparent);
        if (!trace_context_random_trace_id(ctx->trace_id)) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "mod_trace_context: failed to generate secure trace id");
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        ctx->trace_flags = 0;
    }

    if (!trace_context_random_parent_id(ctx->parent_id)) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                      "mod_trace_context: failed to generate secure parent id");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    incoming_sampled = (ctx->trace_flags & TRACE_CONTEXT_TRACE_FLAG_SAMPLED) ? 1 : 0;
    sampling_requested = trace_context_is_sampling_requested(r, conf);
    sampled = incoming_sampled;

    if (incoming_sampled) {
        if (trace_context_is_sampling_allowed(r, conf)) {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: kept incoming sampled flag (TraceContextSampleAllowExpr matched)");
        }
        else {
            ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                          "mod_trace_context: removed incoming sampled flag (TraceContextSampleAllowExpr denied)");
            sampled = 0;
        }
    }

    if (sampling_requested) {
        sampled = 1;
    }

    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                  "mod_trace_context: sampling decision incoming=%d requested=%d final=%d",
                  incoming_sampled,
                  sampling_requested,
                  sampled);

    if (sampled) {
        ctx->trace_flags |= TRACE_CONTEXT_TRACE_FLAG_SAMPLED;
    }
    else {
        ctx->trace_flags &= ~TRACE_CONTEXT_TRACE_FLAG_SAMPLED;
    }

    ctx->traceparent = trace_context_build_traceparent(r->pool, ctx->trace_id, ctx->parent_id, ctx->trace_flags);
    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                  "mod_trace_context: built traceparent=%s",
                  ctx->traceparent);

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "mod_trace_context: initialized context trace_id=%s parent_id=%s sampled=%d",
                  ctx->trace_id,
                  ctx->parent_id,
                  (ctx->trace_flags & TRACE_CONTEXT_TRACE_FLAG_SAMPLED) ? 1 : 0);

    return OK;
}

/* Build request tracestate and ensure an Apache member is present first. */
static void trace_context_build_request_tracestate(request_rec *r,
                                                   const trace_context_dir_conf_t *conf,
                                                   trace_context_request_ctx_t *ctx)
{
    const char *unique_id = apr_table_get(r->subprocess_env,
                                          TRACE_CONTEXT_ENV_UNIQUE_ID);
    const char *original_unique_id = unique_id;
    const char *replacement_unique_id = NULL;

    if (original_unique_id != NULL) {
        apr_table_setn(r->subprocess_env,
                       TRACE_CONTEXT_ENV_UNIQUE_ID_ORIG,
                       original_unique_id);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: preserved original %s as %s",
                      TRACE_CONTEXT_ENV_UNIQUE_ID,
                      TRACE_CONTEXT_ENV_UNIQUE_ID_ORIG);
    }
    else {
        apr_table_unset(r->subprocess_env, TRACE_CONTEXT_ENV_UNIQUE_ID_ORIG);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: no %s present in subprocess_env",
                      TRACE_CONTEXT_ENV_UNIQUE_ID);
    }

    switch (conf->replace_unique_id) {
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_TRACE_ID:
        replacement_unique_id = ctx->trace_id;
        break;
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_PARENT_ID:
        replacement_unique_id = ctx->parent_id;
        break;
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_BOTH:
        replacement_unique_id = apr_psprintf(r->pool,
                                             "%s-%s",
                                             ctx->trace_id,
                                             ctx->parent_id);
        break;
    case TRACE_CONTEXT_REPLACE_UNIQUE_ID_OFF:
    default:
        break;
    }

    if (replacement_unique_id != NULL) {
        unique_id = replacement_unique_id;
        apr_table_setn(r->subprocess_env, TRACE_CONTEXT_ENV_UNIQUE_ID, unique_id);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: replaced UNIQUE_ID for tracestate (mode=%s)",
                      trace_context_replace_unique_id_mode_name(conf->replace_unique_id));
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: kept original UNIQUE_ID for tracestate (mode=%s)",
                      trace_context_replace_unique_id_mode_name(conf->replace_unique_id));
    }

    ctx->tracestate = trace_context_ensure_apache_tracestate(r->pool,
                                                              ctx->tracestate,
                                                              conf->tracestate_member_name,
                                                              unique_id);
    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                  "mod_trace_context: built tracestate with member '%s': %s",
                  conf->tracestate_member_name,
                  ctx->tracestate);
}

/* Set trace headers into the provided table according to configured mode. */
static void trace_context_set_headers(request_rec *r,
                                      apr_table_t *headers,
                                      int header_mode,
                                      const char *header_scope,
                                      const trace_context_request_ctx_t *ctx)
{
    if ((header_mode & TRACE_CONTEXT_HEADER_TRACEPARENT) &&
        ctx->traceparent != NULL) {
        apr_table_setn(headers, "traceparent", ctx->traceparent);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: propagated %s header traceparent",
                      header_scope);
    }
    else if (header_mode & TRACE_CONTEXT_HEADER_TRACEPARENT) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: skipped %s header traceparent propagation (missing value)",
                      header_scope);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: %s header traceparent propagation disabled",
                      header_scope);
    }

    if ((header_mode & TRACE_CONTEXT_HEADER_TRACESTATE) &&
        ctx->tracestate != NULL) {
        apr_table_setn(headers, "tracestate", ctx->tracestate);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: propagated %s header tracestate",
                      header_scope);
    }
    else if (header_mode & TRACE_CONTEXT_HEADER_TRACESTATE) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: skipped %s header tracestate propagation (missing value)",
                      header_scope);
    }
    else {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: %s header tracestate propagation disabled",
                      header_scope);
    }

    ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                  "mod_trace_context: %s header propagation completed (mode=%s)",
                  header_scope,
                  trace_context_header_mode_name(header_mode));
}

/* Mirror trace context values into request environment variables. */
static void trace_context_set_sampled_env_var(request_rec *r,
                                               const trace_context_request_ctx_t *ctx)
{
    if (ctx->trace_id[0] != '\0') {
        apr_table_setn(r->subprocess_env, TRACE_CONTEXT_NOTE_TRACE_ID, ctx->trace_id);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: set request env var %s=%s",
                      TRACE_CONTEXT_NOTE_TRACE_ID,
                      ctx->trace_id);
    }
    else {
        apr_table_unset(r->subprocess_env, TRACE_CONTEXT_NOTE_TRACE_ID);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: unset request env var %s",
                      TRACE_CONTEXT_NOTE_TRACE_ID);
    }

    if (ctx->parent_id[0] != '\0') {
        apr_table_setn(r->subprocess_env, TRACE_CONTEXT_NOTE_PARENT_ID, ctx->parent_id);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: set request env var %s=%s",
                      TRACE_CONTEXT_NOTE_PARENT_ID,
                      ctx->parent_id);
    }
    else {
        apr_table_unset(r->subprocess_env, TRACE_CONTEXT_NOTE_PARENT_ID);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: unset request env var %s",
                      TRACE_CONTEXT_NOTE_PARENT_ID);
    }

    if (ctx->traceparent != NULL && ctx->traceparent[0] != '\0') {
        apr_table_setn(r->subprocess_env, TRACE_CONTEXT_NOTE_TRACEPARENT, ctx->traceparent);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: set request env var %s=%s",
                      TRACE_CONTEXT_NOTE_TRACEPARENT,
                      ctx->traceparent);
    }
    else {
        apr_table_unset(r->subprocess_env, TRACE_CONTEXT_NOTE_TRACEPARENT);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: unset request env var %s",
                      TRACE_CONTEXT_NOTE_TRACEPARENT);
    }

    if (ctx->tracestate != NULL && ctx->tracestate[0] != '\0') {
        apr_table_setn(r->subprocess_env, TRACE_CONTEXT_NOTE_TRACESTATE, ctx->tracestate);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: set request env var %s=%s",
                      TRACE_CONTEXT_NOTE_TRACESTATE,
                      ctx->tracestate);
    }
    else {
        apr_table_unset(r->subprocess_env, TRACE_CONTEXT_NOTE_TRACESTATE);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE4, 0, r,
                      "mod_trace_context: unset request env var %s",
                      TRACE_CONTEXT_NOTE_TRACESTATE);
    }

    if ((ctx->trace_flags & TRACE_CONTEXT_TRACE_FLAG_SAMPLED) != 0) {
        apr_table_setn(r->subprocess_env, TRACE_CONTEXT_NOTE_SAMPLED, "1");
        ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                      "mod_trace_context: set request env var %s=1",
                      TRACE_CONTEXT_NOTE_SAMPLED);
    }
    else {
        apr_table_unset(r->subprocess_env, TRACE_CONTEXT_NOTE_SAMPLED);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: unset request env var %s",
                      TRACE_CONTEXT_NOTE_SAMPLED);
    }
}

/* post_read_request hook: create/inherit request context and update request headers. */
static int trace_context_post_read_request(request_rec *r)
{
    trace_context_dir_conf_t *conf;
    trace_context_request_ctx_t *ctx;
    trace_context_request_ctx_t *parent_ctx;

    conf = ap_get_module_config(r->per_dir_config, &trace_context_module);
    if (conf == NULL || !conf->enabled) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: skipped post_read_request (disabled)");
        return DECLINED;
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "mod_trace_context: post_read_request config request_headers=%s response_headers=%s replace_unique_id=%s tracestate_member=%s",
                  trace_context_header_mode_name(conf->request_headers),
                  trace_context_header_mode_name(conf->response_headers),
                  trace_context_replace_unique_id_mode_name(conf->replace_unique_id),
                  conf->tracestate_member_name);

    ctx = apr_pcalloc(r->pool, sizeof(*ctx));
    ap_set_module_config(r->request_config, &trace_context_module, ctx);

    parent_ctx = trace_context_get_parent_ctx(r);
    if (parent_ctx != NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                      "mod_trace_context: inherited context from parent request");
        trace_context_copy_request_ctx(r->pool, ctx, parent_ctx);
    }
    else {
        int rc = trace_context_init_new_ctx(r, conf, ctx);
        if (rc != OK) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "mod_trace_context: failed to initialize request context (rc=%d)",
                          rc);
            return rc;
        }
    }

    trace_context_build_request_tracestate(r, conf, ctx);
    trace_context_set_sampled_env_var(r, ctx);

    trace_context_set_headers(r, r->headers_in, conf->request_headers, "request", ctx);

    ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                  "mod_trace_context: request headers updated (mode=%s)",
                  trace_context_header_mode_name(conf->request_headers));

    return DECLINED;
}

/* fixups hook: propagate trace headers to response/error headers as configured. */
static int trace_context_fixups(request_rec *r)
{
    trace_context_dir_conf_t *conf;
    trace_context_request_ctx_t *ctx;

    conf = ap_get_module_config(r->per_dir_config, &trace_context_module);
    if (conf == NULL || !conf->enabled) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: skipped fixups (disabled)");
        return DECLINED;
    }

    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                  "mod_trace_context: fixups config response_headers=%s",
                  trace_context_header_mode_name(conf->response_headers));

    ctx = ap_get_module_config(r->request_config, &trace_context_module);
    if (ctx == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_TRACE3, 0, r,
                      "mod_trace_context: skipped fixups (no request context)");
        return DECLINED;
    }

    trace_context_set_headers(r, r->err_headers_out, conf->response_headers, "response", ctx);

    ap_log_rerror(APLOG_MARK, APLOG_TRACE1, 0, r,
                  "mod_trace_context: response headers updated (mode=%s)",
                  trace_context_header_mode_name(conf->response_headers));

    return DECLINED;
}


/* Directive handler for `TraceContext` on/off flag. */
static const char *trace_context_cmd_enabled(cmd_parms *cmd,
                                             void *mconfig,
                                             int enabled)
{
    trace_context_dir_conf_t *conf = mconfig;

    (void)cmd;

    conf->enabled = enabled ? 1 : 0;
    conf->enabled_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextRequestHeaders` mode. */
static const char *trace_context_cmd_request_headers(cmd_parms *cmd,
                                                     void *mconfig,
                                                     const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;
    const char *err;

    (void)cmd;

    err = trace_context_parse_header_mode(arg, &conf->request_headers);
    if (err != NULL) {
        return err;
    }

    conf->request_headers_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextResponseHeaders` mode. */
static const char *trace_context_cmd_response_headers(cmd_parms *cmd,
                                                      void *mconfig,
                                                      const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;
    const char *err;

    (void)cmd;

    err = trace_context_parse_header_mode(arg, &conf->response_headers);
    if (err != NULL) {
        return err;
    }

    conf->response_headers_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextSampleAllowExpr`. */
static const char *trace_context_cmd_sample_allow_expr(cmd_parms *cmd,
                                                       void *mconfig,
                                                       const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;
    ap_expr_info_t *expr;
    const char *err = NULL;

    if (arg == NULL || *arg == '\0') {
        return "TraceContextSampleAllowExpr must be 'none', 'all', or an ap_expr";
    }

    if (!strcasecmp(arg, "none")) {
        conf->sample_allow = NULL;
        conf->sample_allow_all = 0;
        conf->sample_allow_set = 1;
        return NULL;
    }

    if (!strcasecmp(arg, "all")) {
        conf->sample_allow = NULL;
        conf->sample_allow_all = 1;
        conf->sample_allow_set = 1;
        return NULL;
    }

    expr = ap_expr_parse_cmd(cmd, arg, 0, &err, NULL);
    if (expr == NULL) {
        return err ? err : "failed to parse TraceContextSampleAllowExpr";
    }

    conf->sample_allow = expr;
    conf->sample_allow_all = 0;
    conf->sample_allow_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextSampleExpr`. */
static const char *trace_context_cmd_sample_expr(cmd_parms *cmd,
                                                 void *mconfig,
                                                 const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;
    ap_expr_info_t *expr;
    const char *err = NULL;

    if (arg == NULL || *arg == '\0') {
        return "TraceContextSampleExpr must be 'none', 'all', or an ap_expr";
    }

    if (!strcasecmp(arg, "none")) {
        conf->sample = NULL;
        conf->sample_all = 0;
        conf->sample_set = 1;
        return NULL;
    }

    if (!strcasecmp(arg, "all")) {
        conf->sample = NULL;
        conf->sample_all = 1;
        conf->sample_set = 1;
        return NULL;
    }

    expr = ap_expr_parse_cmd(cmd, arg, 0, &err, NULL);
    if (expr == NULL) {
        return err ? err : "failed to parse TraceContextSampleExpr";
    }

    conf->sample = expr;
    conf->sample_all = 0;
    conf->sample_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextContinueTraceAllowExpr`. */
static const char *trace_context_cmd_continue_trace_allow_expr(cmd_parms *cmd,
                                                               void *mconfig,
                                                               const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;
    ap_expr_info_t *expr;
    const char *err = NULL;

    if (arg == NULL || *arg == '\0') {
        return "TraceContextContinueTraceAllowExpr must be 'none', 'all', or an ap_expr";
    }

    if (!strcasecmp(arg, "none")) {
        conf->continue_trace_allow = NULL;
        conf->continue_trace_allow_all = 0;
        conf->continue_trace_allow_set = 1;
        return NULL;
    }

    if (!strcasecmp(arg, "all")) {
        conf->continue_trace_allow = NULL;
        conf->continue_trace_allow_all = 1;
        conf->continue_trace_allow_set = 1;
        return NULL;
    }

    expr = ap_expr_parse_cmd(cmd, arg, 0, &err, NULL);
    if (expr == NULL) {
        return err ? err : "failed to parse TraceContextContinueTraceAllowExpr";
    }

    conf->continue_trace_allow = expr;
    conf->continue_trace_allow_all = 0;
    conf->continue_trace_allow_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextTracestateMemberName`. */
static const char *trace_context_cmd_tracestate_member_name(cmd_parms *cmd,
                                                            void *mconfig,
                                                            const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;

    (void)cmd;

    if (!trace_context_is_valid_tracestate_member_name(arg)) {
        return "TraceContextTracestateMemberName must be a valid tracestate key";
    }

    conf->tracestate_member_name = apr_pstrdup(cmd->pool, arg);
    conf->tracestate_member_name_set = 1;

    return NULL;
}

/* Directive handler for `TraceContextReplaceUniqueID` mode. */
static const char *trace_context_cmd_replace_unique_id(cmd_parms *cmd,
                                                       void *mconfig,
                                                       const char *arg)
{
    trace_context_dir_conf_t *conf = mconfig;
    const char *err;

    (void)cmd;

    err = trace_context_parse_replace_unique_id_mode(arg, &conf->replace_unique_id);
    if (err != NULL) {
        return err;
    }

    conf->replace_unique_id_set = 1;

    return NULL;
}

static const command_rec trace_context_cmds[] = {
    AP_INIT_FLAG("TraceContext",
                 trace_context_cmd_enabled,
                 NULL,
                 OR_FILEINFO,
                 "Enable or disable trace context processing"),
    AP_INIT_TAKE1("TraceContextSampleAllowExpr",
                  trace_context_cmd_sample_allow_expr,
                  NULL,
                  OR_FILEINFO,
                  "Apache expression allowing the sampled trace flag, or 'none' or 'all'"),
    AP_INIT_TAKE1("TraceContextSampleExpr",
                  trace_context_cmd_sample_expr,
                  NULL,
                  OR_FILEINFO,
                  "Apache expression proactively setting the sampled trace flag, or 'none' or 'all'"),
    AP_INIT_TAKE1("TraceContextContinueTraceAllowExpr",
                  trace_context_cmd_continue_trace_allow_expr,
                  NULL,
                  OR_FILEINFO,
                  "Apache expression allowing trace continuation, or 'none' or 'all'"),
    AP_INIT_TAKE1("TraceContextTracestateMemberName",
                  trace_context_cmd_tracestate_member_name,
                  NULL,
                  OR_FILEINFO,
                  "Name used for the tracestate member key (default: apache)"),
    AP_INIT_TAKE1("TraceContextReplaceUniqueID",
                  trace_context_cmd_replace_unique_id,
                  NULL,
                  OR_FILEINFO,
                  "Replace UNIQUE_ID with trace context values: off, on, trace-id, parent-id, both"),
    AP_INIT_TAKE1("TraceContextRequestHeaders",
                  trace_context_cmd_request_headers,
                  NULL,
                  OR_FILEINFO,
                  "Choose request header propagation mode: none, "
                  "traceparent, tracestate, both"),
    AP_INIT_TAKE1("TraceContextResponseHeaders",
                  trace_context_cmd_response_headers,
                  NULL,
                  OR_FILEINFO,
                  "Choose response header propagation mode: none, "
                  "traceparent, tracestate, both"),
    { NULL }
};

/* Register module hooks for request processing. */
static void trace_context_register_hooks(apr_pool_t *p)
{
    static const char *const pre[] = { "mod_unique_id.c", NULL };

    (void)p;

    ap_hook_post_read_request(trace_context_post_read_request, pre, NULL, APR_HOOK_MIDDLE);
    ap_hook_fixups(trace_context_fixups, NULL, NULL, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(trace_context) = {
    STANDARD20_MODULE_STUFF,
    trace_context_create_dir_config,
    trace_context_merge_dir_config,
    NULL,
    NULL,
    trace_context_cmds,
    trace_context_register_hooks,
    AP_MODULE_FLAG_NONE
};
