/* util.c — 内存、UTF-8、诊断、拼写建议 */
#include "mi.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ------------------------------------------------------------------ 内存 */
static void mi_oom(void) {
    fputs("out of memory\n", stderr);
    exit(70);
}

void *mi_alloc(size_t bytes) {
    void *ptr = malloc(bytes ? bytes : 1);
    if (!ptr) mi_oom();
    return ptr;
}

void *mi_calloc(size_t count, size_t size) {
    void *ptr = calloc(count ? count : 1, size ? size : 1);
    if (!ptr) mi_oom();
    return ptr;
}

void *mi_realloc(void *ptr, size_t bytes) {
    void *out = realloc(ptr, bytes ? bytes : 1);
    if (!out) mi_oom();
    return out;
}

char *mi_strdup(const char *text) {
    if (!text) return NULL;
    size_t len = strlen(text);
    char *out = mi_alloc(len + 1);
    memcpy(out, text, len + 1);
    return out;
}

char *mi_strndup(const char *text, size_t len) {
    char *out = mi_alloc(len + 1);
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

char *mi_asprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (need < 0) need = 0;
    char *out = mi_alloc((size_t)need + 1);
    vsnprintf(out, (size_t)need + 1, fmt, args);
    va_end(args);
    return out;
}

/* ----------------------------------------------------------------- UTF-8 */
int mi_utf8_next(const char *text, size_t len, size_t *consumed) {
    const unsigned char *ptr = (const unsigned char *)text;
    if (len == 0) {
        *consumed = 0;
        return -1;
    }
    unsigned char first = ptr[0];
    if (first < 0x80) {
        *consumed = 1;
        return (int)first;
    }
    if ((first & 0xE0) == 0xC0) {
        if (len < 2) goto bad;
        *consumed = 2;
        return ((first & 0x1F) << 6) | (ptr[1] & 0x3F);
    }
    if ((first & 0xF0) == 0xE0) {
        if (len < 3) goto bad;
        *consumed = 3;
        return ((first & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
    }
    if ((first & 0xF8) == 0xF0) {
        if (len < 4) goto bad;
        *consumed = 4;
        return ((first & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
               ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
    }
bad:
    *consumed = 1;
    return -1;
}

size_t mi_utf8_len(const char *text) {
    size_t bytes = strlen(text);
    size_t count = 0;
    size_t pos = 0;
    while (pos < bytes) {
        size_t step = 0;
        mi_utf8_next(text + pos, bytes - pos, &step);
        pos += step;
        count++;
    }
    return count;
}

size_t mi_utf8_offset(const char *text, size_t index) {
    size_t bytes = strlen(text);
    size_t pos = 0;
    size_t count = 0;
    while (pos < bytes && count < index) {
        size_t step = 0;
        mi_utf8_next(text + pos, bytes - pos, &step);
        pos += step;
        count++;
    }
    return pos;
}

size_t mi_utf8_encode(unsigned code, char out[4]) {
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (code >> 18));
    out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

/* ------------------------------------------------------- UTF-8 路径开文件 */
/* Windows 的 fopen 走 ANSI 代码页，项目路径含中文时必须用 _wfopen。 */
FILE *mi_fopen_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[2048];
    wchar_t wmode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 2048) == 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16) == 0) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

/* ------------------------------------------------------------ 读源文件 */
/* 与 Python 的默认文本模式一致：先把 CRLF / 孤立 CR 统一成 LF，再交给词法器。 */
char *mi_read_source(const char *path) {
    FILE *fh = mi_fopen_utf8(path, "rb");
    if (!fh) return NULL;
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (size < 0) {
        fclose(fh);
        return NULL;
    }
    char *raw = mi_alloc((size_t)size + 1);
    size_t got = fread(raw, 1, (size_t)size, fh);
    raw[got] = '\0';
    fclose(fh);

    char *out = mi_alloc(got + 1);
    size_t pos = 0;
    /* Python 的 CLI 用 utf-8-sig 读文件，会剥掉开头的 BOM，这里保持一致 */
    size_t begin = 0;
    if (got >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        begin = 3;
    }
    for (size_t i = begin; i < got; i++) {
        if (raw[i] == '\r') {
            if (i + 1 < got && raw[i + 1] == '\n') continue;
            out[pos++] = '\n';
        } else {
            out[pos++] = raw[i];
        }
    }
    out[pos] = '\0';
    free(raw);
    return out;
}

/* ----------------------------------------------------------------- 诊断 */
void mi_diag_init(MiDiagBag *bag) {
    memset(bag, 0, sizeof(*bag));
}

void mi_diag_add(MiDiagBag *bag, const char *code, MiSpan span, const char *hint,
                 int is_warning, const char *fmt, ...) {
    MiDiag *diag = mi_alloc(sizeof(MiDiag));
    memset(diag, 0, sizeof(*diag));
    snprintf(diag->code, sizeof(diag->code), "%s", code ? code : "E0000");
    diag->span = span;
    diag->is_warning = is_warning;

    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    diag->message = mi_strdup(buffer);
    diag->hint = hint ? mi_strdup(hint) : NULL;

    if (bag->tail) {
        bag->tail->next = diag;
        bag->tail = diag;
    } else {
        bag->head = bag->tail = diag;
    }
    if (is_warning) {
        bag->warnings++;
    } else {
        bag->errors++;
    }
}

void mi_diag_free(MiDiagBag *bag) {
    MiDiag *diag = bag->head;
    while (diag) {
        MiDiag *next = diag->next;
        free(diag->message);
        free(diag->hint);
        free(diag);
        diag = next;
    }
    memset(bag, 0, sizeof(*bag));
}

static int mi_lcs_len(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la == 0 || lb == 0) return 0;
    int *prev = mi_calloc(lb + 1, sizeof(int));
    int *cur = mi_calloc(lb + 1, sizeof(int));
    for (size_t i = 1; i <= la; i++) {
        cur[0] = 0;
        for (size_t j = 1; j <= lb; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
            } else {
                cur[j] = cur[j - 1] > prev[j] ? cur[j - 1] : prev[j];
            }
        }
        int *swap = prev;
        prev = cur;
        cur = swap;
    }
    int out = prev[lb];
    free(prev);
    free(cur);
    return out;
}

