/* parser.c — MI 语法分析器（递归下降 + 优先级爬升 + 语句边界恢复）
 *
 * 与 Python 版逐条对齐：接受 SPEC.md 里所有书写形式（等价拼写、可选标点、
 * 可省括号、隐式调用、中缀 emit、`{}` 与 `do…end` 块、`otherwise when` 链）。
 */
#include "mi.h"
#include "tables.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MiTokList stripped;   /* 去掉了括号内部分隔符的 token 流 */
    size_t pos;
    const char *source;
    const char *filename;
    MiDiagBag *bag;
    jmp_buf recover;
} MiParser;

static MiAst *p_parse_item(MiParser *p);
static MiAst *p_parse_flow(MiParser *p);
static MiAst *p_parse_block(MiParser *p);
static MiAst *p_parse_expr(MiParser *p, int min_prec);
static MiAst *p_parse_when(MiParser *p);

/* ------------------------------------------------------------- 基础 */
static MiTok *p_peek(MiParser *p, size_t ahead) {
    size_t index = p->pos + ahead;
    if (index >= p->stripped.len) index = p->stripped.len - 1;
    return &p->stripped.items[index];
}

static MiTok *p_advance(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind != TOK_EOF) p->pos++;
    return tok;
}

static int p_at_eof(MiParser *p) { return p_peek(p, 0)->kind == TOK_EOF; }
static int p_at_sep(MiParser *p) { return p_peek(p, 0)->kind == TOK_SEP; }

static void p_skip_seps(MiParser *p) {
    while (p_peek(p, 0)->kind == TOK_SEP) p_advance(p);
}

static int p_at_punct(MiParser *p, const char *value) {
    MiTok *tok = p_peek(p, 0);
    return tok->kind == TOK_PUNCT && tok->sval && strcmp(tok->sval, value) == 0;
}

static int p_at_kw(MiParser *p, int keyword) {
    MiTok *tok = p_peek(p, 0);
    return tok->kind == TOK_KW && tok->id == keyword;
}

static int p_at_op(MiParser *p, int op) {
    MiTok *tok = p_peek(p, 0);
    return tok->kind == TOK_OP && tok->id == op;
}

static MiSpan p_span(MiPos start, MiPos end) {
    MiSpan span;
    span.start = start;
    span.end = end;
    return span;
}

static void p_unexpected(MiParser *p, const char *expected, MiTok *tok) {
    if (!tok) tok = p_peek(p, 0);
    const char *shown = (tok->text && *tok->text) ? tok->text : "end of input";
    char hint[256];
    hint[0] = '\0';
    if (tok->text && *tok->text) {
        const char *suggestion = mi_suggest(tok->text, MI_SUGGEST_WORDS,
                                            MI_SUGGEST_WORD_COUNT, 0.6);
        if (suggestion) {
            snprintf(hint, sizeof(hint), "maybe you meant `%s`?", suggestion);
        }
    }
    mi_diag_add(p->bag, "E0100", tok->span, hint[0] ? hint : NULL, 0,
                "unexpected '%s'; expected %s", shown, expected);
}

static void p_fail(MiParser *p) { longjmp(p->recover, 1); }

static MiTok *p_expect_punct(MiParser *p, const char *value) {
    if (p_at_punct(p, value)) return p_advance(p);
    char expected[32];
    snprintf(expected, sizeof(expected), "`%s`", value);
    p_unexpected(p, expected, NULL);
    p_fail(p);
    return NULL;
}

static MiTok *p_expect_kw(MiParser *p, int keyword) {
    if (p_at_kw(p, keyword)) return p_advance(p);
    char expected[64];
    snprintf(expected, sizeof(expected), "`%s`", MI_KEYWORD_NAMES[keyword]);
    p_unexpected(p, expected, NULL);
    p_fail(p);
    return NULL;
}

/* 关键字被当成名字用：报 E0100 但继续 */
static MiTok *p_expect_ident(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_IDENT) return p_advance(p);
    if (tok->kind == TOK_KW) {
        mi_diag_add(p->bag, "E0100", tok->span, "pick a different name", 0,
                    "`%s` is a keyword and cannot be used as a name", tok->text);
        MiTok *advance = p_advance(p);
        static MiTok converted;
        memset(&converted, 0, sizeof(converted));
        converted.kind = TOK_IDENT;
        converted.text = advance->text;
        converted.sval = advance->text;
        converted.span = advance->span;
        converted.num = MI_NOTHING_VALUE;
        return &converted;
    }
    p_unexpected(p, "a name", NULL);
    p_fail(p);
    return NULL;
}

