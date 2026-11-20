/* nethost.h — 宿主网络能力：net.get / net.post（WinHTTP，含 https）与 serve（Winsock）
 *
 * 由 builtins_impl.h 直接 include，因此能用 eval.c 的内部符号
 * （MiBinding、mi_invoke、output_line、g_state、arg_of、need_text 等）。
 *
 * 语义与 Python 宿主（米/宿主/python_host.py）对齐：
 *   net.get  url [safe]           -> assembly{status, ok, body}
 *   net.post url body [safe]      -> assembly{status, ok, body}
 *   serve    port handler         -> nothing（阻塞服务，每请求重置步数预算）
 *
 * safe 为真时网络故障返回 status 0，否则抛 E0309。
 */
#ifndef MI_NETHOST_H
#define MI_NETHOST_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <strings.h>

#define MI_NET_TIMEOUT_MS 10000

/* --------------------------------------------------------------- 小工具 */
static char *net_utf16_to_utf8(const wchar_t *text) {
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return mi_strdup("");
    char *out = mi_alloc((size_t)need);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out, need, NULL, NULL);
    return out;
}

static wchar_t *net_utf8_to_utf16(const char *text) {
    int need = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (need <= 0) return NULL;
    wchar_t *out = mi_alloc((size_t)need * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, need);
    return out;
}

/* 把变长缓冲区追加一段字节 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} NetBuf;

static void net_buf_append(NetBuf *buf, const char *bytes, size_t len) {
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 4096;
        while (cap < buf->len + len + 1) cap *= 2;
        buf->data = mi_realloc(buf->data, cap);
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static MiValue net_result(int status, int ok, const char *body) {
    MiAsm *asmv = mi_asm_alloc(3);
    mi_asm_set(asmv, "status", mi_int(status));
    mi_asm_set(asmv, "ok", mi_flag(ok));
    mi_asm_set(asmv, "body", mi_text_new(body ? body : ""));
    return mi_asm_take(asmv);
}

/* -------------------------------------------------------------- net.get/post */
static MiValue host_http(const char *method, const char *url, const char *body,
                         int has_body, int safe, MiSpan span) {
    if (!url || !*url) {
        if (safe) return net_result(0, 0, "网络错误：URL 为空");
        mi_raise("E0309", span, NULL, "network error: empty URL");
    }

    wchar_t *wurl = net_utf8_to_utf16(url);
    if (!wurl) {
        if (safe) return net_result(0, 0, "网络错误：URL 无法转换");
        mi_raise("E0309", span, NULL, "network error: cannot convert URL %s", url);
    }

    URL_COMPONENTS parts;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    wchar_t host[512];
    wchar_t path[4096];
    wchar_t extra[2048];
    memset(host, 0, sizeof(host));
    memset(path, 0, sizeof(path));
    memset(extra, 0, sizeof(extra));
    parts.lpszHostName = host;
    parts.dwHostNameLength = 512;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;

    if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        free(wurl);
        if (safe) return net_result(0, 0, "网络错误：URL 格式不正确");
        mi_raise("E0309", span, NULL, "network error for %s: invalid URL", url);
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        free(wurl);
        if (safe) return net_result(0, 0, "网络错误：只支持 http 与 https");
        mi_raise("E0309", span, NULL,
                 "network error for %s: only http and https are supported", url);
    }

    MiValue result = MI_NOTHING_VALUE;
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    char *payload = NULL;

    session = WinHttpOpen(L"MI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto transport_failure;
    WinHttpSetTimeouts(session, MI_NET_TIMEOUT_MS, MI_NET_TIMEOUT_MS,
                       MI_NET_TIMEOUT_MS, MI_NET_TIMEOUT_MS);

    connection = WinHttpConnect(session, host, (INTERNET_PORT)parts.nPort, 0);
    if (!connection) goto transport_failure;

    {
        /* 路径 + 查询串 */
        wchar_t target[6144];
        swprintf(target, 6144, L"%s%s", path, extra);
        wchar_t *wmethod = net_utf8_to_utf16(method);
        if (!wmethod) goto transport_failure;
        request = WinHttpOpenRequest(
            connection, wmethod, target, NULL, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        free(wmethod);
        if (!request) goto transport_failure;
    }

    {
        int sent;
        if (has_body) {
            size_t body_len = strlen(body ? body : "");
            sent = WinHttpSendRequest(
                request, L"Content-Type: text/plain; charset=utf-8", (DWORD)-1L,
                (LPVOID)(body ? body : ""), (DWORD)body_len, (DWORD)body_len, 0);
        } else {
            sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        }
        if (!sent) goto transport_failure;
    }
    if (!WinHttpReceiveResponse(request, NULL)) goto transport_failure;

    {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            status = 0;
        }

        NetBuf buffer = {NULL, 0, 0};
        net_buf_append(&buffer, "", 0);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) break;
            char *chunk = mi_alloc(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk, available, &read)) {
                free(chunk);
                break;
            }
            if (read == 0) {
                free(chunk);
                break;
            }
            net_buf_append(&buffer, chunk, read);
            free(chunk);
        }
        payload = buffer.data;
        int ok = status >= 200 && status < 300;
        result = net_result((int)status, ok, payload ? payload : "");
    }
    goto cleanup;

