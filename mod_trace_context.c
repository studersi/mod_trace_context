/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <strings.h>

#include "apr_general.h"
#include "apr_lib.h"
#include "apr_optional.h"
#include "apr_strings.h"
#include "apr_tables.h"

#include "ap_expr.h"
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "mod_log_config.h"

module AP_MODULE_DECLARE_DATA trace_context_module;

#define TRACE_CONTEXT_TRACEPARENT_LEN 55
#define TRACE_CONTEXT_TRACE_ID_LEN 32
#define TRACE_CONTEXT_PARENT_ID_LEN 16
#define TRACE_CONTEXT_TRACE_FLAG_SAMPLED 0x01
#define TRACE_CONTEXT_SUPPORTED_VERSION 0x00
#define TRACE_CONTEXT_TRACESTATE_MAX_LEN 512
#define TRACE_CONTEXT_TRACESTATE_MAX_MEMBERS 32
#define TRACE_CONTEXT_TRACESTATE_MEMBER_MAX_LEN 256
#define TRACE_CONTEXT_TRACESTATE_MEMBER_NAME_MAX_LEN \
    (TRACE_CONTEXT_TRACESTATE_MEMBER_MAX_LEN - 1)
#define TRACE_CONTEXT_DEFAULT_TRACESTATE_MEMBER_NAME "apache"
#define TRACE_CONTEXT_NOTE_SAMPLED "tc-sampled"

enum trace_context_header_mask {
    TRACE_CONTEXT_HEADER_NONE = 0,
    TRACE_CONTEXT_HEADER_TRACEPARENT = 1 << 0,
    TRACE_CONTEXT_HEADER_TRACESTATE = 1 << 1,
    TRACE_CONTEXT_HEADER_BOTH = TRACE_CONTEXT_HEADER_TRACEPARENT |
                                TRACE_CONTEXT_HEADER_TRACESTATE
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
} trace_context_dir_conf_t;

typedef struct trace_context_request_ctx_t {
    char trace_id[TRACE_CONTEXT_TRACE_ID_LEN + 1];
    char parent_id[TRACE_CONTEXT_PARENT_ID_LEN + 1];
    unsigned int trace_flags;
    const char *traceparent;
    const char *tracestate;
} trace_context_request_ctx_t;

static const char trace_context_log_dash[] = "-";

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
    conf->request_headers_set = add->request_headers_set ||
                                base->request_headers_set;

    conf->response_headers = add->response_headers_set ? add->response_headers
                                                       : base->response_headers;
    conf->response_headers_set = add->response_headers_set ||
                                 base->response_headers_set;

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
    conf->continue_trace_allow_set = add->continue_trace_allow_set ||
                                     base->continue_trace_allow_set;

    conf->tracestate_member_name = add->tracestate_member_name_set
                                       ? add->tracestate_member_name
                                       : base->tracestate_member_name;
    conf->tracestate_member_name_set = add->tracestate_member_name_set ||
                                       base->tracestate_member_name_set;

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