static void p_recover(MiParser *p) {
    while (!p_at_eof(p)) {
        if (p_at_sep(p)) {
            p_advance(p);
            return;
        }
        if (p_at_punct(p, "}") || p_at_kw(p, KW_END)) return;
        p_advance(p);
    }
}

/* token 的原文能否当字段名/键名（Python: 全为字母数字或下划线） */
static int p_text_is_name(MiTok *tok) {
    if (!tok->text || !*tok->text) return 0;
    for (const char *cursor = tok->text; *cursor; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        int ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= 'A' && ch <= 'Z') || ch == '_' || ch >= 0x80;
        if (!ok) return 0;
    }
    return 1;
}

static MiTok *p_field_name(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_IDENT) return p_advance(p);
    if ((tok->kind == TOK_KW || tok->kind == TOK_OP || tok->kind == TOK_FLAG ||
         tok->kind == TOK_NIL) && p_text_is_name(tok)) {
        MiTok *advance = p_advance(p);
        static MiTok converted;
        memset(&converted, 0, sizeof(converted));
        converted.kind = TOK_IDENT;
        converted.text = advance->text;
        converted.sval = advance->text;
        converted.span = advance->span;
        converted.num = MI_NOTHING_VALUE;
        return &converted;
    }
    mi_diag_add(p->bag, "E0100", tok->span, NULL, 0, "expected a field name after `.`");
    p_fail(p);
    return NULL;
}

static MiTok *p_key_name(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_IDENT) return p_advance(p);
    if ((tok->kind == TOK_KW || tok->kind == TOK_OP || tok->kind == TOK_FLAG ||
         tok->kind == TOK_NIL) && p_text_is_name(tok)) {
        MiTok *advance = p_advance(p);
        static MiTok converted;
        memset(&converted, 0, sizeof(converted));
        converted.kind = TOK_IDENT;
        converted.text = advance->text;
        converted.sval = advance->text;
        converted.span = advance->span;
        converted.num = MI_NOTHING_VALUE;
        return &converted;
    }
    return p_expect_ident(p);
}

/* --------------------------------------------------------- 去括号内分隔符 */
static void p_strip_inner_seps(MiTokList *source, MiTokList *out) {
    int depth = 0;
    out->items = mi_calloc(source->len, sizeof(MiTok));
    out->len = 0;
    out->cap = source->len;
    for (size_t i = 0; i < source->len; i++) {
        MiTok *tok = &source->items[i];
        if (tok->kind == TOK_PUNCT && tok->sval) {
            if (strcmp(tok->sval, "(") == 0 || strcmp(tok->sval, "[") == 0 ||
                strcmp(tok->sval, "\xe2\x9f\xa8") == 0) {
                depth++;
            } else if (strcmp(tok->sval, ")") == 0 || strcmp(tok->sval, "]") == 0 ||
                       strcmp(tok->sval, "\xe2\x9f\xa9") == 0) {
                if (depth > 0) depth--;
            }
        }
        if (tok->kind == TOK_SEP && depth > 0) continue;
        out->items[out->len++] = *tok;
    }
}

/* ------------------------------------------------------------- 表达式 */
static int p_starts_implicit_arg(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    switch (tok->kind) {
        case TOK_IDENT:
        case TOK_NUMBER:
        case TOK_TEXT:
        case TOK_FLAG:
        case TOK_NIL:
        case TOK_GLYPH:
            return 1;
        default:
            break;
    }
    if (tok->kind == TOK_PUNCT && tok->sval &&
        (strcmp(tok->sval, "(") == 0 || strcmp(tok->sval, "[") == 0 ||
         strcmp(tok->sval, "\xe2\x9f\xa8") == 0)) {
        return 1;
    }
    if (tok->kind == TOK_KW && (tok->id == KW_ASSEMBLY || tok->id == KW_DRAW)) return 1;
    return 0;
}

static MiAst *p_parse_arg(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    MiTok *next = p_peek(p, 1);
    if (tok->kind == TOK_IDENT && next->kind == TOK_PUNCT && next->sval &&
        (strcmp(next->sval, ":") == 0 || strcmp(next->sval, "=") == 0)) {
        MiTok *label = p_advance(p);
        p_advance(p);
        MiAst *value = p_parse_expr(p, 0);
        MiAst *arg = mi_ast_new(AST_ARG, p_span(label->span.start, value->span.end));
        arg->name = mi_strdup(label->sval);
        arg->a = value;
        return arg;
    }
    MiAst *value = p_parse_expr(p, 0);
    MiAst *arg = mi_ast_new(AST_ARG, value->span);
    arg->a = value;
    return arg;
}

