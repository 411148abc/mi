/* lexer.c — MI 词法分析器
 *
 * 由 c/tables.h 的拼写表驱动：所有关键字/运算符/字面量/符号内置都在表里，
 * 按拼写长度降序最长匹配；单词拼写大小写不敏感且要求词边界。
 *
 * 位置语义与 Python 版一致：`line`/`col`/`offset` 都以 **Unicode 码点** 计数，
 * 不是字节。所以内部同时维护字节下标（切文本用）与码点下标（位置用）。
 */
#include "mi.h"
#include "tables.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MI_PUNCTUATION "(){}[]⟨⟩,:.;="

typedef struct {
    const char *src;
    size_t len;      /* 字节长度 */
    size_t pos;      /* 字节下标 */
    size_t cp;       /* 码点下标（= Position.offset） */
    int line;
    int col;
    const char *filename;
    MiDiagBag *bag;
    MiTokList *tokens;
} MiLexer;

typedef struct {
    MiPos pos;
    size_t byte;
} MiMark;

/* --------------------------------------------------------------- 工具 */
/* 码点是否算「词字符」。与 Python 的 str.isalnum() 近似对齐：
 * ASCII 字母数字下划线、CJK / 假名 / 谚文 / 希腊 / 西里尔 / 拉丁扩展算词字符；
 * 箭头、数学符号、几何图形、标点（¬ ≤ ▷ ■ ⟲ → § ¤ « »）不算。 */
static int mi_cp_is_word(unsigned code) {
    if (code < 0x80) {
        return (code >= '0' && code <= '9') || (code >= 'a' && code <= 'z') ||
               (code >= 'A' && code <= 'Z') || code == '_';
    }
    if (code >= 0x00C0 && code <= 0x024F && code != 0x00D7 && code != 0x00F7) return 1;
    if (code >= 0x0370 && code <= 0x052F) return 1; /* 希腊 + 西里尔 */
    if (code >= 0x1E00 && code <= 0x1FFF) return 1;
    if (code >= 0x2C60 && code <= 0x2C7F) return 1;
    if (code >= 0x3040 && code <= 0x30FF) return 1; /* 假名 */
    if (code >= 0x3400 && code <= 0x4DBF) return 1;
    if (code >= 0x4E00 && code <= 0x9FFF) return 1; /* CJK */
    if (code >= 0xAC00 && code <= 0xD7AF) return 1; /* 谚文 */
    if (code >= 0xF900 && code <= 0xFAFF) return 1;
    if (code >= 0xFF10 && code <= 0xFF19) return 1;
    if (code >= 0xFF21 && code <= 0xFF3A) return 1;
    if (code >= 0xFF41 && code <= 0xFF5A) return 1;
    if (code >= 0x1F100 && code <= 0x1F1FF) return 1;
    return 0;
}

static int mi_cp_at(const char *src, size_t len, size_t pos) {
    size_t step = 0;
    return mi_utf8_next(src + pos, len - pos, &step);
}