/* Validate a configured tracestate member key. */
static int trace_context_is_valid_tracestate_member_name(const char *name)
{
    const char *at;
    const char *cursor;
    apr_size_t name_len;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }

    name_len = strlen(name);
    if (name_len == 0 ||
        name_len > TRACE_CONTEXT_TRACESTATE_MEMBER_NAME_MAX_LEN) {
        return 0;
    }

    at = strchr(name, '@');
    if (at != NULL) {
        apr_size_t tenant_len;
        apr_size_t system_len;

        if (strchr(at + 1, '@') != NULL) {
            return 0;
        }

        tenant_len = (apr_size_t)(at - name);
        system_len = name_len - tenant_len - 1;
        if (tenant_len == 0 || tenant_len > 241 || system_len == 0 ||
            system_len > 14) {
            return 0;
        }
    }

    cursor = name;
    if (*cursor < 'a' || *cursor > 'z') {
        return 0;
    }

    for (++cursor; *cursor != '\0'; ++cursor) {
        if (*cursor == '@') {
            if (cursor[1] < 'a' || cursor[1] > 'z') {
                return 0;
            }
            continue;
        }

        if (!trace_context_is_tracestate_member_name_char((unsigned char)*cursor)) {
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

    return apr_pstrcat(p,
                       member_name,
                       "=",
                       apr_pstrndup(p, member_value, member_value_len),
                       NULL);
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

/* Ensure the module's tracestate member is prepended and preserve valid peers. */
static const char *trace_context_ensure_apache_tracestate(apr_pool_t *p,
                                                          const char *tracestate,
                                                          const char *member_name,
                                                          const char *unique_id)
{
    const char *apache_member;
    apr_array_header_t *members;
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
    cursor = tracestate;

    while (*cursor != '\0') {
        const char *member_start;
        const char *member_end;
        apr_size_t member_len;

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
        if (member_len > 0 &&
            trace_context_can_append_tracestate_member(tracestate_len,
                                                       member_count,
                                                       member_len)) {
            *(const char **)apr_array_push(members) =
                apr_pstrndup(p, member_start, member_len);
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

/* Evaluate whether incoming sampled traces are allowed to remain sampled. */
static int trace_context_is_sampling_allowed(request_rec *r,
                                             const trace_context_dir_conf_t *conf)
{
    const char *err = NULL;
    int rc;

    if (conf->sample_allow_all) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE2,
                      0,
                      r,
                      "mod_trace_context: sampling allowed by TraceContextSampleAllowExpr=all");
        return 1;
    }

    if (conf->sample_allow == NULL) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE2,
                      0,
                      r,
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

    ap_log_rerror(APLOG_MARK,
                  APLOG_TRACE2,
                  0,
                  r,
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
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE2,
                      0,
                      r,
                      "mod_trace_context: sampling requested by TraceContextSampleExpr=all");
        return 1;
    }

    if (conf->sample == NULL) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE2,
                      0,
                      r,
                      "mod_trace_context: sampling not requested (TraceContextSampleExpr=none)");
        return 0;
    }

    rc = ap_expr_exec(r, conf->sample, &err);
    if (err != NULL) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_WARNING,
                      0,
                      r,
                      "mod_trace_context: failed to evaluate TraceContextSampleExpr: %s",
                      err);
        return 0;
    }

    ap_log_rerror(APLOG_MARK,
                  APLOG_TRACE2,
                  0,
                  r,
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
        ap_log_rerror(
            APLOG_MARK,
            APLOG_TRACE2,
            0,
            r,
            "mod_trace_context: trace continuation %s (TraceContextContinueTraceAllowExpr=%s)",
            conf->continue_trace_allow_all ? "allowed" : "denied",
            conf->continue_trace_allow_all ? "all" : "none");
        return conf->continue_trace_allow_all ? 1 : 0;
    }

    rc = ap_expr_exec(r, conf->continue_trace_allow, &err);
    if (err != NULL) {
        ap_log_rerror(
            APLOG_MARK,
            APLOG_WARNING,
            0,
            r,
            "mod_trace_context: failed to evaluate TraceContextContinueTraceAllowExpr: %s",
            err);
        return 0;
    }

    ap_log_rerror(APLOG_MARK,
                  APLOG_TRACE2,
                  0,
                  r,
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
    }
    if (ctx == NULL && r->prev != NULL) {
        ctx = ap_get_module_config(r->prev->request_config, &trace_context_module);
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
    int incoming_sampled;
    int sampling_requested;
    int sampled;

    if (continue_trace &&
        trace_context_parse_traceparent(traceparent,
                                        ctx->trace_id,
                                        ctx->parent_id,
                                        &ctx->trace_flags)) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE1,
                      0,
                      r,
                      "mod_trace_context: continuing incoming traceparent=%s",
                      traceparent);
        if (tracestate != NULL && tracestate[0] != '\0') {
            ctx->tracestate = apr_pstrdup(r->pool, tracestate);
            ap_log_rerror(APLOG_MARK,
                          APLOG_TRACE2,
                          0,
                          r,
                          "mod_trace_context: accepted incoming tracestate");
        }
    }
    else {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE1,
                      0,
                      r,
                      "mod_trace_context: starting new trace (continuation=%d, traceparent_valid=%d)",
                      continue_trace,
                      trace_context_parse_traceparent(traceparent,
                                                      ctx->trace_id,
                                                      ctx->parent_id,
                                                      &ctx->trace_flags));
        if (!trace_context_random_trace_id(ctx->trace_id)) {
            ap_log_rerror(APLOG_MARK,
                          APLOG_ERR,
                          0,
                          r,
                          "mod_trace_context: failed to generate secure trace id");
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        ctx->trace_flags = 0;
    }

    if (!trace_context_random_parent_id(ctx->parent_id)) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_ERR,
                      0,
                      r,
                      "mod_trace_context: failed to generate secure parent id");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    incoming_sampled = (ctx->trace_flags & TRACE_CONTEXT_TRACE_FLAG_SAMPLED) ? 1 : 0;
    sampling_requested = trace_context_is_sampling_requested(r, conf);
    sampled = incoming_sampled;

    if (incoming_sampled) {
        if (trace_context_is_sampling_allowed(r, conf)) {
            ap_log_rerror(APLOG_MARK,
                          APLOG_TRACE2,
                          0,
                          r,
                          "mod_trace_context: kept incoming sampled flag (TraceContextSampleAllowExpr matched)");
        }
        else {
            ap_log_rerror(APLOG_MARK,
                          APLOG_TRACE2,
                          0,
                          r,
                          "mod_trace_context: removed incoming sampled flag (TraceContextSampleAllowExpr denied)");
            sampled = 0;
        }
    }

    if (sampling_requested) {
        sampled = 1;
    }

    if (sampled) {
        ctx->trace_flags |= TRACE_CONTEXT_TRACE_FLAG_SAMPLED;
    }
    else {
        ctx->trace_flags &= ~TRACE_CONTEXT_TRACE_FLAG_SAMPLED;
    }

    ctx->traceparent = trace_context_build_traceparent(r->pool,
                                                       ctx->trace_id,
                                                       ctx->parent_id,
                                                       ctx->trace_flags);

    ap_log_rerror(APLOG_MARK,
                  APLOG_DEBUG,
                  0,
                  r,
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
    const char *unique_id = apr_table_get(r->subprocess_env, "UNIQUE_ID");

    if (unique_id == NULL || unique_id[0] == '\0') {
        unique_id = apr_psprintf(r->pool, "%s-%s", ctx->trace_id, ctx->parent_id);
        apr_table_setn(r->subprocess_env, "UNIQUE_ID", unique_id);
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE2,
                      0,
                      r,
                      "mod_trace_context: generated fallback UNIQUE_ID for tracestate");
    }

    ctx->tracestate = trace_context_ensure_apache_tracestate(r->pool,
                                                              ctx->tracestate,
                                                              conf->tracestate_member_name,
                                                              unique_id);
    ap_log_rerror(APLOG_MARK,
                  APLOG_TRACE2,
                  0,
                  r,
                  "mod_trace_context: built tracestate with member '%s'",
                  conf->tracestate_member_name);
}

