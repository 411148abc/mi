/* value.c — MI 值模型：引用计数、显示、相等、比较、运算符 */
#include "mi.h"
#include "tables.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

MiValue MI_NOTHING_VALUE = {MI_NOTHING, {0}};

/* ------------------------------------------------------------ 引用计数 */
static void mi_str_free(MiStr *text) {
    if (!text) return;
    free(text->data);
    free(text);
}

static void mi_list_free(MiList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->len; i++) mi_release(list->items[i]);
    free(list->items);
    free(list);
}

static void mi_asm_free(MiAsm *asmv) {
    if (!asmv) return;
    for (size_t i = 0; i < asmv->len; i++) {
        free(asmv->names[i]);
        mi_release(asmv->values[i]);
    }
    free(asmv->names);
    free(asmv->values);
    free(asmv);
}

MiValue mi_retain(MiValue value) {
    switch (value.kind) {
        case MI_TEXT:
            value.as.text->refs++;
            break;
        case MI_LIST:
            value.as.list->refs++;
            break;
        case MI_ASSEMBLY:
            value.as.asmv->refs++;
            break;
        case MI_STATION:
            value.as.station->refs++;
            break;
        case MI_BIG: {
            /* MiBig 没有引用计数（bigint 模块是独立的），按值复制最安全 */
            MiValue copy = value;
            copy.as.big = mi_big_copy(value.as.big);
            return copy;
        }
        default:
            break;
    }
    return value;
}

