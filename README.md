mod_trace_context
=================

The Apache HTTP Server module `mod_trace_context` implements the W3C Trace Context protocol:
[W3C Trace Context](https://www.w3.org/TR/trace-context/).

The module produces Trace Context headers similar to the following:
```http
traceparent: 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01
tracestate: apache=U09ZosCoABMAAC9CATgAAAAA
```



Configuration
-------------


### TraceContext

* Description: `Enable or disable trace context processing.`
* Syntax: `TraceContext on|off`
* Default: `TraceContext on`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextRequestHeaders

* Description: `Controls which Trace Context headers are injected for backend requests.`
* Syntax: `TraceContextRequestHeaders none|traceparent|tracestate|both`
* Default: `TraceContextRequestHeaders both`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextResponseHeaders

* Description: `Controls which Trace Context headers are injected for client responses.`
* Syntax: `TraceContextResponseHeaders none|traceparent|tracestate|both`
* Default: `TraceContextResponseHeaders none`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextSampleAllowExpr

* Description: `Defines which requests may honor an incoming sampled flag from client traceparent. If a client sends sampled=1 and this expression does not match, Apache ignores the incoming sampled flag.`
* Syntax: `TraceContextSampleAllowExpr none|all|<ap_expr>`
* Default: `TraceContextSampleAllowExpr none`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextSampleExpr

* Description: `Defines requests proactively marked as sampled by Apache (independent of incoming client sampled flag).`
* Syntax: `TraceContextSampleExpr none|all|<ap_expr>`
* Default: `TraceContextSampleExpr none`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextContinueTraceAllowExpr

* Description: `Defines requests for which the trace is continued. For other requests, the trace is restarted.`
* Syntax: `TraceContextContinueTraceAllowExpr none|all|<ap_expr>`
* Default: `TraceContextContinueTraceAllowExpr none`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextTracestateMemberName

* Description: `Defines the tracestate member key used by this module.`
* Syntax: `TraceContextTracestateMemberName <name>`
* Default: `TraceContextTracestateMemberName apache`
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`


### TraceContextReplaceUniqueID

* Description: `Controls whether and how UNIQUE_ID is replaced with trace context values before tracestate is built.`
* Syntax: `TraceContextReplaceUniqueID off|on|trace-id|parent-id|both`
* Default: `TraceContextReplaceUniqueID off`
* Notes: `on` is equivalent to `both` (trace-id and parent-id combination).
* Context: `server config`, `virtual host`, `directory`, `.htaccess`
* Status: `Extension`
* Module: `mod_trace_context`



Environment variables
---------------------


### tc-trace-id

* `tc-trace-id`: set to the resulting trace ID for the request.


### tc-parent-id

* `tc-parent-id`: set to the resulting parent ID for the request.


### tc-traceparent

* `tc-traceparent`: set to the resulting `traceparent` header value for the request.


### tc-tracestate

* `tc-tracestate`: set to the resulting `tracestate` header value for the request.


### tc-sampled

* `tc-sampled`: set to `1` for requests where the resulting trace context has the sampled flag set.


### UNIQUE_ID_ORIG

* `UNIQUE_ID_ORIG`: set to the original incoming `UNIQUE_ID` value before `TraceContextReplaceUniqueID` applies any replacement.
* If no original `UNIQUE_ID` is available for a request, `UNIQUE_ID_ORIG` is not set.

```apache2
LogFormat "%h %l %u %t \"%r\" %>s %b unique_id=%{UNIQUE_ID}e unique_id_orig=%{UNIQUE_ID_ORIG}e trace=%{tc-trace-id}e parent=%{tc-parent-id}e sampled=%{tc-sampled}e traceparent=\"%{tc-traceparent}e\" tracestate=\"%{tc-tracestate}e\"" trace-context
CustomLog logs/access.log trace-context

RequestHeader set X-TC-Trace-ID %{tc-trace-id}e
RequestHeader set X-TC-Parent-ID %{tc-parent-id}e
RequestHeader set X-TC-Sampled %{tc-sampled}e
RequestHeader set X-Unique-ID %{UNIQUE_ID}e
RequestHeader set X-Unique-ID-Orig %{UNIQUE_ID_ORIG}e
```



Development
-----------


Build and run development environment consisting of an httpbin backend and an Apache httpd proxy:

```bash
docker compose build && docker compose up -d && docker compose logs -f
```

Open httpbin through the proxy:

```bash
curl -vks https://localhost:8443/headers
```
```bash
watch -n 1 "curl -vks https://localhost:8443/headers 2>&1 | grep -i trace"
```

Tear down development environment:

```bash
docker compose down
```



License
-------

Licensed under the [Apache License, Version 2.0](./LICENSE).



Useful resources
----------------

* [W3C Trace Context](https://www.w3.org/TR/trace-context/)