/* Set trace headers into the provided table according to configured mode. */
static void trace_context_set_headers(apr_table_t *headers,
                                      int header_mode,
                                      const trace_context_request_ctx_t *ctx)
{
    if ((header_mode & TRACE_CONTEXT_HEADER_TRACEPARENT) &&
        ctx->traceparent != NULL) {
        apr_table_setn(headers, "traceparent", ctx->traceparent);
    }
    if ((header_mode & TRACE_CONTEXT_HEADER_TRACESTATE) &&
        ctx->tracestate != NULL) {
        apr_table_setn(headers, "tracestate", ctx->tracestate);
    }
}

/* Mirror sampled state into a request note used by logging and other modules. */
static void trace_context_set_sampled_note(request_rec *r,
                                           const trace_context_request_ctx_t *ctx)
{
    if ((ctx->trace_flags & TRACE_CONTEXT_TRACE_FLAG_SAMPLED) != 0) {
        apr_table_setn(r->notes, TRACE_CONTEXT_NOTE_SAMPLED, "1");
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE2,
                      0,
                      r,
                      "mod_trace_context: set request note %s=1",
                      TRACE_CONTEXT_NOTE_SAMPLED);
    }
    else {
        apr_table_unset(r->notes, TRACE_CONTEXT_NOTE_SAMPLED);
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
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE3,
                      0,
                      r,
                      "mod_trace_context: skipped post_read_request (disabled)");
        return DECLINED;
    }

    ctx = apr_pcalloc(r->pool, sizeof(*ctx));
    ap_set_module_config(r->request_config, &trace_context_module, ctx);

    parent_ctx = trace_context_get_parent_ctx(r);
    if (parent_ctx != NULL) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE1,
                      0,
                      r,
                      "mod_trace_context: inherited context from parent request");
        trace_context_copy_request_ctx(r->pool, ctx, parent_ctx);
    }
    else {
        int rc = trace_context_init_new_ctx(r, conf, ctx);
        if (rc != OK) {
            return rc;
        }
    }

    trace_context_build_request_tracestate(r, conf, ctx);
    trace_context_set_sampled_note(r, ctx);

    trace_context_set_headers(r->headers_in, conf->request_headers, ctx);

    ap_log_rerror(APLOG_MARK,
                  APLOG_TRACE1,
                  0,
                  r,
                  "mod_trace_context: request headers updated (mode=%d)",
                  conf->request_headers);

    return DECLINED;
}