transport_failure:
    {
        DWORD code = GetLastError();
        if (payload) {
            free(payload);
            payload = NULL;
        }
        if (safe) {
            char message[256];
            snprintf(message, sizeof(message), "网络错误：WinHTTP 失败（错误码 %lu）",
                     (unsigned long)code);
            result = net_result(0, 0, message);
        } else {
            if (request) WinHttpCloseHandle(request);
            if (connection) WinHttpCloseHandle(connection);
            if (session) WinHttpCloseHandle(session);
            free(wurl);
            mi_raise("E0309", span, NULL,
                     "network error for %s: WinHTTP failed (code %lu)", url,
                     (unsigned long)code);
        }
    }

cleanup:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    free(payload);
    free(wurl);
    return result;
}

/* ------------------------------------------------------------------- serve */
/* 十六进制文本解码（响应的 bytes 字段） */
static unsigned char *net_hex_decode(const char *text, size_t *out_len) {
    size_t len = strlen(text);
    if (len % 2 != 0) return NULL;
    unsigned char *out = mi_alloc(len / 2 + 1);
    for (size_t i = 0; i < len; i += 2) {
        int high = 0;
        int low = 0;
        char a = text[i];
        char b = text[i + 1];
        if (a >= '0' && a <= '9') high = a - '0';
        else if (a >= 'a' && a <= 'f') high = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') high = a - 'A' + 10;
        else { free(out); return NULL; }
        if (b >= '0' && b <= '9') low = b - '0';
        else if (b >= 'a' && b <= 'f') low = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') low = b - 'A' + 10;
        else { free(out); return NULL; }
        out[i / 2] = (unsigned char)(high * 16 + low);
    }
    *out_len = len / 2;
    return out;
}

static void net_send_all(SOCKET client, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = send(client, data + sent, (int)(len - sent), 0);
        if (chunk <= 0) return;
        sent += (size_t)chunk;
    }
}