static void p_append(MiAst ***list, size_t *count, MiAst *node) {
    *list = mi_realloc(*list, (*count + 1) * sizeof(MiAst *));
    (*list)[(*count)++] = node;
}

static void p_parse_paren_args(MiParser *p, MiAst ***args, size_t *count) {
    *args = NULL;
    *count = 0;
    p_expect_punct(p, "(");
    if (p_at_punct(p, ")")) {
        p_advance(p);
        return;
    }
    for (;;) {
        if (p_at_eof(p)) {
            mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0,
                        "missing `)` to close this argument list");
            break;
        }
        if (p_at_punct(p, ")")) {
            p_advance(p);
            break;
        }
        p_append(args, count, p_parse_arg(p));
        if (p_at_punct(p, ",")) {
            p_advance(p);
            continue;
        }
        if (p_at_punct(p, ")")) {
            p_advance(p);
            break;
        }
        if (p_starts_implicit_arg(p)) continue;
        mi_diag_add(p->bag, "E0103", p_peek(p, 0)->span, NULL, 0,
                    "expected `,` or `)` in the argument list");
        p_fail(p);
    }
}

static void p_parse_implicit_args(MiParser *p, MiAst ***args, size_t *count) {
    *args = NULL;
    *count = 0;
    p_append(args, count, p_parse_arg(p));
    while (p_at_punct(p, ",")) {
        p_advance(p);
        p_append(args, count, p_parse_arg(p));
    }
}

static MiAst *p_parse_list_literal(MiParser *p, const char *closer) {
    MiTok *opener = p_advance(p);
    MiAst **items = NULL;
    size_t count = 0;
    for (;;) {
        p_skip_seps(p);
        if (p_at_eof(p)) {
            mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0,
                        "missing `%s` to close this list", closer);
            break;
        }
        if (p_at_punct(p, closer)) {
            MiTok *end = p_advance(p);
            MiAst *node = mi_ast_new(AST_LIST, p_span(opener->span.start, end->span.end));
            node->list = items;
            node->count = count;
            return node;
        }
        p_append(&items, &count, p_parse_expr(p, 0));
        if (p_at_punct(p, ",")) {
            p_advance(p);
            continue;
        }
        p_skip_seps(p);
        if (p_at_punct(p, closer)) continue;
        if (p_starts_implicit_arg(p)) continue;
        mi_diag_add(p->bag, "E0103", p_peek(p, 0)->span, NULL, 0,
                    "expected `,` or `%s` in this list", closer);
        p_fail(p);
    }
    if (count == 0) p_fail(p);
    MiAst *node = mi_ast_new(AST_LIST, p_span(opener->span.start, p_peek(p, 0)->span.end));
    node->list = items;
    node->count = count;
    return node;
}

static MiAst *p_parse_assembly_literal(MiParser *p) {
    MiTok *tok = p_advance(p);
    if (!p_at_punct(p, "{")) {
        mi_diag_add(p->bag, "E0100", p_peek(p, 0)->span, NULL, 0,
                    "expected `{` after `assembly`");
        p_fail(p);
    }
    p_advance(p);
    MiAst **fields = NULL;
    size_t count = 0;
    for (;;) {
        p_skip_seps(p);
        if (p_at_eof(p)) {
            mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0,
                        "missing `}` to close this assembly");
            break;
        }
        if (p_at_punct(p, "}")) {
            MiTok *end = p_advance(p);
            MiAst *node =
                mi_ast_new(AST_ASSEMBLY, p_span(tok->span.start, end->span.end));
            node->list = fields;
            node->count = count;
            return node;
        }
        MiTok *name = p_key_name(p);
        if (p_at_punct(p, ":") || p_at_punct(p, "=")) p_advance(p);
        MiAst *value = p_parse_expr(p, 0);
        MiAst *field = mi_ast_new(AST_FIELDINIT, p_span(name->span.start, value->span.end));
        field->name = mi_strdup(name->sval);
        field->a = value;
        p_append(&fields, &count, field);
        if (p_at_punct(p, ",")) p_advance(p);
    }
    MiAst *node = mi_ast_new(AST_ASSEMBLY, p_span(tok->span.start, p_peek(p, 0)->span.end));
    node->list = fields;
    node->count = count;
    return node;
}

