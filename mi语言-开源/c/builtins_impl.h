/* builtins_impl.h — 由 eval.c 直接 include（共享 MiBinding / mi_raise 等内部符号） */

#define mi_value_release mi_release

static int bi_lookup_name(const char *name) {
    if (!name || !*name) return -1;
    char low[160];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(low); i++) {
        char ch = name[i];
        low[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    low[i] = '\0';
    for (size_t k = 0; k < MI_BUILTIN_SYMBOL_ALIAS_COUNT; k++) {
        if (strcmp(MI_BUILTIN_SYMBOLS[k].spelling, name) == 0) return MI_BUILTIN_SYMBOLS[k].builtin;
    }
    for (size_t k = 0; k < MI_BUILTIN_ALL_ALIAS_COUNT; k++) {
        if (strcmp(MI_BUILTIN_ALL[k].spelling, low) == 0) return MI_BUILTIN_ALL[k].builtin;
    }
    return -1;
}

/* 按标签取值，其次按位置 */
static MiValue arg_of(MiBinding *args, size_t count, const char *label, size_t slot) {
    for (size_t i = 0; i < count; i++) {
        if (args[i].name && strcmp(args[i].name, label) == 0) return args[i].value;
    }
    size_t seen = 0;
    for (size_t i = 0; i < count; i++) {
        if (!args[i].name) {
            if (seen == slot) return args[i].value;
            seen++;
        }
    }
    return MI_NOTHING_VALUE;
}

static int64_t need_int(MiValue value, const char *what, MiSpan span) {
    int64_t out = 0;
    if (!mi_as_int(value, &out)) {
        mi_raise("E0300", span, NULL, "%s must be a whole number, not %s", what,
                 mi_kind_name(value));
    }
    return out;
}

static double need_number(MiValue value, const char *what, MiSpan span) {
    double out = 0;
    if (!mi_as_float(value, &out)) {
        mi_raise("E0300", span, NULL, "%s needs a number, not %s", what,
                 mi_kind_name(value));
    }
    return out;
}

static MiStr *need_text(MiValue value, const char *what, MiSpan span) {
    if (value.kind != MI_TEXT) {
        mi_raise("E0300", span, NULL, "%s must be text, not %s", what, mi_kind_name(value));
    }
    return value.as.text;
}

static MiList *need_list(MiValue value, const char *what, MiSpan span) {
    if (value.kind != MI_LIST) {
        mi_raise("E0300", span, NULL, "%s must be a list, not %s", what, mi_kind_name(value));
    }
    return value.as.list;
}

static MiValue number_from_double(double value) {
    if (value == floor(value) && fabs(value) < 9.0e18) return mi_int((int64_t)value);
    return mi_real(value);
}

/* ---- 变参实参收集（show / min / max / list）---- */
static MiList *collect_args(MiBinding *args, size_t count) {
    MiList *list = mi_list_alloc(count);
    for (size_t i = 0; i < count; i++) mi_list_push(list, mi_retain(args[i].value));
    return list;
}

/* ---- 排序（插入排序，稳定且不受 qsort 回调限制）---- */
static void sort_items(MiList *list) {
    for (size_t i = 1; i < list->len; i++) {
        MiValue key = list->items[i];
        size_t j = i;
        while (j > 0 && value_compare(list->items[j - 1], key) > 0) {
            list->items[j] = list->items[j - 1];
            j--;
        }
        list->items[j] = key;
    }
}

/* ---- SHA-256 ---- */
static void sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t padded = ((len + 9 + 63) / 64) * 64;
    unsigned char *buffer = mi_calloc(padded, 1);
    memcpy(buffer, data, len);
    buffer[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buffer[padded - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)buffer[chunk + i * 4] << 24) |
                   ((uint32_t)buffer[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)buffer[chunk + i * 4 + 2] << 8) |
                   (uint32_t)buffer[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^
                          ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
            uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^
                          ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^
                          ((e >> 25) | (e << 7));
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + s1 + ch + K[i] + w[i];
            uint32_t s0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^
                          ((a >> 22) | (a << 10));
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    free(buffer);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h[i];
    }
}