static MiValue host_serve(int port, MiValue handler, MiSpan span) {
    if (handler.kind != MI_STATION) {
        mi_raise("E0300", span, NULL, "`serve`'s handler must be a station, not %s",
                 mi_kind_name(handler));
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        mi_raise("E0309", span, NULL, "`serve` cannot start Winsock");
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        mi_raise("E0309", span, NULL, "`serve` cannot create a socket");
    }
    {
        BOOL reuse = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((u_short)(port < 0 ? 0 : port));
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        mi_raise("E0309", span, NULL, "`serve` cannot bind port %d", port);
    }
    if (listen(listener, 64) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        mi_raise("E0309", span, NULL, "`serve` cannot listen on port %d", port);
    }

    /* 端口传 0 时由系统分配，回报真实端口 */
    {
        struct sockaddr_in actual;
        int size = sizeof(actual);
        int real_port = port;
        if (getsockname(listener, (struct sockaddr *)&actual, &size) == 0) {
            real_port = ntohs(actual.sin_port);
        }
        char line[128];
        snprintf(line, sizeof(line), "listening on http://127.0.0.1:%d", real_port);
        output_line(line);
    }

    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;

        /* 先读到请求头结束 */
        NetBuf raw = {NULL, 0, 0};
        net_buf_append(&raw, "", 0);
        int header_done = 0;
        while (!header_done) {
            char chunk[4096];
            int got = recv(client, chunk, (int)sizeof(chunk), 0);
            if (got <= 0) break;
            net_buf_append(&raw, chunk, (size_t)got);
            if (strstr(raw.data, "\r\n\r\n")) header_done = 1;
            if (raw.len > 1u << 20) break;
        }

        char method[16] = "GET";
        char *path = mi_strdup("/");
        char *query = mi_strdup("");
        char *body = mi_strdup("");
        long content_length = 0;

        if (header_done) {
            char *header_end = strstr(raw.data, "\r\n\r\n");
            size_t header_len = (size_t)(header_end - raw.data) + 4;

            /* 请求行 */
            char *line_end = strstr(raw.data, "\r\n");
            if (line_end) {
                char *space1 = strchr(raw.data, ' ');
                if (space1 && space1 < line_end) {
                    size_t method_len = (size_t)(space1 - raw.data);
                    if (method_len < sizeof(method)) {
                        memcpy(method, raw.data, method_len);
                        method[method_len] = '\0';
                    }
                    char *target = space1 + 1;
                    char *space2 = strchr(target, ' ');
                    if (space2 && space2 < line_end) {
                        size_t target_len = (size_t)(space2 - target);
                        char *full = mi_strndup(target, target_len);
                        char *question = strchr(full, '?');
                        free(path);
                        free(query);
                        if (question) {
                            *question = '\0';
                            path = mi_strdup(full);
                            query = mi_strdup(question + 1);
                        } else {
                            path = mi_strdup(full);
                            query = mi_strdup("");
                        }
                        free(full);
                    }
                }
            }

            /* Content-Length */
            for (char *cursor = raw.data; cursor < header_end; cursor++) {
                if ((cursor == raw.data || cursor[-1] == '\n') &&
                    strncasecmp(cursor, "Content-Length:", 15) == 0) {
                    content_length = strtol(cursor + 15, NULL, 10);
                    break;
                }
            }

            /* 请求体：可能已经读到一部分 */
            size_t have = raw.len - header_len;
            free(body);
            body = mi_alloc((size_t)(content_length > 0 ? content_length : 0) + 1);
            size_t body_len = 0;
            if (have > 0) {
                body_len = have;
                memcpy(body, raw.data + header_len, have);
            }
            while (content_length > 0 && (long)body_len < content_length) {
                char chunk[4096];
                int got = recv(client, chunk, (int)sizeof(chunk), 0);
                if (got <= 0) break;
                if (body_len + (size_t)got > (size_t)content_length) {
                    got = (int)((size_t)content_length - body_len);
                }
                memcpy(body + body_len, chunk, (size_t)got);
                body_len += (size_t)got;
            }
            body[body_len] = '\0';
        }

        /* 组装请求并调用处理器 */
        MiAsm *request_asm = mi_asm_alloc(4);
        mi_asm_set(request_asm, "method", mi_text_new(method));
        mi_asm_set(request_asm, "path", mi_text_new(path));
        mi_asm_set(request_asm, "query", mi_text_new(query));
        mi_asm_set(request_asm, "body", mi_text_new(body));
        MiValue request_value = mi_asm_take(request_asm);

        int status = 200;
        char content_type[128] = "text/plain; charset=utf-8";
        char *response_bytes = NULL;
        size_t response_len = 0;

        /* 每个请求一份新的预算，否则长时间服务会把总预算耗光 */
        if (g_state) {
            g_state->steps = 0;
            g_state->depth = 0;
        }

        MiValue outcome = MI_NOTHING_VALUE;
        if (setjmp(g_state->unwind) == 0) {
            outcome = mi_invoke(handler, &request_value, 1, span);
        } else {
            /* 处理器抛错：当作 500 */
            char message[1024];
            snprintf(message, sizeof(message), "handler failed: [%s] %s", mi_err.code,
                     mi_err.message);
            mi_err.raised = 0;
            g_signal.kind = 0;
            mi_release(g_signal.value);
            g_signal.value = MI_NOTHING_VALUE;
            response_bytes = mi_strdup(message);
            response_len = strlen(response_bytes);
            status = 500;
            outcome = MI_NOTHING_VALUE;
        }

        if (!response_bytes) {
            if (outcome.kind == MI_TEXT) {
                response_bytes = mi_strndup(outcome.as.text->data, outcome.as.text->len);
                response_len = outcome.as.text->len;
            } else if (outcome.kind == MI_ASSEMBLY) {
                MiAsm *asmv = outcome.as.asmv;
                if (mi_asm_find(asmv, "status") >= 0) {
                    int64_t parsed = 0;
                    if (mi_as_int(mi_asm_get(asmv, "status"), &parsed)) status = (int)parsed;
                }
                if (mi_asm_find(asmv, "content_type") >= 0) {
                    char *shown = mi_display(mi_asm_get(asmv, "content_type"), 0);
                    snprintf(content_type, sizeof(content_type), "%s", shown);
                    free(shown);
                }
                if (mi_asm_find(asmv, "bytes") >= 0) {
                    MiValue raw_value = mi_asm_get(asmv, "bytes");
                    if (raw_value.kind == MI_TEXT) {
                        response_bytes = (char *)net_hex_decode(raw_value.as.text->data,
                                                                &response_len);
                        if (!response_bytes) {
                            mi_release(outcome);
                            mi_release(request_value);
                            free(raw.data);
                            free(path);
                            free(query);
                            free(body);
                            closesocket(client);
                            mi_raise("E0300", span, NULL,
                                     "`bytes` must be hexadecimal text");
                        }
                    }
                }
                if (!response_bytes && mi_asm_find(asmv, "body") >= 0) {
                    char *shown = mi_display(mi_asm_get(asmv, "body"), 0);
                    response_bytes = shown;
                    response_len = strlen(shown);
                }
                if (!response_bytes) {
                    response_bytes = mi_strdup("");
                    response_len = 0;
                }
            } else if (outcome.kind != MI_NOTHING) {
                response_bytes = mi_display(outcome, 0);
                response_len = strlen(response_bytes);
            } else {
                response_bytes = mi_strdup("");
                response_len = 0;
            }
        }

        /* 发响应（始终带 CORS，Mi Browser 从 file:// 也能调用） */
        {
            char header[1024];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: *\r\n"
                     "Access-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, status >= 400 ? "Error" : "OK", content_type, response_len);
            net_send_all(client, header, strlen(header));
            if (strcmp(method, "HEAD") != 0 && response_len > 0) {
                net_send_all(client, response_bytes, response_len);
            }
        }

        free(response_bytes);
        mi_release(outcome);
        mi_release(request_value);
        free(raw.data);
        free(path);
        free(query);
        free(body);
        shutdown(client, SD_SEND);
        closesocket(client);
    }

    closesocket(listener);
    WSACleanup();
    return MI_NOTHING_VALUE;
}

#else /* 非 Windows：这些宿主能力不可用 */

static MiValue host_http(const char *method, const char *url, const char *body,
                         int has_body, int safe, MiSpan span) {
    (void)method;
    (void)url;
    (void)body;
    (void)has_body;
    (void)safe;
    mi_raise("E0308", span, NULL, "network builtins are only implemented on Windows");
    return MI_NOTHING_VALUE;
}

static MiValue host_serve(int port, MiValue handler, MiSpan span) {
    (void)port;
    (void)handler;
    mi_raise("E0308", span, NULL, "`serve` is only implemented on Windows");
    return MI_NOTHING_VALUE;
}

#endif /* _WIN32 */

#endif /* MI_NETHOST_H */