static MiAst *p_parse_primary(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    switch (tok->kind) {
        case TOK_NUMBER:
            p_advance(p);
            {
                MiAst *node = mi_ast_new(AST_NUMBER, tok->span);
                node->literal = mi_retain(tok->num);
                return node;
            }
        case TOK_TEXT:
            p_advance(p);
            {
                MiAst *node = mi_ast_new(AST_TEXT, tok->span);
                node->literal = mi_text_new(tok->sval);
                return node;
            }
        case TOK_FLAG:
            p_advance(p);
            {
                MiAst *node = mi_ast_new(AST_FLAG, tok->span);
                node->literal = tok->num;
                return node;
            }
        case TOK_NIL:
            p_advance(p);
            return mi_ast_new(AST_NIL, tok->span);
        case TOK_IDENT:
            p_advance(p);
            {
                MiAst *node = mi_ast_new(AST_NAME, tok->span);
                node->name = mi_strdup(tok->sval);
                return node;
            }
        case TOK_GLYPH:
            p_advance(p);
            {
                MiAst *node = mi_ast_new(AST_NAME, tok->span);
                node->name = mi_strdup(tok->sval);
                node->flag = 1; /* glyph */
                return node;
            }
        default:
            break;
    }
    if (tok->kind == TOK_PUNCT && tok->sval) {
        if (strcmp(tok->sval, "(") == 0) {
            p_advance(p);
            MiAst *expr = p_parse_expr(p, 0);
            if (p_at_punct(p, ")")) {
                p_advance(p);
            } else {
                mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0, "missing `)`");
            }
            return expr;
        }
        if (strcmp(tok->sval, "[") == 0) return p_parse_list_literal(p, "]");
        if (strcmp(tok->sval, "\xe2\x9f\xa8") == 0) return p_parse_list_literal(p, "\xe2\x9f\xa9");
    }
    if (tok->kind == TOK_KW && tok->id == KW_ASSEMBLY) return p_parse_assembly_literal(p);
    if (tok->kind == TOK_KW && tok->id == KW_DRAW) {
        p_advance(p);
        MiTok *name = p_expect_ident(p);
        MiAst *node = mi_ast_new(AST_READ, p_span(tok->span.start, name->span.end));
        node->name = mi_strdup(name->sval);
        return node;
    }
    p_unexpected(p, "an expression", NULL);
    p_fail(p);
    return NULL;
}

/* 节点能否直接当被调用者：返回真名（并区分 glyph） */
static int p_callable_name(MiAst *node, const char **name, int *glyph) {
    if (node->kind == AST_NAME) {
        *name = node->name;
        *glyph = node->flag;
        return 1;
    }
    if (node->kind == AST_FIELD && node->a && node->a->kind == AST_NAME &&
        !node->a->flag) {
        static char buffer[512];
        snprintf(buffer, sizeof(buffer), "%s.%s", node->a->name, node->name);
        *name = buffer;
        *glyph = 0;
        return 1;
    }
    return 0;
}

static MiAst *p_make_call(MiAst *node, MiAst **args, size_t count, MiSpan span) {
    const char *name = NULL;
    int glyph = 0;
    MiAst *call = mi_ast_new(AST_CALL, span);
    call->list = args;
    call->count = count;
    if (p_callable_name(node, &name, &glyph)) {
        call->name = mi_strdup(name);
        call->flag = glyph;
    } else {
        call->a = node;
    }
    return call;
}

static MiAst *p_parse_postfix(MiParser *p) {
    MiAst *node = p_parse_primary(p);
    for (;;) {
        MiTok *tok = p_peek(p, 0);
        if (tok->kind == TOK_PUNCT && tok->sval && strcmp(tok->sval, ".") == 0) {
            p_advance(p);
            MiTok *member = p_field_name(p);
            MiTok *next = p_peek(p, 0);
            int called = next->kind == TOK_PUNCT && next->sval &&
                         strcmp(next->sval, "(") == 0;
            if (node->kind == AST_NAME && !node->flag && called) {
                char *joined = mi_asprintf("%s.%s", node->name, member->text);
                MiAst *merged =
                    mi_ast_new(AST_NAME, p_span(node->span.start, member->span.end));
                merged->name = joined;
                node = merged;
                continue;
            }
            MiAst *field = mi_ast_new(AST_FIELD, p_span(node->span.start, member->span.end));
            field->a = node;
            field->name = mi_strdup(member->text);
            node = field;
            continue;
        }
        if (tok->kind == TOK_PUNCT && tok->sval && strcmp(tok->sval, "(") == 0) {
            MiAst **args = NULL;
            size_t count = 0;
            p_parse_paren_args(p, &args, &count);
            MiTok *end = &p->stripped.items[p->pos - 1];
            node = p_make_call(node, args, count, p_span(node->span.start, end->span.end));
            continue;
        }
        const char *name = NULL;
        int glyph = 0;
        if (p_callable_name(node, &name, &glyph) && p_starts_implicit_arg(p)) {
            MiAst **args = NULL;
            size_t count = 0;
            p_parse_implicit_args(p, &args, &count);
            node = p_make_call(node, args, count,
                               p_span(node->span.start, args[count - 1]->span.end));
            continue;
        }
        break;
    }
    return node;
}