static int mi_byte_is_word(unsigned char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static char mi_lower_ascii(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static size_t mi_cp_bytes(const MiLexer *lx, size_t pos) {
    unsigned char first = (unsigned char)lx->src[pos];
    if (first < 0x80) return 1;
    if ((first & 0xE0) == 0xC0) return 2;
    if ((first & 0xF0) == 0xE0) return 3;
    if ((first & 0xF8) == 0xF0) return 4;
    return 1;
}

static int mi_word_before(const MiLexer *lx) {
    if (lx->pos == 0) return 0;
    size_t index = lx->pos - 1;
    while (index > 0 && ((unsigned char)lx->src[index] & 0xC0) == 0x80) index--;
    return mi_cp_is_word((unsigned)mi_cp_at(lx->src, lx->len, index));
}

static int mi_word_after(const MiLexer *lx, size_t end) {
    if (end >= lx->len) return 0;
    return mi_cp_is_word((unsigned)mi_cp_at(lx->src, lx->len, end));
}

static MiPos mi_here(const MiLexer *lx) {
    MiPos pos;
    pos.line = lx->line;
    pos.col = lx->col;
    pos.offset = (int)lx->cp;
    return pos;
}

static MiMark mi_mark(const MiLexer *lx) {
    MiMark mark;
    mark.pos = mi_here(lx);
    mark.byte = lx->pos;
    return mark;
}

static MiSpan mi_span_from(const MiLexer *lx, MiMark start) {
    MiSpan span;
    span.start = start.pos;
    span.end = mi_here(lx);
    return span;
}

static void mi_advance(MiLexer *lx) {
    size_t step = mi_cp_bytes(lx, lx->pos);
    unsigned char ch = (unsigned char)lx->src[lx->pos];
    lx->pos += step;
    lx->cp += 1;
    if (ch == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
}

/* 精确推进 n 个**字节**（拼写表给的是字节长度） */
static void mi_advance_bytes(MiLexer *lx, size_t length) {
    size_t target = lx->pos + length;
    while (lx->pos < target && lx->pos < lx->len) mi_advance(lx);
}

static MiTok *mi_push_token(MiTokList *tokens) {
    if (tokens->len == tokens->cap) {
        tokens->cap = tokens->cap ? tokens->cap * 2 : 256;
        tokens->items = mi_realloc(tokens->items, tokens->cap * sizeof(MiTok));
    }
    MiTok *tok = &tokens->items[tokens->len++];
    memset(tok, 0, sizeof(*tok));
    tok->num = MI_NOTHING_VALUE;
    return tok;
}

static void mi_emit(MiLexer *lx, MiTokenKind kind, int id, const char *sval,
                    MiValue num, MiMark start) {
    /* 连续分隔符合并成一个 */
    if (kind == TOK_SEP && lx->tokens->len > 0 &&
        lx->tokens->items[lx->tokens->len - 1].kind == TOK_SEP) {
        return;
    }
    MiTok *tok = mi_push_token(lx->tokens);
    tok->kind = kind;
    tok->id = id;
    tok->text = mi_strndup(lx->src + start.byte, lx->pos - start.byte);
    tok->sval = sval ? mi_strdup(sval) : NULL;
    tok->num = num;
    tok->span = mi_span_from(lx, start);
}

/* --------------------------------------------------------------- 注释 */
static void mi_skip_line_comment(MiLexer *lx) {
    while (lx->pos < lx->len && lx->src[lx->pos] != '\n') mi_advance(lx);
}

static void mi_block_comment(MiLexer *lx, const char *closer) {
    MiMark start = mi_mark(lx);
    mi_advance(lx);
    mi_advance(lx);
    while (lx->pos < lx->len) {
        if (lx->src[lx->pos] == closer[0] && lx->pos + 1 < lx->len &&
            lx->src[lx->pos + 1] == closer[1]) {
            mi_advance(lx);
            mi_advance(lx);
            return;
        }
        mi_advance(lx);
    }
    mi_diag_add(lx->bag, "E0002", mi_span_from(lx, start), NULL, 0,
                "unterminated block comment, expected '%s'", closer);
}

/* --------------------------------------------------------------- 文本 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MiTextBuf;

static void text_push(MiTextBuf *buf, const char *bytes, size_t len) {
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 32;
        while (cap < buf->len + len + 1) cap *= 2;
        buf->data = mi_realloc(buf->data, cap);
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, bytes, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void text_push_char(MiTextBuf *buf, char ch) { text_push(buf, &ch, 1); }

static void text_push_code(MiTextBuf *buf, unsigned code) {
    char bytes[4];
    size_t count = mi_utf8_encode(code, bytes);
    text_push(buf, bytes, count);
}

static void mi_lex_text(MiLexer *lx) {
    MiMark start = mi_mark(lx);
    const char *closer = "\"";
    size_t closer_len = 1;
    unsigned char first = (unsigned char)lx->src[lx->pos];
    if (first == 0xC2 && lx->pos + 1 < lx->len &&
        (unsigned char)lx->src[lx->pos + 1] == 0xAB) {
        closer = "\xC2\xBB"; /* » */
        closer_len = 2;
        mi_advance_bytes(lx, 2);
    } else {
        if (first == '\'') closer = "'";
        mi_advance(lx);
    }

    MiTextBuf buf = {NULL, 0, 0};
    text_push(&buf, "", 0);

    for (;;) {
        if (lx->pos >= lx->len) {
            mi_diag_add(lx->bag, "E0002", mi_span_from(lx, start), NULL, 0,
                        "unterminated text literal");
            break;
        }
        size_t char_start = lx->pos;
        unsigned char ch = (unsigned char)lx->src[lx->pos];
        if (lx->pos + closer_len <= lx->len &&
            memcmp(lx->src + lx->pos, closer, closer_len) == 0) {
            mi_advance_bytes(lx, closer_len);
            break;
        }
        mi_advance(lx);
        if (ch == '\\') {
            if (lx->pos >= lx->len) {
                mi_diag_add(lx->bag, "E0003", mi_span_from(lx, start), NULL, 0,
                            "text literal ends with a dangling escape");
                break;
            }
            unsigned char esc = (unsigned char)lx->src[lx->pos];
            mi_advance(lx);
            switch (esc) {
                case 'n': text_push_char(&buf, '\n'); break;
                case 't': text_push_char(&buf, '\t'); break;
                case 'r': text_push_char(&buf, '\r'); break;
                case '0': text_push_char(&buf, '\0'); break;
                case '\\': text_push_char(&buf, '\\'); break;
                case '"': text_push_char(&buf, '"'); break;
                case '\'': text_push_char(&buf, '\''); break;
                case 0xC2: {
                    if (lx->pos < lx->len && (unsigned char)lx->src[lx->pos] == 0xAB) {
                        mi_advance(lx);
                        text_push(&buf, "\xC2\xAB", 2);
                    } else if (lx->pos < lx->len && (unsigned char)lx->src[lx->pos] == 0xBB) {
                        mi_advance(lx);
                        text_push(&buf, "\xC2\xBB", 2);
                    } else {
                        text_push(&buf, "\xC2", 1);
                    }
                    break;
                }
                                case '\n': break; /* 续行 */
                case 'u': {
                    if (lx->pos >= lx->len || lx->src[lx->pos] != '{') {
                        mi_diag_add(lx->bag, "E0003", mi_span_from(lx, start),
                                    NULL, 0, "expected `{` after `\\u`");
                        break;
                    }
                    mi_advance(lx);
                    char digits[32];
                    size_t count = 0;
                    while (lx->pos < lx->len && lx->src[lx->pos] != '}') {
                        if (count + 1 < sizeof(digits)) digits[count++] = lx->src[lx->pos];
                        mi_advance(lx);
                    }
                    digits[count] = '\0';
                    if (lx->pos < lx->len && lx->src[lx->pos] == '}') mi_advance(lx);
                    char *endp = NULL;
                    long code = strtol(digits, &endp, 16);
                    if (count == 0 || !endp || *endp != '\0' || code < 0) {
                        mi_diag_add(lx->bag, "E0003", mi_span_from(lx, start),
                                    NULL, 0, "bad unicode escape `\\u{%s}`", digits);
                    } else {
                        text_push_code(&buf, (unsigned)code);
                    }
                    break;
                }
                default:
                    mi_diag_add(lx->bag, "E0003", mi_span_from(lx, start),
                                "use \\\\ for a backslash", 0,
                                "unknown escape `\\%c`", esc);
                    text_push_char(&buf, (char)esc);
                    break;
            }
            continue;
        }
        /* 多字节字符要整段复制，不能只拷首字节 */
        text_push(&buf, lx->src + char_start, lx->pos - char_start);
    }

    MiStr *str = mi_str_alloc(buf.len);
    if (buf.len) memcpy(str->data, buf.data, buf.len);
    free(buf.data);
    mi_emit(lx, TOK_TEXT, 0, str->data, mi_text_take(str), start);
}

/* --------------------------------------------------------------- 数字 */
static int mi_is_digit(const MiLexer *lx, size_t index) {
    if (index >= lx->len) return 0;
    char ch = lx->src[index];
    return ch >= '0' && ch <= '9';
}

static void mi_lex_number(MiLexer *lx) {
    MiMark start = mi_mark(lx);
    char radix = 0;

    if (lx->pos + 1 < lx->len && lx->src[lx->pos] == '0') {
        char next = lx->src[lx->pos + 1];
        if (next == 'x' || next == 'X') radix = 16;
        else if (next == 'b' || next == 'B') radix = 2;
    }
    if (radix) {
        mi_advance(lx);
        mi_advance(lx);
        char digits[256];
        size_t count = 0;
        while (lx->pos < lx->len) {
            unsigned char ch = (unsigned char)lx->src[lx->pos];
            if (!mi_byte_is_word(ch)) break;
            if (ch != '_' && count + 1 < sizeof(digits)) digits[count++] = (char)ch;
            mi_advance(lx);
        }
        digits[count] = '\0';
        char *endp = NULL;
        long long value = strtoll(digits, &endp, radix);
        if (count == 0 || !endp || *endp != '\0') {
            mi_diag_add(lx->bag, "E0004", mi_span_from(lx, start), NULL, 0,
                        radix == 16 ? "bad hexadecimal number `0x%s`"
                                    : "bad binary number `0b%s`",
                        digits);
            value = 0;
        }
        mi_emit(lx, TOK_NUMBER, 0, NULL, mi_int(value), start);
        return;
    }

    char text[512];
    size_t count = 0;
    int seen_dot = 0;
#define MI_PUSH(ch)                                              \
    do {                                                         \
        if (count + 1 < sizeof(text)) text[count++] = (char)(ch); \
    } while (0)

    for (;;) {
        if (lx->pos >= lx->len) break;
        char ch = lx->src[lx->pos];
        if (ch >= '0' && ch <= '9') {
            MI_PUSH(ch);
            mi_advance(lx);
        } else if (ch == '_' && count > 0 && mi_is_digit(lx, lx->pos + 1)) {
            mi_advance(lx);
        } else if (ch == '.' && !seen_dot && mi_is_digit(lx, lx->pos + 1)) {
            seen_dot = 1;
            MI_PUSH(ch);
            mi_advance(lx);
        } else {
            break;
        }
    }
    if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E')) {
        size_t save_pos = lx->pos;
        size_t save_cp = lx->cp;
        int save_line = lx->line;
        int save_col = lx->col;
        char exp_head[4];
        size_t head_count = 0;
        exp_head[head_count++] = lx->src[lx->pos];
        mi_advance(lx);
        if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-')) {
            if (head_count + 1 < sizeof(exp_head)) exp_head[head_count++] = lx->src[lx->pos];
            mi_advance(lx);
        }
        char digits[64];
        size_t digit_count = 0;
        while (lx->pos < lx->len &&
               ((lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') ||
                lx->src[lx->pos] == '_')) {
            if (lx->src[lx->pos] != '_' && digit_count + 1 < sizeof(digits)) {
                digits[digit_count++] = lx->src[lx->pos];
            }
            mi_advance(lx);
        }
        if (digit_count > 0) {
            for (size_t i = 0; i < head_count; i++) MI_PUSH(exp_head[i]);
            for (size_t i = 0; i < digit_count; i++) MI_PUSH(digits[i]);
        } else {
            lx->pos = save_pos;
            lx->cp = save_cp;
            lx->line = save_line;
            lx->col = save_col;
            mi_diag_add(lx->bag, "E0004", mi_span_from(lx, start), NULL, 0,
                        "bad number: exponent has no digits");
        }
    }
#undef MI_PUSH
    text[count] = '\0';

    int is_real = 0;
    for (size_t i = 0; i < count; i++) {
        if (text[i] == '.' || text[i] == 'e' || text[i] == 'E') {
            is_real = 1;
            break;
        }
    }
    if (is_real) {
        char *endp = NULL;
        double value = strtod(text, &endp);
        if (!endp || *endp != '\0') {
            mi_diag_add(lx->bag, "E0004", mi_span_from(lx, start), NULL, 0,
                        "bad number `%s`", text);
            value = 0.0;
        }
        mi_emit(lx, TOK_NUMBER, 0, NULL, mi_real(value), start);
    } else {
        char *endp = NULL;
        errno = 0;
        long long value = strtoll(text, &endp, 10);
        if (!endp || *endp != '\0') {
            mi_diag_add(lx->bag, "E0004", mi_span_from(lx, start), NULL, 0,
                        "bad number `%s`", text);
            value = 0;
        } else if (errno == ERANGE) {
            /* 超出 int64：交给大整数，别静默截断 */
            MiBig *big = mi_big_from_str(text);
            if (big) {
                mi_emit(lx, TOK_NUMBER, 0, NULL, mi_value_from_big(big), start);
                return;
            }
        }
        mi_emit(lx, TOK_NUMBER, 0, NULL, mi_int(value), start);
    }
}