void mi_release(MiValue value) {
    switch (value.kind) {
        case MI_TEXT:
            if (--value.as.text->refs <= 0) mi_str_free(value.as.text);
            break;
        case MI_LIST:
            if (--value.as.list->refs <= 0) mi_list_free(value.as.list);
            break;
        case MI_ASSEMBLY:
            if (--value.as.asmv->refs <= 0) mi_asm_free(value.as.asmv);
            break;
        case MI_STATION:
            if (--value.as.station->refs <= 0) free(value.as.station);
            break;
        case MI_BIG:
            mi_big_free(value.as.big);
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------- 构造 */
MiValue mi_flag(int value) {
    MiValue out;
    out.kind = MI_FLAG;
    out.as.flag = value ? 1 : 0;
    return out;
}

MiValue mi_int(int64_t value) {
    MiValue out;
    out.kind = MI_INT;
    out.as.i = value;
    return out;
}

MiValue mi_real(double value) {
    MiValue out;
    out.kind = MI_REAL;
    out.as.r = value;
    return out;
}

MiStr *mi_str_alloc(size_t len) {
    MiStr *text = mi_alloc(sizeof(MiStr));
    text->data = mi_alloc(len + 1);
    text->data[len] = '\0';
    text->len = len;
    text->chars = -1;
    text->refs = 1;
    return text;
}

MiValue mi_text_take(MiStr *text) {
    MiValue out;
    out.kind = MI_TEXT;
    out.as.text = text;
    return out;
}

MiValue mi_text_n(const char *text, size_t len) {
    MiStr *str = mi_str_alloc(len);
    if (len) memcpy(str->data, text, len);
    return mi_text_take(str);
}

MiValue mi_text_new(const char *text) {
    return mi_text_n(text ? text : "", text ? strlen(text) : 0);
}

MiList *mi_list_alloc(size_t cap) {
    MiList *list = mi_alloc(sizeof(MiList));
    list->items = cap ? mi_calloc(cap, sizeof(MiValue)) : NULL;
    list->len = 0;
    list->refs = 1;
    return list;
}

MiValue mi_list_take(MiList *list) {
    MiValue out;
    out.kind = MI_LIST;
    out.as.list = list;
    return out;
}

MiValue mi_list_new(void) { return mi_list_take(mi_list_alloc(0)); }

void mi_list_push(MiList *list, MiValue value) {
    list->items = mi_realloc(list->items, (list->len + 1) * sizeof(MiValue));
    list->items[list->len++] = value;
}

MiAsm *mi_asm_alloc(size_t cap) {
    MiAsm *asmv = mi_alloc(sizeof(MiAsm));
    asmv->names = cap ? mi_calloc(cap, sizeof(char *)) : NULL;
    asmv->values = cap ? mi_calloc(cap, sizeof(MiValue)) : NULL;
    asmv->len = 0;
    asmv->refs = 1;
    return asmv;
}

MiValue mi_asm_take(MiAsm *asmv) {
    MiValue out;
    out.kind = MI_ASSEMBLY;
    out.as.asmv = asmv;
    return out;
}

MiValue mi_asm_new(void) { return mi_asm_take(mi_asm_alloc(0)); }

int mi_asm_find(const MiAsm *asmv, const char *name) {
    for (size_t i = 0; i < asmv->len; i++) {
        if (strcmp(asmv->names[i], name) == 0) return (int)i;
    }
    return -1;
}

MiValue mi_asm_get(const MiAsm *asmv, const char *name) {
    int index = mi_asm_find(asmv, name);
    return index < 0 ? MI_NOTHING_VALUE : asmv->values[index];
}

/* 同名覆盖，否则追加（对应 Assembly.from_pairs） */
void mi_asm_set(MiAsm *asmv, const char *name, MiValue value) {
    int index = mi_asm_find(asmv, name);
    if (index >= 0) {
        mi_release(asmv->values[index]);
        asmv->values[index] = value;
        return;
    }
    asmv->names = mi_realloc(asmv->names, (asmv->len + 1) * sizeof(char *));
    asmv->values = mi_realloc(asmv->values, (asmv->len + 1) * sizeof(MiValue));
    asmv->names[asmv->len] = mi_strdup(name);
    asmv->values[asmv->len] = value;
    asmv->len++;
}

MiAsm *mi_asm_copy(const MiAsm *asmv) {
    MiAsm *out = mi_asm_alloc(asmv->len);
    for (size_t i = 0; i < asmv->len; i++) {
        out->names[i] = mi_strdup(asmv->names[i]);
        out->values[i] = mi_retain(asmv->values[i]);
    }
    out->len = asmv->len;
    return out;
}

MiValue mi_value_take_big(MiBig *value) {
    MiValue out;
    out.kind = MI_BIG;
    out.as.big = value;
    return out;
}

MiBig *mi_value_to_big(MiValue value) {
    if (value.kind == MI_BIG) return mi_big_copy(value.as.big);
    if (value.kind == MI_INT) return mi_big_from_i64(value.as.i);
    return NULL;
}

MiValue mi_value_from_big(MiBig *value) {
    int64_t out = 0;
    if (mi_big_fits_i64(value, &out)) {
        mi_big_free(value);
        return mi_int(out);
    }
    return mi_value_take_big(value);
}

MiValue mi_builtin(int id) {
    MiValue out;
    out.kind = MI_BUILTIN;
    out.as.builtin = id;
    return out;
}

MiValue mi_station_ref(MiStationRef *ref) {
    MiValue out;
    out.kind = MI_STATION;
    out.as.station = ref;
    return out;
}

/* ---------------------------------------------------------------- 类别 */
const char *mi_kind_name(MiValue value) {
    switch (value.kind) {
        case MI_NOTHING: return "nothing";
        case MI_FLAG: return "flag";
        case MI_INT:
        case MI_REAL: return "number";
        case MI_TEXT: return "text";
        case MI_LIST: return "list";
        case MI_ASSEMBLY: return "assembly";
        case MI_STATION: return "station";
        case MI_BUILTIN: return "builtin";
        case MI_BIG: return "number";
    }
    return "unknown";
}

static long mi_text_chars(MiStr *text) {
    if (text->chars < 0) text->chars = (long)mi_utf8_len(text->data);
    return text->chars;
}

int mi_truthy(MiValue value) {
    switch (value.kind) {
        case MI_NOTHING: return 0;
        case MI_FLAG: return value.as.flag;
        case MI_INT: return value.as.i != 0;
        case MI_REAL: return value.as.r != 0.0;
        case MI_BIG: return !mi_big_is_zero(value.as.big);
        case MI_TEXT: return value.as.text->len > 0;
        case MI_LIST: return value.as.list->len > 0;
        case MI_ASSEMBLY: return value.as.asmv->len > 0;
        default: return 1;
    }
}

int mi_equal(MiValue a, MiValue b) {
    int an = (a.kind == MI_INT || a.kind == MI_REAL || a.kind == MI_BIG);
    int bn = (b.kind == MI_INT || b.kind == MI_REAL || b.kind == MI_BIG);
    if (an && bn) {
        if (a.kind == MI_BIG || b.kind == MI_BIG) {
            MiBig *x = mi_value_to_big(a);
            MiBig *y = mi_value_to_big(b);
            if (!x || !y) {
                mi_big_free(x);
                mi_big_free(y);
                return 0;
            }
            int cmp = mi_big_cmp(x, y);
            mi_big_free(x);
            mi_big_free(y);
            return cmp == 0;
        }
        if (a.kind == MI_INT && b.kind == MI_INT) return a.as.i == b.as.i;
        double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
        double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
        return x == y;
    }
    if (a.kind != b.kind) return 0;
    switch (a.kind) {
        case MI_NOTHING: return 1;
        case MI_FLAG: return a.as.flag == b.as.flag;
        case MI_TEXT:
            return a.as.text->len == b.as.text->len &&
                   memcmp(a.as.text->data, b.as.text->data, a.as.text->len) == 0;
        case MI_LIST: {
            if (a.as.list->len != b.as.list->len) return 0;
            for (size_t i = 0; i < a.as.list->len; i++) {
                if (!mi_equal(a.as.list->items[i], b.as.list->items[i])) return 0;
            }
            return 1;
        }
        case MI_ASSEMBLY: {
            if (a.as.asmv->len != b.as.asmv->len) return 0;
            for (size_t i = 0; i < a.as.asmv->len; i++) {
                if (strcmp(a.as.asmv->names[i], b.as.asmv->names[i]) != 0) return 0;
                if (!mi_equal(a.as.asmv->values[i], b.as.asmv->values[i])) return 0;
            }
            return 1;
        }
        case MI_STATION: return a.as.station == b.as.station;
        case MI_BUILTIN: return a.as.builtin == b.as.builtin;
        default: return 0;
    }
}

/* ---------------------------------------------------------------- 显示 */
/* Python repr 风格的浮点文本：最短往返 + 定点/科学记数切换。 */
static char *mi_float_repr(double value) {
    if (isnan(value)) return mi_strdup("nan");
    if (isinf(value)) return mi_strdup(value > 0 ? "inf" : "-inf");

    char buffer[64];
    int sig = 0;
    for (int p = 1; p <= 17; p++) {
        snprintf(buffer, sizeof(buffer), "%.*e", p - 1, value);
        if (strtod(buffer, NULL) == value) {
            sig = p;
            break;
        }
    }
    if (sig == 0) {
        sig = 17;
        snprintf(buffer, sizeof(buffer), "%.16e", value);
    }

    /* buffer 形如 "-1.2345e+06"；提取数字与指数 */
    int negative = buffer[0] == '-';
    char *body = buffer + (negative ? 1 : 0);
    char digits[32];
    int ndigits = 0;
    char *cursor = body;
    while (*cursor && *cursor != 'e' && *cursor != 'E') {
        if (*cursor >= '0' && *cursor <= '9') digits[ndigits++] = *cursor;
        cursor++;
    }
    digits[ndigits] = '\0';
    int exponent = 0;
    if (*cursor == 'e' || *cursor == 'E') exponent = atoi(cursor + 1);
    int decpt = exponent + 1;

    /* 去掉尾随 0，保持至少 1 位 */
    while (ndigits > 1 && digits[ndigits - 1] == '0') ndigits--;
    digits[ndigits] = '\0';

    char out[512];
    size_t pos = 0;
    if (negative) out[pos++] = '-';

    if (decpt > -4 && decpt <= 16) {
        /* 定点 */
        if (decpt <= 0) {
            out[pos++] = '0';
            out[pos++] = '.';
            for (int i = 0; i < -decpt; i++) out[pos++] = '0';
            for (int i = 0; i < ndigits; i++) out[pos++] = digits[i];
        } else if (decpt < ndigits) {
            for (int i = 0; i < decpt; i++) out[pos++] = digits[i];
            out[pos++] = '.';
            for (int i = decpt; i < ndigits; i++) out[pos++] = digits[i];
        } else {
            for (int i = 0; i < ndigits; i++) out[pos++] = digits[i];
            for (int i = 0; i < decpt - ndigits; i++) out[pos++] = '0';
            out[pos++] = '.';
            out[pos++] = '0';
        }
    } else {
        /* 科学记数 */
        out[pos++] = digits[0];
        if (ndigits > 1) {
            out[pos++] = '.';
            for (int i = 1; i < ndigits; i++) out[pos++] = digits[i];
        }
        out[pos++] = 'e';
        int shown = decpt - 1;
        if (shown >= 0) {
            out[pos++] = '+';
        } else {
            out[pos++] = '-';
            shown = -shown;
        }
        char expbuf[16];
        snprintf(expbuf, sizeof(expbuf), "%02d", shown);
        for (char *e = expbuf; *e; e++) out[pos++] = *e;
    }
    out[pos] = '\0';
    return mi_strdup(out);
}

char *mi_number_text(MiValue value) {
    if (value.kind == MI_BIG) return mi_big_to_str(value.as.big);
    if (value.kind == MI_INT) return mi_asprintf("%lld", (long long)value.as.i);
    double real = value.as.r;
    if (isnan(real)) return mi_strdup("nan");
    if (isinf(real)) return mi_strdup(real > 0 ? "inf" : "-inf");
    if (real == floor(real) && fabs(real) < 1e16) {
        return mi_asprintf("%lld", (long long)real);
    }
    return mi_float_repr(real);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MiBuf;

static void buf_reserve(MiBuf *buf, size_t extra) {
    if (buf->len + extra + 1 <= buf->cap) return;
    size_t cap = buf->cap ? buf->cap : 32;
    while (cap < buf->len + extra + 1) cap *= 2;
    buf->data = mi_realloc(buf->data, cap);
    buf->cap = cap;
}

static void buf_append(MiBuf *buf, const char *text) {
    size_t len = strlen(text);
    buf_reserve(buf, len);
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void buf_append_n(MiBuf *buf, const char *text, size_t len) {
    buf_reserve(buf, len);
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void buf_append_char(MiBuf *buf, char ch) {
    buf_reserve(buf, 1);
    buf->data[buf->len++] = ch;
    buf->data[buf->len] = '\0';
}

static void mi_display_into(MiBuf *buf, MiValue value, int nested) {
    switch (value.kind) {
        case MI_NOTHING:
            buf_append(buf, "nothing");
            break;
        case MI_FLAG:
            buf_append(buf, value.as.flag ? "on" : "off");
            break;
        case MI_INT:
        case MI_REAL:
        case MI_BIG: {
            char *text = mi_number_text(value);
            buf_append(buf, text);
            free(text);
            break;
        }
        case MI_TEXT:
            if (!nested) {
                buf_append_n(buf, value.as.text->data, value.as.text->len);
            } else {
                buf_append_char(buf, '"');
                for (size_t i = 0; i < value.as.text->len; i++) {
                    char ch = value.as.text->data[i];
                    if (ch == '"') {
                        buf_append(buf, "\\\"");
                    } else if (ch == '\\') {
                        buf_append(buf, "\\\\");
                    } else if (ch == '\n') {
                        buf_append(buf, "\\n");
                    } else if (ch == '\t') {
                        buf_append(buf, "\\t");
                    } else if (ch == '\r') {
                        buf_append(buf, "\\r");
                    } else {
                        buf_append_char(buf, ch);
                    }
                }
                buf_append_char(buf, '"');
            }
            break;
        case MI_LIST:
            buf_append_char(buf, '[');
            for (size_t i = 0; i < value.as.list->len; i++) {
                if (i) buf_append(buf, ", ");
                mi_display_into(buf, value.as.list->items[i], 1);
            }
            buf_append_char(buf, ']');
            break;
        case MI_ASSEMBLY:
            buf_append(buf, "assembly{");
            for (size_t i = 0; i < value.as.asmv->len; i++) {
                if (i) buf_append(buf, ", ");
                buf_append(buf, value.as.asmv->names[i]);
                buf_append(buf, ": ");
                mi_display_into(buf, value.as.asmv->values[i], 1);
            }
            buf_append_char(buf, '}');
            break;
        case MI_STATION:
            buf_append(buf, "station:");
            if (value.as.station && value.as.station->decl && value.as.station->decl->name) {
                buf_append(buf, value.as.station->decl->name);
            }
            break;
        case MI_BUILTIN:
            buf_append(buf, "builtin:");
            buf_append(buf, MI_BUILTIN_NAMES[value.as.builtin]);
            break;
    }
}

char *mi_display(MiValue value, int nested) {
    MiBuf buf = {NULL, 0, 0};
    mi_display_into(&buf, value, nested);
    if (!buf.data) return mi_strdup("");
    return buf.data;
}

/* ------------------------------------------------------------ 数值读取 */
int mi_as_int(MiValue value, int64_t *out) {
    if (value.kind == MI_INT) {
        *out = value.as.i;
        return 1;
    }
    if (value.kind == MI_BIG) return mi_big_fits_i64(value.as.big, out);
    if (value.kind == MI_REAL) {
        double real = value.as.r;
        if (real == floor(real) && fabs(real) < 9.0e18) {
            *out = (int64_t)real;
            return 1;
        }
    }
    return 0;
}

int mi_as_float(MiValue value, double *out) {
    if (value.kind == MI_INT) {
        *out = (double)value.as.i;
        return 1;
    }
    if (value.kind == MI_BIG) {
        *out = mi_big_to_double(value.as.big);
        return 1;
    }
    if (value.kind == MI_REAL) {
        *out = value.as.r;
        return 1;
    }
    return 0;
}

MiValue mi_number_from_double(double value) {
    return mi_real(value);
}