static MiAst *p_parse_unary(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_OP && (tok->id == OPK_NOT || tok->id == OPK_MINUS)) {
        p_advance(p);
        MiAst *operand = p_parse_unary(p);
        MiAst *node = mi_ast_new(AST_UNARY, p_span(tok->span.start, operand->span.end));
        node->name = mi_strdup(MI_OPERATOR_NAMES[tok->id]);
        node->a = operand;
        return node;
    }
    return p_parse_postfix(p);
}

static MiAst *p_parse_expr(MiParser *p, int min_prec) {
    MiAst *left = p_parse_unary(p);
    for (;;) {
        MiTok *tok = p_peek(p, 0);
        if (tok->kind != TOK_OP) break;
        const MiOperatorInfo *info = &MI_OPERATORS[tok->id];
        if (info->prefix) break; /* 前缀运算符不是中缀 */
        if (info->precedence < min_prec) break;
        p_advance(p);
        int next_min = info->right_assoc ? info->precedence : info->precedence + 1;
        MiAst *right = p_parse_expr(p, next_min);
        MiAst *node = mi_ast_new(AST_BINARY, p_span(left->span.start, right->span.end));
        node->name = mi_strdup(MI_OPERATOR_NAMES[tok->id]);
        node->a = left;
        node->b = right;
        left = node;
    }
    return left;
}

/* --------------------------------------------------------------- 流程 */
static void p_collect_out_ports(MiAst *block, char ***ports, size_t *count);

static void p_collect_flow_ports(MiAst *flow, char ***ports, size_t *count) {
    if (!flow) return;
    if (flow->kind == AST_OUT && flow->name) {
        for (size_t i = 0; i < *count; i++) {
            if (strcmp((*ports)[i], flow->name) == 0) return;
        }
        *ports = mi_realloc(*ports, (*count + 1) * sizeof(char *));
        (*ports)[(*count)++] = mi_strdup(flow->name);
        return;
    }
    if (flow->kind == AST_RETURN && flow->a) {
        for (size_t i = 0; i < *count; i++) {
            if (strcmp((*ports)[i], "value") == 0) return;
        }
        *ports = mi_realloc(*ports, (*count + 1) * sizeof(char *));
        (*ports)[(*count)++] = mi_strdup("value");
        return;
    }
    if (flow->kind == AST_WHEN) {
        p_collect_out_ports(flow->b, ports, count);
        if (flow->c) {
            if (flow->c->kind == AST_BLOCK) {
                p_collect_out_ports(flow->c, ports, count);
            } else {
                p_collect_flow_ports(flow->c, ports, count);
            }
        }
        return;
    }
    if (flow->kind == AST_CYCLE || flow->kind == AST_FOR) {
        p_collect_out_ports(flow->b, ports, count);
        return;
    }
}

static void p_collect_out_ports(MiAst *block, char ***ports, size_t *count) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        p_collect_flow_ports(block->list[i], ports, count);
    }
}

static MiAst *p_parse_emit(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_EMIT);
    MiTok *target = p_expect_ident(p);
    if (p_at_punct(p, ":") || p_at_punct(p, "=")) {
        p_advance(p);
    } else if (p_at_kw(p, KW_EMIT)) {
        p_advance(p);
    }
    if (p_at_sep(p) || p_at_eof(p) || p_at_punct(p, "}")) {
        mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0, "`emit` needs a value");
        p_fail(p);
    }
    MiAst *value = p_parse_expr(p, 0);
    MiAst *node = mi_ast_new(AST_EMIT, p_span(tok->span.start, value->span.end));
    node->name = mi_strdup(target->sval);
    node->a = value;
    return node;
}

static MiAst *p_parse_emit_infix(MiParser *p) {
    MiTok *target = p_advance(p);
    p_advance(p); /* emit 的拼写 */
    MiAst *value = p_parse_expr(p, 0);
    MiAst *node = mi_ast_new(AST_EMIT, p_span(target->span.start, value->span.end));
    node->name = mi_strdup(target->sval);
    node->a = value;
    return node;
}