static void mi_lower_into(const char *src, char *dst, size_t size) {
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++) {
        char ch = src[i];
        dst[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    dst[i] = '\0';
}

const char *mi_suggest(const char *word, const char *const *candidates, int count,
                       double cutoff) {
    if (!word || !*word || count <= 0) return NULL;
    char low[128];
    mi_lower_into(word, low, sizeof(low));
    size_t word_len = strlen(low);

    const char *best = NULL;
    double best_score = cutoff;
    for (int i = 0; i < count; i++) {
        const char *candidate = candidates[i];
        if (!candidate) continue;
        char cbuf[128];
        mi_lower_into(candidate, cbuf, sizeof(cbuf));
        size_t cand_len = strlen(cbuf);
        if (cand_len == 0) continue;
        int lcs = mi_lcs_len(low, cbuf);
        double score = 2.0 * (double)lcs / (double)(word_len + cand_len);
        if (score > best_score) {
            best_score = score;
            best = candidate;
        }
    }
    return best;
}

/* ------------------------------------------------------------ 运行时错误 */
MiError mi_err;

void mi_raise(const char *code, MiSpan span, const char *hint, const char *fmt, ...) {
    snprintf(mi_err.code, sizeof(mi_err.code), "%s", code ? code : "E0300");
    va_list args;
    va_start(args, fmt);
    vsnprintf(mi_err.message, sizeof(mi_err.message), fmt, args);
    va_end(args);
    if (hint) {
        snprintf(mi_err.hint, sizeof(mi_err.hint), "%s", hint);
    } else {
        mi_err.hint[0] = '\0';
    }
    mi_err.span = span;
    mi_err.has_span = (span.start.line > 0);
    mi_err.raised = 1;
    /* 用 longjmp 在 eval.c 里捕获 */
    extern void mi_unwind(void);
    mi_unwind();
    /* 不会走到这里 */
    abort();
}