/* fixups hook: propagate trace headers to response/error headers as configured. */
static int trace_context_fixups(request_rec *r)
{
    trace_context_dir_conf_t *conf;
    trace_context_request_ctx_t *ctx;

    conf = ap_get_module_config(r->per_dir_config, &trace_context_module);
    if (conf == NULL || !conf->enabled) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE3,
                      0,
                      r,
                      "mod_trace_context: skipped fixups (disabled)");
        return DECLINED;
    }

    ctx = ap_get_module_config(r->request_config, &trace_context_module);
    if (ctx == NULL) {
        ap_log_rerror(APLOG_MARK,
                      APLOG_TRACE3,
                      0,
                      r,
                      "mod_trace_context: skipped fixups (no request context)");
        return DECLINED;
    }

    trace_context_set_headers(r->err_headers_out, conf->response_headers, ctx);

    ap_log_rerror(APLOG_MARK,
                  APLOG_TRACE1,
                  0,
                  r,
                  "mod_trace_context: response headers updated (mode=%d)",
                  conf->response_headers);

    return DECLINED;
}

/* Retrieve this module's request context from a request record. */
static trace_context_request_ctx_t *trace_context_get_req_ctx(request_rec *r)
{
    if (r == NULL) {
        return NULL;
    }

    return ap_get_module_config(r->request_config, &trace_context_module);
}

/* Log provider for the current trace-id. */
static const char *trace_context_log_trace_id(request_rec *r, char *a)
{
    trace_context_request_ctx_t *ctx = trace_context_get_req_ctx(r);

    (void)a;

    return ctx ? ctx->trace_id : trace_context_log_dash;
}

/* Log provider for the current parent-id. */
static const char *trace_context_log_parent_id(request_rec *r, char *a)
{
    trace_context_request_ctx_t *ctx = trace_context_get_req_ctx(r);

    (void)a;

    return ctx ? ctx->parent_id : trace_context_log_dash;
}

/* Log provider for the full traceparent header value. */
static const char *trace_context_log_traceparent(request_rec *r, char *a)
{
    trace_context_request_ctx_t *ctx = trace_context_get_req_ctx(r);

    (void)a;

    return (ctx != NULL && ctx->traceparent != NULL) ? ctx->traceparent
                                                     : trace_context_log_dash;
}

/* Log provider for the tracestate header value. */
static const char *trace_context_log_tracestate(request_rec *r, char *a)
{
    trace_context_request_ctx_t *ctx = trace_context_get_req_ctx(r);

    (void)a;

    return (ctx != NULL && ctx->tracestate != NULL) ? ctx->tracestate
                                                    : trace_context_log_dash;
}

/* Log provider for sampled flag as `1`/`0` or `-` when unavailable. */
static const char *trace_context_log_sampled(request_rec *r, char *a)
{
    trace_context_request_ctx_t *ctx = trace_context_get_req_ctx(r);

    (void)a;

    if (ctx == NULL) {
        return trace_context_log_dash;
    }

    return (ctx->trace_flags & TRACE_CONTEXT_TRACE_FLAG_SAMPLED) ? "1" : "0";
}

/* Generic `%{name}^TC` log handler dispatcher. */
static const char *trace_context_log_generic(request_rec *r, char *a)
{
    if (a == NULL || *a == '\0') {
        return trace_context_log_dash;
    }
    if (!strcasecmp(a, "traceparent")) {
        return trace_context_log_traceparent(r, a);
    }
    if (!strcasecmp(a, "trace-id")) {
        return trace_context_log_trace_id(r, a);
    }
    if (!strcasecmp(a, "parent-id")) {
        return trace_context_log_parent_id(r, a);
    }
    if (!strcasecmp(a, "tracestate")) {
        return trace_context_log_tracestate(r, a);
    }
    if (!strcasecmp(a, "sampled")) {
        return trace_context_log_sampled(r, a);
    }

    return trace_context_log_dash;
}

/* pre_config hook: register custom log handler tokens for trace context. */
static int trace_context_pre_config(apr_pool_t *p, apr_pool_t *plog,
                                    apr_pool_t *ptemp)
{
    APR_OPTIONAL_FN_TYPE(ap_register_log_handler) *register_log_handler;

    (void)plog;
    (void)ptemp;

    register_log_handler = APR_RETRIEVE_OPTIONAL_FN(ap_register_log_handler);
    if (register_log_handler != NULL) {
        register_log_handler(p, "^TC", trace_context_log_generic, 0);
    }

    return OK;
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

/* Register module hooks for request processing and log token setup. */
static void trace_context_register_hooks(apr_pool_t *p)
{
    static const char *const pre[] = { "mod_unique_id.c", NULL };

    (void)p;

    ap_hook_pre_config(trace_context_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
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