static MiAst *p_parse_when(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_WHEN);
    MiAst *cond = p_parse_expr(p, 0);
    if (p_at_kw(p, KW_THEN)) p_advance(p);
    MiAst *body = p_parse_block(p);
    MiAst *otherwise = NULL;
    MiPos end = body->span.end;
    p_skip_seps(p);
    if (p_at_kw(p, KW_OTHERWISE)) {
        p_advance(p);
        p_skip_seps(p);
        if (p_at_kw(p, KW_WHEN)) {
            otherwise = p_parse_when(p);
        } else {
            otherwise = p_parse_block(p);
        }
        end = otherwise->span.end;
    }
    MiAst *node = mi_ast_new(AST_WHEN, p_span(tok->span.start, end));
    node->a = cond;
    node->b = body;
    node->c = otherwise;
    return node;
}

static MiAst *p_parse_cycle(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_CYCLE);
    const char *kind = "count";
    MiAst *head = NULL;
    if (p_at_kw(p, KW_WHILE)) {
        p_advance(p);
        kind = "while";
        head = p_parse_expr(p, 0);
    } else if (p_at_kw(p, KW_UNTIL)) {
        p_advance(p);
        kind = "until";
        head = p_parse_expr(p, 0);
    } else if (p_at_kw(p, KW_OVER)) {
        p_advance(p);
        kind = "over";
        head = p_parse_expr(p, 0);
    } else {
        head = p_parse_expr(p, 0);
    }
    char *var = NULL;
    if (p_at_kw(p, KW_AS)) {
        p_advance(p);
        var = mi_strdup(p_expect_ident(p)->sval);
    }
    MiAst *body = p_parse_block(p);
    MiAst *node = mi_ast_new(AST_CYCLE, p_span(tok->span.start, body->span.end));
    node->name2 = mi_strdup(kind);
    node->a = head;
    node->name = var;
    node->b = body;
    return node;
}

static MiAst *p_parse_for(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_FOR);
    MiTok *var = p_expect_ident(p);
    p_expect_kw(p, KW_IN);
    MiAst *items = p_parse_expr(p, 0);
    MiAst *body = p_parse_block(p);
    MiAst *node = mi_ast_new(AST_FOR, p_span(tok->span.start, body->span.end));
    node->name = mi_strdup(var->sval);
    node->a = items;
    node->b = body;
    return node;
}

static MiAst *p_parse_out(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_OUT);
    MiTok *name = p_expect_ident(p);
    if (p_at_punct(p, ":") || p_at_punct(p, "=")) p_advance(p);
    if (p_at_sep(p) || p_at_eof(p) || p_at_punct(p, "}")) {
        mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0,
                    "output port `%s` needs a value", name->sval);
        p_fail(p);
    }
    MiAst *value = p_parse_expr(p, 0);
    MiAst *node = mi_ast_new(AST_OUT, p_span(tok->span.start, value->span.end));
    node->name = mi_strdup(name->sval);
    node->a = value;
    return node;
}

static MiAst *p_parse_return(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_RETURN);
    if (p_at_sep(p) || p_at_eof(p) || p_at_punct(p, "}") || p_at_kw(p, KW_END)) {
        return mi_ast_new(AST_RETURN, tok->span);
    }
    MiAst *value = p_parse_expr(p, 0);
    MiAst *node = mi_ast_new(AST_RETURN, p_span(tok->span.start, value->span.end));
    node->a = value;
    return node;
}

/* --------------------------------------------------------------- 块 */
static int p_at_block_start(MiParser *p) {
    return p_at_punct(p, "{") || p_at_kw(p, KW_DO) || p_at_kw(p, KW_BEGIN);
}