/* --------------------------------------------------------- 标识符 */
static void mi_lex_identifier(MiLexer *lx) {
    MiMark start = mi_mark(lx);
    while (lx->pos < lx->len &&
           mi_cp_is_word((unsigned)mi_cp_at(lx->src, lx->len, lx->pos))) {
        mi_advance(lx);
    }
    char *text = mi_strndup(lx->src + start.byte, lx->pos - start.byte);
    mi_emit(lx, TOK_IDENT, 0, text, MI_NOTHING_VALUE, start);
    free(text);
}

/* --------------------------------------------------------- 拼写匹配 */
static int mi_match_entry(MiLexer *lx, const MiSpelling *entry) {
    size_t length = strlen(entry->spelling);
    if (lx->pos + length > lx->len) return 0;
    const char *window = lx->src + lx->pos;
    if (entry->word) {
        for (size_t i = 0; i < length; i++) {
            if (mi_lower_ascii(window[i]) != entry->spelling[i]) return 0;
        }
        if (mi_word_after(lx, lx->pos + length)) return 0;
        if (mi_word_before(lx)) return 0;
    } else if (memcmp(window, entry->spelling, length) != 0) {
        return 0;
    }
    return 1;
}

static int mi_match_spelling(MiLexer *lx) {
    for (size_t i = 0; i < MI_SPELLING_COUNT; i++) {
        const MiSpelling *entry = &MI_SPELLINGS[i];
        if (!mi_match_entry(lx, entry)) continue;

        MiMark start = mi_mark(lx);
        mi_advance_bytes(lx, strlen(entry->spelling));

        switch (entry->kind) {
            case 'K':
                mi_emit(lx, TOK_KW, entry->id, NULL, MI_NOTHING_VALUE, start);
                break;
            case 'O':
                mi_emit(lx, TOK_OP, entry->id, NULL, MI_NOTHING_VALUE, start);
                break;
            case 'L': {
                int mark = MI_LITERAL_KIND[entry->id];
                if (mark == 2) {
                    mi_emit(lx, TOK_NIL, entry->id, NULL, MI_NOTHING_VALUE, start);
                } else {
                    mi_emit(lx, TOK_FLAG, entry->id, NULL, mi_flag(mark == 0), start);
                }
                break;
            }
            case 'G':
                mi_emit(lx, TOK_GLYPH, entry->id, MI_BUILTIN_NAMES[entry->id],
                        MI_NOTHING_VALUE, start);
                break;
            default:
                break;
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- 主循环 */
int mi_lex(const char *source, const char *filename, MiTokList *out, MiDiagBag *bag) {
    MiLexer lx;
    memset(&lx, 0, sizeof(lx));
    lx.src = source ? source : "";
    lx.len = strlen(lx.src);
    lx.pos = 0;
    lx.cp = 0;
    lx.line = 1;
    lx.col = 1;
    lx.filename = filename;
    lx.bag = bag;
    lx.tokens = out;

    while (lx.pos < lx.len) {
        unsigned char ch = (unsigned char)lx.src[lx.pos];

        if (ch == ' ' || ch == '\t' || ch == '\r') {
            mi_advance(&lx);
            continue;
        }
        if (ch == '\n') {
            MiMark start = mi_mark(&lx);
            mi_advance(&lx);
            mi_emit(&lx, TOK_SEP, 0, "\n", MI_NOTHING_VALUE, start);
            continue;
        }
        if (ch == ';' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == ';') {
            mi_skip_line_comment(&lx);
            continue;
        }
        if (ch == ';') {
            MiMark start = mi_mark(&lx);
            mi_advance(&lx);
            mi_emit(&lx, TOK_SEP, 0, ";", MI_NOTHING_VALUE, start);
            continue;
        }
        if (ch == '#') {
            mi_skip_line_comment(&lx);
            continue;
        }
        if (ch == '-' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == '-') {
            mi_skip_line_comment(&lx);
            continue;
        }
        if (ch == '/' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == '*') {
            mi_block_comment(&lx, "*/");
            continue;
        }
        if (ch == '(' && lx.pos + 1 < lx.len && lx.src[lx.pos + 1] == '*') {
            mi_block_comment(&lx, "*)");
            continue;
        }
        if (ch == '"' || ch == '\'' ||
            (ch == 0xC2 && lx.pos + 1 < lx.len && (unsigned char)lx.src[lx.pos + 1] == 0xAB)) {
            mi_lex_text(&lx);
            continue;
        }
        if ((ch >= '0' && ch <= '9') || (ch == '.' && mi_is_digit(&lx, lx.pos + 1))) {
            mi_lex_number(&lx);
            continue;
        }
        if (mi_match_spelling(&lx)) continue;
        if (ch < 0x80 && strchr(MI_PUNCTUATION, (char)ch)) {
            MiMark start = mi_mark(&lx);
            mi_advance(&lx);
            char text[2] = {(char)ch, '\0'};
            mi_emit(&lx, TOK_PUNCT, 0, text, MI_NOTHING_VALUE, start);
            continue;
        }
        if (mi_cp_is_word((unsigned)mi_cp_at(lx.src, lx.len, lx.pos))) {
            mi_lex_identifier(&lx);
            continue;
        }

        MiMark start = mi_mark(&lx);
        mi_advance(&lx);
        char shown[8] = {(char)ch, '\0'};
        const char *hint = mi_suggest(shown, MI_SUGGEST_WORDS, MI_SUGGEST_WORD_COUNT, 0.6);
        char hint_text[128];
        if (hint) snprintf(hint_text, sizeof(hint_text), "maybe you meant `%s`?", hint);
        mi_diag_add(bag, "E0001", mi_span_from(&lx, start),
                    hint ? hint_text : NULL, 0, "unexpected character '%c'", ch);
    }

    if (out->len == 0 || out->items[out->len - 1].kind != TOK_SEP) {
        MiPos here = mi_here(&lx);
        MiTok *tok = mi_push_token(out);
        tok->kind = TOK_SEP;
        tok->text = mi_strdup("");
        tok->sval = mi_strdup("\n");
        tok->span.start = here;
        tok->span.end = here;
    }
    {
        MiPos here = mi_here(&lx);
        MiTok *tok = mi_push_token(out);
        tok->kind = TOK_EOF;
        tok->text = mi_strdup("");
        tok->span.start = here;
        tok->span.end = here;
    }
    return 0;
}

void mi_toklist_free(MiTokList *tokens) {
    if (!tokens) return;
    for (size_t i = 0; i < tokens->len; i++) {
        free(tokens->items[i].text);
        free(tokens->items[i].sval);
        mi_release(tokens->items[i].num);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->len = tokens->cap = 0;
}