/* ---- 宿主：键值存储（单文件 JSON）---- */
static char *store_path(void) {
    const char *custom = getenv("MI_STORE_FILE");
    if (custom && *custom) return mi_strdup(custom);
    return mi_strdup(".mi-store.json");
}

static MiAsm *store_load(void) {
    char *path = store_path();
    char *source = mi_read_source(path);
    free(path);
    if (!source) return mi_asm_alloc(0);
    MiJson json = {source, 0, strlen(source), 0};
    MiValue value = json_value(&json);
    MiAsm *out = mi_asm_alloc(0);
    if (value.kind == MI_ASSEMBLY) {
        mi_release(mi_asm_take(out));
        out = mi_asm_copy(value.as.asmv);
        mi_release(value);
    } else {
        mi_release(value);
    }
    free(source);
    return out;
}

static void store_save(MiAsm *asmv) {
    char *path = store_path();
    MiValue value = mi_asm_take(mi_asm_copy(asmv));
    char *text = json_encode(value);
    mi_release(value);
    FILE *fh = fopen(path, "wb");
    if (fh) {
        fputs(text, fh);
        fclose(fh);
    }
    free(text);
    free(path);
}

#include "nethost.h"

/* ---- 主分派 ---- */
static MiValue call_builtin(int id, MiBinding *args, size_t count, MiSpan span) {
    if (id < 0 || id >= BI_COUNT) {
        mi_raise("E0203", span, NULL, "unknown builtin");
    }
    MiValue v0 = arg_of(args, count, "value", 0);
    switch (id) {
        case BI_SHOW: {
            /* 变参：把所有实参按顺序显示成一行 */
            size_t total = 0;
            for (size_t i = 0; i < count; i++) {
                char *piece = mi_display(args[i].value, 0);
                total += strlen(piece) + 1;
                free(piece);
            }
            char *line = mi_alloc(total + 1);
            line[0] = '\0';
            for (size_t i = 0; i < count; i++) {
                char *piece = mi_display(args[i].value, 0);
                if (i) strcat(line, " ");
                strcat(line, piece);
                free(piece);
            }
            output_line(line);
            free(line);
            return MI_NOTHING_VALUE;
        }
        case BI_LEN: {
            if (v0.kind == MI_TEXT) return mi_int((int64_t)mi_utf8_len(v0.as.text->data));
            if (v0.kind == MI_LIST) return mi_int((int64_t)v0.as.list->len);
            if (v0.kind == MI_ASSEMBLY) return mi_int((int64_t)v0.as.asmv->len);
            mi_raise("E0300", span, NULL,
                     "`len` needs text, a list or an assembly, not %s", mi_kind_name(v0));
            return MI_NOTHING_VALUE;
        }
        case BI_PUSH: {
            MiList *into = need_list(arg_of(args, count, "into", 0), "`push`'s `into`", span);
            MiList *list = mi_list_alloc(into->len + 1);
            for (size_t i = 0; i < into->len; i++) {
                mi_list_push(list, mi_retain(into->items[i]));
            }
            mi_list_push(list, mi_retain(arg_of(args, count, "value", 1)));
            return mi_list_take(list);
        }
        case BI_AT: {
            MiValue items = arg_of(args, count, "items", 0);
            int64_t index = need_int(arg_of(args, count, "index", 1), "`at`'s `index`", span);
            if (items.kind == MI_LIST) {
                int64_t len = (int64_t)items.as.list->len;
                int64_t real = index < 0 ? len + index : index;
                if (real < 0 || real >= len) {
                    mi_raise("E0300", span, NULL,
                             "index %lld is out of range for a list of length %lld",
                             (long long)index, (long long)len);
                }
                return mi_retain(items.as.list->items[real]);
            }
            if (items.kind == MI_TEXT) {
                int64_t chars = (int64_t)mi_utf8_len(items.as.text->data);
                int64_t real = index < 0 ? chars + index : index;
                if (real < 0 || real >= chars) {
                    mi_raise("E0300", span, NULL,
                             "index %lld is out of range for a text of length %lld",
                             (long long)index, (long long)chars);
                }
                size_t begin = mi_utf8_offset(items.as.text->data, (size_t)real);
                size_t end = mi_utf8_offset(items.as.text->data, (size_t)real + 1);
                return mi_text_n(items.as.text->data + begin, end - begin);
            }
            mi_raise("E0300", span, NULL, "`at` needs a list or text, not %s",
                     mi_kind_name(items));
            return MI_NOTHING_VALUE;
        }
        case BI_SLICE: {
            MiValue items = arg_of(args, count, "items", 0);
            int64_t from = need_int(arg_of(args, count, "from", 1), "`slice`'s `from`", span);
            MiValue to_value = arg_of(args, count, "to", 2);
            int64_t total = 0;
            if (items.kind == MI_LIST) total = (int64_t)items.as.list->len;
            else if (items.kind == MI_TEXT) total = (int64_t)mi_utf8_len(items.as.text->data);
            else mi_raise("E0300", span, NULL, "`slice` needs a list or text, not %s",
                          mi_kind_name(items));
            int64_t to = to_value.kind == MI_NOTHING ? total
                                                     : need_int(to_value, "`slice`'s `to`", span);
            if (from < 0) from = 0;
            if (to > total) to = total;
            if (to < from) to = from;
            if (items.kind == MI_LIST) {
                MiList *list = mi_list_alloc((size_t)(to - from));
                for (int64_t i = from; i < to; i++) {
                    mi_list_push(list, mi_retain(items.as.list->items[i]));
                }
                return mi_list_take(list);
            }
            size_t begin = mi_utf8_offset(items.as.text->data, (size_t)from);
            size_t end = mi_utf8_offset(items.as.text->data, (size_t)to);
            return mi_text_n(items.as.text->data + begin, end - begin);
        }
        case BI_MAP: {
            MiList *items = need_list(arg_of(args, count, "items", 0), "`map`'s `items`", span);
            MiValue fn = arg_of(args, count, "with", 1);
            MiList *out = mi_list_alloc(items->len);
            for (size_t i = 0; i < items->len; i++) {
                MiValue arg = items->items[i];
                mi_list_push(out, mi_invoke(fn, &arg, 1, span));
            }
            return mi_list_take(out);
        }
        case BI_FILTER: {
            MiList *items =
                need_list(arg_of(args, count, "items", 0), "`filter`'s `items`", span);
            MiValue fn = arg_of(args, count, "by", 1);
            if (fn.kind == MI_NOTHING) fn = arg_of(args, count, "with", 1);
            MiList *out = mi_list_alloc(items->len);
            for (size_t i = 0; i < items->len; i++) {
                MiValue arg = items->items[i];
                MiValue keep = mi_invoke(fn, &arg, 1, span);
                if (mi_truthy(keep)) mi_list_push(out, mi_retain(items->items[i]));
                mi_release(keep);
            }
            return mi_list_take(out);
        }
        case BI_FOLD: {
            MiList *items = need_list(arg_of(args, count, "items", 0), "`fold`'s `items`", span);
            MiValue acc = mi_retain(arg_of(args, count, "from", 1));
            MiValue fn = arg_of(args, count, "with", 2);
            for (size_t i = 0; i < items->len; i++) {
                MiValue pair[2];
                pair[0] = acc;
                pair[1] = items->items[i];
                MiValue next = mi_invoke(fn, pair, 2, span);
                mi_release(acc);
                acc = next;
            }
            return acc;
        }
        case BI_SPAN: {
            int64_t from = need_int(arg_of(args, count, "from", 0), "`span`'s `from`", span);
            int64_t to = need_int(arg_of(args, count, "to", 1), "`span`'s `to`", span);
            MiValue step_value = arg_of(args, count, "step", 2);
            int64_t step = step_value.kind == MI_NOTHING
                               ? 1
                               : need_int(step_value, "`span`'s `step`", span);
            if (step == 0) mi_raise("E0300", span, NULL, "`span`'s `step` cannot be zero");
            MiList *list = mi_list_alloc(0);
            if (step > 0) {
                for (int64_t i = from; i < to; i += step) mi_list_push(list, mi_int(i));
            } else {
                for (int64_t i = from; i > to; i += step) mi_list_push(list, mi_int(i));
            }
            return mi_list_take(list);
        }
        case BI_JOIN: {
            MiList *items = need_list(arg_of(args, count, "items", 0), "`join`'s `items`", span);
            MiValue sep_value = arg_of(args, count, "with", 1);
            MiStr *sep = sep_value.kind == MI_NOTHING ? NULL
                                                      : need_text(sep_value, "`join`'s separator", span);
            size_t total = 1;
            char **pieces = mi_calloc(items->len ? items->len : 1, sizeof(char *));
            for (size_t i = 0; i < items->len; i++) {
                pieces[i] = mi_display(items->items[i], 0);
                total += strlen(pieces[i]) + (sep ? sep->len : 0);
            }
            char *out = mi_alloc(total + 1);
            out[0] = '\0';
            for (size_t i = 0; i < items->len; i++) {
                if (i && sep) strcat(out, sep->data);
                strcat(out, pieces[i]);
                free(pieces[i]);
            }
            free(pieces);
            MiValue value = mi_text_new(out);
            free(out);
            return value;
        }
        case BI_SPLIT: {
            MiStr *text = need_text(arg_of(args, count, "text", 0), "`split`'s `text`", span);
            MiValue at_value = arg_of(args, count, "at", 1);
            MiStr *sep = need_text(at_value, "`split`'s separator", span);
            if (sep->len == 0) mi_raise("E0300", span, NULL, "`split` needs a non-empty separator");
            MiList *list = mi_list_alloc(0);
            const char *cursor = text->data;
            const char *found = NULL;
            while ((found = strstr(cursor, sep->data)) != NULL) {
                mi_list_push(list, mi_text_n(cursor, (size_t)(found - cursor)));
                cursor = found + sep->len;
            }
            mi_list_push(list, mi_text_new(cursor));
            return mi_list_take(list);
        }
        case BI_TEXT: {
            char *text = mi_display(v0, 0);
            MiValue out = mi_text_new(text);
            free(text);
            return out;
        }
        case BI_NUMBER: {
            if (v0.kind == MI_INT || v0.kind == MI_REAL || v0.kind == MI_BIG) {
                return mi_retain(v0);
            }
            if (v0.kind == MI_TEXT) {
                const char *text = v0.as.text->data;
                char *end = NULL;
                errno = 0;
                long long as_int = strtoll(text, &end, 0);
                if (end && *end == '\0' && end != text && errno != ERANGE) {
                    return mi_int(as_int);
                }
                double as_real = strtod(text, &end);
                if (end && *end == '\0' && end != text) {
                    /* 整数字面量超出 int64 时用大整数，别退化成 double */
                    if (strpbrk(text, ".eE") == NULL && strncasecmp(text, "0x", 2) != 0 &&
                        strncasecmp(text, "0b", 2) != 0) {
                        MiBig *big = mi_big_from_str(text);
                        if (big) return mi_value_from_big(big);
                    }
                    return mi_real(as_real);
                }
                mi_raise("E0300", span, NULL, "cannot read '%s' as a number", text);
            }
            mi_raise("E0300", span, NULL, "cannot convert %s to a number", mi_kind_name(v0));
            return MI_NOTHING_VALUE;
        }
        case BI_KIND:
            return mi_text_new(mi_kind_name(v0));
        case BI_HAS: {
            MiValue container = arg_of(args, count, "container", 0);
            MiValue needle = arg_of(args, count, "value", 1);
            if (container.kind == MI_LIST) {
                for (size_t i = 0; i < container.as.list->len; i++) {
                    if (mi_equal(container.as.list->items[i], needle)) return mi_flag(1);
                }
                return mi_flag(0);
            }
            if (container.kind == MI_ASSEMBLY) {
                MiStr *name = need_text(needle, "`has`'s `value`", span);
                return mi_flag(mi_asm_find(container.as.asmv, name->data) >= 0);
            }
            if (container.kind == MI_TEXT) {
                MiStr *needle_text = need_text(needle, "`has`'s `value`", span);
                return mi_flag(strstr(container.as.text->data, needle_text->data) != NULL);
            }
            mi_raise("E0300", span, NULL, "`has` needs a list, assembly or text, not %s",
                     mi_kind_name(container));
            return MI_NOTHING_VALUE;
        }
        case BI_KEYS: {
            MiValue target = arg_of(args, count, "target", 0);
            if (target.kind != MI_ASSEMBLY) {
                mi_raise("E0300", span, NULL, "`keys` needs an assembly, not %s",
                         mi_kind_name(target));
            }
            MiList *list = mi_list_alloc(target.as.asmv->len);
            for (size_t i = 0; i < target.as.asmv->len; i++) {
                mi_list_push(list, mi_text_new(target.as.asmv->names[i]));
            }
            return mi_list_take(list);
        }
        case BI_PLACE: {
            MiValue target = arg_of(args, count, "target", 0);
            if (target.kind != MI_ASSEMBLY) {
                mi_raise("E0300", span, NULL, "`place` needs an assembly, not %s",
                         mi_kind_name(target));
            }
            MiStr *field = need_text(arg_of(args, count, "field", 1), "`place`'s `field`", span);
            MiAsm *asmv = mi_asm_copy(target.as.asmv);
            mi_asm_set(asmv, field->data, mi_retain(arg_of(args, count, "value", 2)));
            return mi_asm_take(asmv);
        }
        case BI_LIST:
            return mi_list_take(collect_args(args, count));
        case BI_SORT: {
            MiList *items = need_list(arg_of(args, count, "items", 0), "`sort`'s `items`", span);
            MiList *list = mi_list_alloc(items->len);
            for (size_t i = 0; i < items->len; i++) {
                mi_list_push(list, mi_retain(items->items[i]));
            }
            sort_items(list);
            return mi_list_take(list);
        }
        case BI_REVERSE: {
            MiValue items = arg_of(args, count, "items", 0);
            if (items.kind == MI_LIST) {
                MiList *list = mi_list_alloc(items.as.list->len);
                for (size_t i = items.as.list->len; i > 0; i--) {
                    mi_list_push(list, mi_retain(items.as.list->items[i - 1]));
                }
                return mi_list_take(list);
            }
            if (items.kind == MI_TEXT) {
                size_t chars = mi_utf8_len(items.as.text->data);
                char *out = mi_alloc(items.as.text->len + 1);
                size_t pos = 0;
                for (size_t i = chars; i > 0; i--) {
                    size_t begin = mi_utf8_offset(items.as.text->data, i - 1);
                    size_t end = mi_utf8_offset(items.as.text->data, i);
                    memcpy(out + pos, items.as.text->data + begin, end - begin);
                    pos += end - begin;
                }
                out[pos] = '\0';
                MiValue value = mi_text_new(out);
                free(out);
                return value;
            }
            mi_raise("E0300", span, NULL, "`reverse` needs a list or text, not %s",
                     mi_kind_name(items));
            return MI_NOTHING_VALUE;
        }
        case BI_ABS:
            if (v0.kind == MI_BIG) return mi_value_take_big(mi_big_abs(v0.as.big));
            if (v0.kind == MI_INT) return mi_int(v0.as.i < 0 ? -v0.as.i : v0.as.i);
            if (v0.kind == MI_REAL) return mi_real(fabs(v0.as.r));
            mi_raise("E0300", span, NULL, "`abs` needs a number, not %s", mi_kind_name(v0));
            return MI_NOTHING_VALUE;
        case BI_FLOOR:
            return mi_int((int64_t)floor(need_number(v0, "`floor`", span)));
        case BI_CEIL:
            return mi_int((int64_t)ceil(need_number(v0, "`ceil`", span)));
        case BI_ROUND: {
            double value = need_number(v0, "`round`", span);
            MiValue digits = arg_of(args, count, "digits", 1);
            if (digits.kind == MI_NOTHING) return mi_int((int64_t)nearbyint(value));
            int64_t places = need_int(digits, "`round`'s `digits`", span);
            double scale = pow(10.0, (double)places);
            return mi_real(nearbyint(value * scale) / scale);
        }
        case BI_SQRT: {
            double value = need_number(v0, "`sqrt`", span);
            if (value < 0) {
                mi_raise("E0300", span, NULL,
                         "cannot take the square root of a negative number");
            }
            return mi_real(sqrt(value));
        }
        case BI_MIN:
        case BI_MAX: {
            MiList *values = collect_args(args, count);
            if (values->len == 1 && values->items[0].kind == MI_LIST) {
                MiList *inner = values->items[0].as.list;
                mi_release(mi_list_take(values));
                values = mi_list_alloc(inner->len);
                for (size_t i = 0; i < inner->len; i++) {
                    mi_list_push(values, mi_retain(inner->items[i]));
                }
            }
            if (values->len == 0) {
                mi_release(mi_list_take(values));
                mi_raise("E0300", span, NULL, "`%s` needs at least one value",
                         id == BI_MIN ? "min" : "max");
            }
            MiValue best = mi_retain(values->items[0]);
            for (size_t i = 1; i < values->len; i++) {
                int cmp = value_compare(values->items[i], best);
                if ((id == BI_MIN && cmp < 0) || (id == BI_MAX && cmp > 0)) {
                    mi_release(best);
                    best = mi_retain(values->items[i]);
                }
            }
            mi_release(mi_list_take(values));
            return best;
        }
        case BI_RANDOM: {
            MiValue from = arg_of(args, count, "from", 0);
            MiValue to = arg_of(args, count, "to", 1);
            int64_t low = from.kind == MI_NOTHING ? 0 : need_int(from, "`random`", span);
            int64_t high = to.kind == MI_NOTHING ? 1 : need_int(to, "`random`", span);
            if (high <= low) {
                mi_raise("E0300", span, NULL,
                         "`random` needs `to` greater than `from` for whole numbers");
            }
            return mi_int(low + (int64_t)(rand() % (int)(high - low)));
        }
        case BI_CLOCK: {
            return mi_real((double)time(NULL));
        }
        case BI_JSON_PARSE: {
            MiStr *text = need_text(arg_of(args, count, "text", 0), "`json.parse`'s `text`", span);
            MiJson json = {text->data, 0, text->len, 0};
            MiValue value = json_value(&json);
            if (json.failed) {
                mi_raise("E0300", span, NULL, "invalid JSON");
            }
            return value;
        }
        case BI_JSON_TEXT: {
            MiValue value = arg_of(args, count, "value", 0);
            char *text = json_encode(value);
            MiValue out = mi_text_new(text);
            free(text);
            return out;
        }
        /* ---------------- 宿主 ---------------- */
        case BI_FS_READ: {
            MiStr *path = need_text(arg_of(args, count, "path", 0), "`fs.read`'s `path`", span);
            char *source = mi_read_source(path->data);
            if (!source) {
                mi_raise("E0309", span, NULL, "cannot read %s", path->data);
            }
            MiValue out = mi_text_new(source);
            free(source);
            return out;
        }
        case BI_FS_WRITE: {
            MiStr *path = need_text(arg_of(args, count, "path", 0), "`fs.write`'s `path`", span);
            MiStr *text = need_text(arg_of(args, count, "text", 1), "`fs.write`'s `text`", span);
            FILE *fh = fopen(path->data, "wb");
            if (!fh) mi_raise("E0309", span, NULL, "cannot write %s", path->data);
            fwrite(text->data, 1, text->len, fh);
            fclose(fh);
            return MI_NOTHING_VALUE;
        }
        case BI_FS_EXISTS: {
            MiStr *path =
                need_text(arg_of(args, count, "path", 0), "`fs.exists`'s `path`", span);
            FILE *fh = fopen(path->data, "rb");
            if (fh) {
                fclose(fh);
                return mi_flag(1);
            }
            return mi_flag(0);
        }
        case BI_STORE_READ: {
            MiStr *name =
                need_text(arg_of(args, count, "name", 0), "`store.read`'s `name`", span);
            MiAsm *store = store_load();
            int index = mi_asm_find(store, name->data);
            MiValue out = index >= 0 ? mi_retain(store->values[index])
                                     : mi_retain(arg_of(args, count, "fallback", 1));
            mi_release(mi_asm_take(store));
            return out;
        }
        case BI_STORE_WRITE: {
            MiStr *name =
                need_text(arg_of(args, count, "name", 0), "`store.write`'s `name`", span);
            MiAsm *store = store_load();
            mi_asm_set(store, name->data, mi_retain(arg_of(args, count, "value", 1)));
            store_save(store);
            mi_release(mi_asm_take(store));
            return MI_NOTHING_VALUE;
        }
        case BI_STORE_CLEAR: {
            MiValue name = arg_of(args, count, "name", 0);
            MiAsm *store = store_load();
            if (name.kind == MI_TEXT) {
                int index = mi_asm_find(store, name.as.text->data);
                if (index >= 0) {
                    int last = (int)store->len - 1;
                    free(store->names[index]);
                    mi_release(store->values[index]);
                    store->names[index] = store->names[last];
                    store->values[index] = store->values[last];
                    store->len--;
                }
            } else {
                mi_release(mi_asm_take(store));
                store = mi_asm_alloc(0);
            }
            store_save(store);
            mi_release(mi_asm_take(store));
            return MI_NOTHING_VALUE;
        }
        case BI_CRYPTO_SHA256: {
            MiStr *text =
                need_text(arg_of(args, count, "text", 0), "`crypto.sha256`'s `text`", span);
            unsigned char digest[32];
            sha256((const unsigned char *)text->data, text->len, digest);
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            return mi_text_new(hex);
        }
        case BI_ENV: {
            MiStr *name = need_text(arg_of(args, count, "name", 0), "`env`'s `name`", span);
            const char *value = getenv(name->data);
            if (value) return mi_text_new(value);
            return mi_retain(arg_of(args, count, "fallback", 1));
        }
        case BI_ARGV:
            return mi_list_new();
        case BI_NET_GET: {
            MiStr *url =
                need_text(arg_of(args, count, "url", 0), "`net.get`'s `url`", span);
            MiValue safe_value = arg_of(args, count, "safe", 1);
            int safe = safe_value.kind != MI_NOTHING && mi_truthy(safe_value);
            return host_http("GET", url->data, NULL, 0, safe, span);
        }
        case BI_NET_POST: {
            MiStr *url =
                need_text(arg_of(args, count, "url", 0), "`net.post`'s `url`", span);
            MiStr *payload =
                need_text(arg_of(args, count, "body", 1), "`net.post`'s `body`", span);
            MiValue safe_value = arg_of(args, count, "safe", 2);
            int safe = safe_value.kind != MI_NOTHING && mi_truthy(safe_value);
            return host_http("POST", url->data, payload->data, 1, safe, span);
        }
        case BI_SERVE: {
            int64_t port =
                need_int(arg_of(args, count, "port", 0), "`serve`'s `port`", span);
            MiValue handler = arg_of(args, count, "handler", 1);
            return host_serve((int)port, handler, span);
        }
        case BI_ASK: {
            MiValue prompt = arg_of(args, count, "prompt", 0);
            if (prompt.kind != MI_NOTHING) {
                char *text = mi_display(prompt, 0);
                fputs(text, stdout);
                fflush(stdout);
                free(text);
            }
            char line[1024];
            if (!fgets(line, sizeof(line), stdin)) return mi_text_new("");
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
            return mi_text_new(line);
        }
        default: {
            const char *canonical = (id >= 0 && id < BI_COUNT) ? MI_BUILTIN_NAMES[id] : "?";
            mi_raise("E0308", span, NULL,
                     "builtin `%s` has no implementation on this host", canonical);
            return MI_NOTHING_VALUE;
        }
    }
}