static MiAst *p_parse_block(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_PUNCT && tok->sval && strcmp(tok->sval, "{") == 0) {
        p_advance(p);
        MiAst **flows = NULL;
        size_t count = 0;
        p_skip_seps(p);
        while (!p_at_eof(p) && !p_at_punct(p, "}")) {
            p_append(&flows, &count, p_parse_flow(p));
            p_skip_seps(p);
        }
        if (p_at_punct(p, "}")) {
            MiTok *closer = p_advance(p);
            MiAst *block = mi_ast_new(AST_BLOCK, p_span(tok->span.start, closer->span.end));
            block->list = flows;
            block->count = count;
            return block;
        }
        mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0,
                    "missing `}` to close this block");
        MiAst *block = mi_ast_new(AST_BLOCK, p_span(tok->span.start, p_peek(p, 0)->span.end));
        block->list = flows;
        block->count = count;
        return block;
    }
    if (tok->kind == TOK_KW && (tok->id == KW_DO || tok->id == KW_BEGIN)) {
        MiTok *opener = p_advance(p);
        MiAst **flows = NULL;
        size_t count = 0;
        p_skip_seps(p);
        while (!p_at_eof(p) && !p_at_kw(p, KW_END)) {
            p_append(&flows, &count, p_parse_flow(p));
            p_skip_seps(p);
        }
        if (p_at_kw(p, KW_END)) {
            MiTok *closer = p_advance(p);
            MiAst *block =
                mi_ast_new(AST_BLOCK, p_span(opener->span.start, closer->span.end));
            block->list = flows;
            block->count = count;
            return block;
        }
        mi_diag_add(p->bag, "E0102", p_peek(p, 0)->span, NULL, 0,
                    "missing `end` to close this block");
        MiAst *block =
            mi_ast_new(AST_BLOCK, p_span(opener->span.start, p_peek(p, 0)->span.end));
        block->list = flows;
        block->count = count;
        return block;
    }
    mi_diag_add(p->bag, "E0102", tok->span, "a block is required here", 0,
                "expected a block: `{ … }`, or `do … end` / `begin … end`");
    p_fail(p);
    return NULL;
}

/* --------------------------------------------------------------- 流程分派 */
static const char *p_keyword_typo_hint(const char *name) {
    const char *match = mi_suggest(name, MI_KEYWORD_WORDS, MI_KEYWORD_WORD_COUNT, 0.7);
    if (!match) return NULL;
    static char buffer[256];
    snprintf(buffer, sizeof(buffer), "maybe you meant `%s`?", match);
    return buffer;
}

static MiAst *p_parse_flow(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_KW) {
        switch (tok->id) {
            case KW_EMIT: return p_parse_emit(p);
            case KW_WHEN: return p_parse_when(p);
            case KW_CYCLE: return p_parse_cycle(p);
            case KW_FOR: return p_parse_for(p);
            case KW_OUT: return p_parse_out(p);
            case KW_RETURN: return p_parse_return(p);
            case KW_HALT:
                p_advance(p);
                return mi_ast_new(AST_HALT, tok->span);
            case KW_SKIP:
                p_advance(p);
                return mi_ast_new(AST_SKIP, tok->span);
            case KW_OTHERWISE:
                mi_diag_add(p->bag, "E0100", tok->span, "use `when … { } otherwise { }`",
                            0, "`otherwise` must directly follow a `when` block");
                p_fail(p);
                break;
            case KW_STATION:
            case KW_USE:
            case KW_WIRE:
                return p_parse_item(p);
            case KW_END:
                mi_diag_add(p->bag, "E0100", tok->span, NULL, 0, "unexpected `end`");
                p_fail(p);
                break;
            default:
                break;
        }
    }
    if (tok->kind == TOK_IDENT && p_peek(p, 1)->kind == TOK_KW &&
        p_peek(p, 1)->id == KW_EMIT) {
        return p_parse_emit_infix(p);
    }
    MiAst *expr = p_parse_expr(p, 0);
    MiAst *flow = mi_ast_new(AST_FLOW, p_span(tok->span.start, expr->span.end));
    flow->a = expr;
    /* `wenn cond { … }`：标识符出现在该是关键字的位置 */
    if (tok->kind == TOK_IDENT && p_at_block_start(p)) {
        const char *hint = p_keyword_typo_hint(tok->sval);
        if (hint) {
            mi_diag_add(p->bag, "E0101", tok->span, hint, 0,
                        "unknown keyword `%s`", tok->sval);
            if (expr->kind == AST_CALL) expr->flag2 = 1;
        }
    }
    return flow;
}

/* --------------------------------------------------------------- 条目 */
static MiAst *p_parse_wire(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_WIRE);
    MiTok *name = p_expect_ident(p);
    MiAst *init = NULL;
    if (p_at_punct(p, ":") || p_at_punct(p, "=")) {
        p_advance(p);
        init = p_parse_expr(p, 0);
    }
    MiPos end = init ? init->span.end : name->span.end;
    MiAst *node = mi_ast_new(AST_WIRE, p_span(tok->span.start, end));
    node->name = mi_strdup(name->sval);
    node->a = init;
    return node;
}

static MiAst *p_parse_use(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_USE);
    if (p_peek(p, 0)->kind != TOK_TEXT) {
        mi_diag_add(p->bag, "E0100", p_peek(p, 0)->span, NULL, 0,
                    "expected a module path text after `use`");
        p_fail(p);
    }
    MiTok *path = p_advance(p);
    char *alias = NULL;
    MiPos end = path->span.end;
    if (p_at_kw(p, KW_AS)) {
        p_advance(p);
        MiTok *alias_tok = p_expect_ident(p);
        alias = mi_strdup(alias_tok->sval);
        end = alias_tok->span.end;
    }
    MiAst *node = mi_ast_new(AST_USE, p_span(tok->span.start, end));
    node->name = mi_strdup(path->sval);
    node->name2 = alias;
    return node;
}

static MiAst *p_parse_station(MiParser *p) {
    MiTok *tok = p_expect_kw(p, KW_STATION);
    MiTok *name = p_expect_ident(p);
    p_expect_punct(p, "(");
    MiAst **params = NULL;
    size_t count = 0;
    if (!p_at_punct(p, ")")) {
        for (;;) {
            MiTok *param = p_expect_ident(p);
            MiAst *def = NULL;
            if (p_at_punct(p, ":") || p_at_punct(p, "=")) {
                p_advance(p);
                def = p_parse_expr(p, 0);
            }
            MiPos end = def ? def->span.end : param->span.end;
            MiAst *node = mi_ast_new(AST_PARAM, p_span(param->span.start, end));
            node->name = mi_strdup(param->sval);
            node->a = def;
            p_append(&params, &count, node);
            if (p_at_punct(p, ",")) {
                p_advance(p);
                continue;
            }
            if (p_at_punct(p, ")")) break;
            mi_diag_add(p->bag, "E0103", p_peek(p, 0)->span, NULL, 0,
                        "expected `,` or `)` in the parameter list");
            break;
        }
    }
    p_expect_punct(p, ")");
    MiAst *body = p_parse_block(p);
    MiAst *node = mi_ast_new(AST_STATION, p_span(tok->span.start, body->span.end));
    node->name = mi_strdup(name->sval);
    node->params = params;
    node->param_count = count;
    node->b = body;
    p_collect_out_ports(body, &node->ports, &node->port_count);
    return node;
}

static MiAst *p_parse_item(MiParser *p) {
    MiTok *tok = p_peek(p, 0);
    if (tok->kind == TOK_KW) {
        if (tok->id == KW_WIRE) return p_parse_wire(p);
        if (tok->id == KW_STATION) return p_parse_station(p);
        if (tok->id == KW_USE) return p_parse_use(p);
    }
    return p_parse_flow(p);
}

/* --------------------------------------------------------------- 入口 */
static MiAst *p_parse_program(MiParser *p) {
    MiPos start = p_peek(p, 0)->span.start;
    MiAst **items = NULL;
    size_t count = 0;
    p_skip_seps(p);
    while (!p_at_eof(p)) {
        size_t before = p->pos;
        if (setjmp(p->recover) == 0) {
            MiAst *item = p_parse_item(p);
            if (item) p_append(&items, &count, item);
        } else {
            if (p->pos == before) p_advance(p);
            p_recover(p);
        }
        if (p->pos == before && setjmp(p->recover) == 0) p_advance(p);
        p_skip_seps(p);
    }
    MiPos end = p_peek(p, 0)->span.end;
    MiAst *program = mi_ast_new(AST_PROGRAM, p_span(start, end));
    program->list = items;
    program->count = count;
    return program;
}

MiAst *mi_parse_tokens(MiTokList *tokens, const char *filename, MiDiagBag *bag) {
    MiParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.source = "";
    parser.filename = filename;
    parser.bag = bag;
    p_strip_inner_seps(tokens, &parser.stripped);
    return p_parse_program(&parser);
}

MiAst *mi_parse(const char *source, const char *filename, MiDiagBag *bag) {
    MiTokList tokens;
    memset(&tokens, 0, sizeof(tokens));
    mi_lex(source, filename, &tokens, bag);
    MiAst *program = mi_parse_tokens(&tokens, filename, bag);
    mi_toklist_free(&tokens);
    return program;
}

void mi_ast_free(MiAst *node) {
    if (!node) return;
    free(node->name);
    free(node->name2);
    mi_release(node->literal);
    mi_ast_free(node->a);
    mi_ast_free(node->b);
    mi_ast_free(node->c);
    for (size_t i = 0; i < node->count; i++) mi_ast_free(node->list[i]);
    free(node->list);
    for (size_t i = 0; i < node->param_count; i++) mi_ast_free(node->params[i]);
    free(node->params);
    for (size_t i = 0; i < node->port_count; i++) free(node->ports[i]);
    free(node->ports);
    free(node);
}

MiAst *mi_ast_new(MiAstKind kind, MiSpan span) {
    MiAst *node = mi_alloc(sizeof(MiAst));
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->span = span;
    node->literal = MI_NOTHING_VALUE;
    return node;
}
