/* checker.c — MI 静态检查
 *
 * 只报「运行前就能确定错」的事情：未声明的 wire、写只读端口、未知站点/内置、
 * 实参标签与数量错误、语句放错位置。不做任何执行。
 * 错误码与 Python 版一一对应。
 */
#include "mi.h"
#include "tables.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 名字集 */
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} MiNames;

static void names_add(MiNames *names, const char *name) {
    for (size_t i = 0; i < names->len; i++) {
        if (strcmp(names->items[i], name) == 0) return;
    }
    if (names->len == names->cap) {
        names->cap = names->cap ? names->cap * 2 : 16;
        names->items = mi_realloc(names->items, names->cap * sizeof(char *));
    }
    names->items[names->len++] = mi_strdup(name);
}

static int names_has(const MiNames *names, const char *name) {
    for (size_t i = 0; i < names->len; i++) {
        if (strcmp(names->items[i], name) == 0) return 1;
    }
    return 0;
}

static void names_free(MiNames *names) {
    for (size_t i = 0; i < names->len; i++) free(names->items[i]);
    free(names->items);
    names->items = NULL;
    names->len = names->cap = 0;
}

static MiNames names_copy(const MiNames *names) {
    MiNames out = {NULL, 0, 0};
    for (size_t i = 0; i < names->len; i++) names_add(&out, names->items[i]);
    return out;
}

/* ----------------------------------------------------------------- 作用域 */
typedef struct {
    MiNames wires;
    MiNames params;
    MiNames variables;
    MiNames aliases;
    int allow_unknown_stations;
} MiScope;

static MiScope scope_clone(const MiScope *scope) {
    MiScope out;
    out.wires = names_copy(&scope->wires);
    out.params = names_copy(&scope->params);
    out.variables = names_copy(&scope->variables);
    out.aliases = names_copy(&scope->aliases);
    out.allow_unknown_stations = scope->allow_unknown_stations;
    return out;
}

static void scope_free(MiScope *scope) {
    names_free(&scope->wires);
    names_free(&scope->params);
    names_free(&scope->variables);
    names_free(&scope->aliases);
}

static int scope_has_value(const MiScope *scope, const char *name) {
    return names_has(&scope->wires, name) || names_has(&scope->params, name) ||
           names_has(&scope->variables, name);
}

/* ---------------------------------------------------------------- 检查器 */
typedef struct {
    MiAst *program;
    MiDiagBag *bag;
    MiNames stations;      /* 顶层站点名 */
    MiNames aliases;
    int flat_import;
    MiNames used_wires;    /* 本作用域声明的 wire（只为告警） */
} MiChecker;

static int bi_lookup(const char *name) {
    if (!name || !*name) return -1;
    char low[160];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(low); i++) {
        char ch = name[i];
        low[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    low[i] = '\0';
    for (size_t k = 0; k < MI_BUILTIN_ALL_ALIAS_COUNT; k++) {
        if (strcmp(MI_BUILTIN_ALL[k].spelling, low) == 0) return MI_BUILTIN_ALL[k].builtin;
    }
    for (size_t k = 0; k < MI_BUILTIN_SYMBOL_ALIAS_COUNT; k++) {
        if (strcmp(MI_BUILTIN_SYMBOLS[k].spelling, name) == 0) return MI_BUILTIN_SYMBOLS[k].builtin;
    }
    return -1;
}

static MiAst *find_station(MiChecker *checker, const char *name) {
    for (size_t i = 0; i < checker->program->count; i++) {
        MiAst *item = checker->program->list[i];
        if (item->kind == AST_STATION && item->name && strcmp(item->name, name) == 0) {
            return item;
        }
    }
    return NULL;
}

static void ck_expr(MiChecker *checker, MiAst *expr, MiScope *scope);

static void ck_unknown_name(MiChecker *checker, const char *name, MiSpan span,
                            const char *what) {
    const char *suggestion = mi_suggest(name, MI_KEYWORD_WORDS, MI_KEYWORD_WORD_COUNT, 0.72);
    if (suggestion) {
        char hint[256];
        snprintf(hint, sizeof(hint), "maybe you meant `%s`", suggestion);
        mi_diag_add(checker->bag, "E0101", span, hint, 0, "unknown keyword `%s`", name);
        return;
    }
    mi_diag_add(checker->bag, "E0203", span, NULL, 0, "unknown %s `%s`", what, name);
}

static void ck_value_ref(MiChecker *checker, const char *name, MiSpan span,
                         MiScope *scope, int glyph) {
    if (glyph) {
        if (bi_lookup(name) < 0) {
            mi_diag_add(checker->bag, "E0203", span, NULL, 0,
                        "unknown builtin glyph `%s`", name);
        }
        return;
    }
    if (scope_has_value(scope, name)) return;
    if (strchr(name, '.')) {
        char prefix[128];
        const char *dot = strchr(name, '.');
        size_t len = (size_t)(dot - name);
        if (len >= sizeof(prefix)) len = sizeof(prefix) - 1;
        memcpy(prefix, name, len);
        prefix[len] = '\0';
        if (names_has(&scope->aliases, prefix)) return;
        if (bi_lookup(name) >= 0) return;
        mi_diag_add(checker->bag, "E0203", span, "import the module with `use … as` first",
                    0, "unknown name `%s`", name);
        return;
    }
    if (find_station(checker, name)) return;
    if (bi_lookup(name) >= 0) return;
    if (scope->allow_unknown_stations) return;
    ck_unknown_name(checker, name, span, "name");
}

/* 把调用实参与参数表对照，报 E0204 / E0205 */
static void ck_args(MiChecker *checker, MiAst *call, const char **params,
                    const int *optional, int param_count, const char *variadic,
                    MiSpan span, MiScope *scope) {
    int label_error = 0;
    int overflow = 0;
    int provided_flags[64];
    int provided_max = param_count < 64 ? param_count : 64;
    for (int i = 0; i < provided_max; i++) provided_flags[i] = 0;
    int positional = 0;
    for (size_t i = 0; i < call->count; i++) {
        MiAst *arg = call->list[i];
        if (arg->a) ck_expr(checker, arg->a, scope);
        if (!arg->name) {
            if (positional >= param_count) {
                if (!variadic) overflow = 1;
            } else {
                if (positional < provided_max) provided_flags[positional] = 1;
                positional++;
            }
            continue;
        }
        int found = -1;
        for (int k = 0; k < param_count; k++) {
            if (params[k] && strcmp(params[k], arg->name) == 0) {
                found = k;
                break;
            }
        }
        if (found < 0) {
            if (variadic) continue;
            char hint[256];
            const char *suggestion = mi_suggest(arg->name, params, param_count, 0.6);
            if (suggestion) {
                snprintf(hint, sizeof(hint), "did you mean `%s`?", suggestion);
            } else {
                size_t pos = 0;
                hint[0] = '\0';
                pos += (size_t)snprintf(hint, sizeof(hint), "parameters: ");
                for (int k = 0; k < param_count && pos + 32 < sizeof(hint); k++) {
                    pos += (size_t)snprintf(hint + pos, sizeof(hint) - pos, "%s%s",
                                            k ? ", " : "", params[k]);
                }
                if (param_count == 0) snprintf(hint, sizeof(hint), "parameters: none");
            }
            mi_diag_add(checker->bag, "E0204", arg->span, hint, 0,
                        "unknown argument label `%s`", arg->name);
            label_error = 1;
            continue;
        }
        if (found < provided_max && provided_flags[found]) {
            mi_diag_add(checker->bag, "E0204", arg->span, NULL, 0,
                        "argument `%s` is given twice", arg->name);
            label_error = 1;
            continue;
        }
        if (found < provided_max) provided_flags[found] = 1;
    }
    if (overflow) {
        mi_diag_add(checker->bag, "E0205", span, NULL, 0, "too many arguments");
    }
    if (label_error) return;
    for (int k = 0; k < param_count; k++) {
        if (optional && optional[k]) continue;
        if (variadic && params[k] && strcmp(params[k], variadic) == 0) continue;
        if (!provided_flags[k]) {
            mi_diag_add(checker->bag, "E0205", span, NULL, 0,
                        "missing argument `%s`", params[k] ? params[k] : "");
        }
    }
}

static void ck_call(MiChecker *checker, MiAst *call, MiScope *scope) {
    if (call->flag && call->name) { /* glyph 调用 */
        int id = bi_lookup(call->name);
        if (id < 0) {
            mi_diag_add(checker->bag, "E0203", call->span, NULL, 0,
                        "unknown builtin `%s`", call->name);
            for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
            return;
        }
        const MiParamSpec *specs = MI_BUILTIN_PARAMS[id];
        const char *params[16];
        int optional[16];
        int count = 0;
        const char *variadic = NULL;
        if (specs) {
            for (int i = 0; specs[i].name && i < 16; i++) {
                params[count] = specs[i].name;
                optional[count] = specs[i].optional;
                if (specs[i].variadic) variadic = specs[i].name;
                count++;
            }
        }
        ck_args(checker, call, params, optional, count, variadic, call->span, scope);
        return;
    }
    if (!call->name) {
        if (call->a) ck_expr(checker, call->a, scope);
        for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
        return;
    }
    const char *name = call->name;
    if (strchr(name, '.')) {
        char prefix[128];
        const char *dot = strchr(name, '.');
        size_t len = (size_t)(dot - name);
        if (len >= sizeof(prefix)) len = sizeof(prefix) - 1;
        memcpy(prefix, name, len);
        prefix[len] = '\0';
        if (names_has(&scope->aliases, prefix)) {
            for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
            return;
        }
        int id = bi_lookup(name);
        if (id < 0) {
            mi_diag_add(checker->bag, "E0203", call->span,
                        "module aliases must be imported with `use … as`", 0,
                        "unknown name `%s`", name);
            for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
            return;
        }
        const MiParamSpec *specs = MI_BUILTIN_PARAMS[id];
        const char *params[16];
        int optional[16];
        int count = 0;
        const char *variadic = NULL;
        if (specs) {
            for (int i = 0; specs[i].name && i < 16; i++) {
                params[count] = specs[i].name;
                optional[count] = specs[i].optional;
                if (specs[i].variadic) variadic = specs[i].name;
                count++;
            }
        }
        ck_args(checker, call, params, optional, count, variadic, call->span, scope);
        return;
    }
    if (scope_has_value(scope, name)) {
        for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
        return;
    }
    MiAst *station = find_station(checker, name);
    if (station) {
        const char *params[64];
        int optional[64];
        int count = 0;
        for (size_t i = 0; i < station->param_count && count < 64; i++) {
            params[count] = station->params[i]->name;
            optional[count] = station->params[i]->a != NULL;
            count++;
        }
        ck_args(checker, call, params, optional, count, NULL, call->span, scope);
        return;
    }
    int id = bi_lookup(name);
    if (id >= 0) {
        const MiParamSpec *specs = MI_BUILTIN_PARAMS[id];
        const char *params[16];
        int optional[16];
        int count = 0;
        const char *variadic = NULL;
        if (specs) {
            for (int i = 0; specs[i].name && i < 16; i++) {
                params[count] = specs[i].name;
                optional[count] = specs[i].optional;
                if (specs[i].variadic) variadic = specs[i].name;
                count++;
            }
        }
        ck_args(checker, call, params, optional, count, variadic, call->span, scope);
        return;
    }
    if (scope->allow_unknown_stations) {
        for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
        return;
    }
    if (call->flag2) { /* 解析器已报 E0101 */
        for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
        return;
    }
    mi_diag_add(checker->bag, "E0203", call->span, NULL, 0,
                "unknown station or builtin `%s`", name);
    for (size_t i = 0; i < call->count; i++) ck_expr(checker, call->list[i]->a, scope);
}

static void ck_expr(MiChecker *checker, MiAst *expr, MiScope *scope) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_NUMBER:
        case AST_TEXT:
        case AST_FLAG:
        case AST_NIL:
            return;
        case AST_LIST:
            for (size_t i = 0; i < expr->count; i++) ck_expr(checker, expr->list[i], scope);
            return;
        case AST_ASSEMBLY:
            for (size_t i = 0; i < expr->count; i++) ck_expr(checker, expr->list[i]->a, scope);
            return;
        case AST_NAME:
            ck_value_ref(checker, expr->name, expr->span, scope, expr->flag);
            return;
        case AST_READ:
            if (!scope_has_value(scope, expr->name)) {
                mi_diag_add(checker->bag, "E0200", expr->span, "declare it with `wire`",
                            0, "`draw` needs a declared wire, but `%s` is not one",
                            expr->name);
            }
            return;
        case AST_FIELD:
            ck_expr(checker, expr->a, scope);
            return;
        case AST_UNARY:
            ck_expr(checker, expr->a, scope);
            return;
        case AST_BINARY:
            ck_expr(checker, expr->a, scope);
            ck_expr(checker, expr->b, scope);
            return;
        case AST_CALL:
            ck_call(checker, expr, scope);
            return;
        default:
            return;
    }
}

static void ck_flow(MiChecker *checker, MiAst *flow, MiScope *scope, int in_station,
                    int cycle_depth);

static void ck_block(MiChecker *checker, MiAst *block, MiScope *scope, int in_station,
                     int cycle_depth) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        ck_flow(checker, block->list[i], scope, in_station, cycle_depth);
    }
}

static void ck_flow(MiChecker *checker, MiAst *flow, MiScope *scope, int in_station,
                    int cycle_depth) {
    if (!flow) return;
    switch (flow->kind) {
        case AST_WIRE: {
            if (flow->a) ck_expr(checker, flow->a, scope);
            if (names_has(&scope->params, flow->name) ||
                names_has(&scope->variables, flow->name)) {
                mi_diag_add(checker->bag, "E0202", flow->span, NULL, 0,
                            "`%s` is a read-only port and cannot be redeclared as a wire",
                            flow->name);
            } else if (names_has(&scope->wires, flow->name)) {
                mi_diag_add(checker->bag, "E0201", flow->span, NULL, 0,
                            "wire `%s` is declared twice", flow->name);
            } else {
                names_add(&scope->wires, flow->name);
                names_add(&checker->used_wires, flow->name);
            }
            return;
        }
        case AST_EMIT: {
            if (names_has(&scope->params, flow->name) ||
                names_has(&scope->variables, flow->name)) {
                mi_diag_add(checker->bag, "E0202", flow->span,
                            "declare a local wire with `wire` and emit into that", 0,
                            "`%s` is a read-only port and cannot be written", flow->name);
            } else if (!names_has(&scope->wires, flow->name)) {
                mi_diag_add(checker->bag, "E0200", flow->span, "declare it with `wire`",
                            0, "wire `%s` is used before it is declared", flow->name);
            }
            ck_expr(checker, flow->a, scope);
            return;
        }
        case AST_WHEN: {
            ck_expr(checker, flow->a, scope);
            MiScope inner = scope_clone(scope);
            ck_block(checker, flow->b, &inner, in_station, cycle_depth);
            scope_free(&inner);
            if (flow->c) {
                MiScope other = scope_clone(scope);
                if (flow->c->kind == AST_BLOCK) {
                    ck_block(checker, flow->c, &other, in_station, cycle_depth);
                } else {
                    ck_flow(checker, flow->c, &other, in_station, cycle_depth);
                }
                scope_free(&other);
            }
            return;
        }
        case AST_CYCLE: {
            if (flow->a) ck_expr(checker, flow->a, scope);
            MiScope inner = scope_clone(scope);
            if (flow->name) names_add(&inner.variables, flow->name);
            ck_block(checker, flow->b, &inner, in_station, cycle_depth + 1);
            scope_free(&inner);
            return;
        }
        case AST_FOR: {
            ck_expr(checker, flow->a, scope);
            MiScope inner = scope_clone(scope);
            names_add(&inner.variables, flow->name);
            ck_block(checker, flow->b, &inner, in_station, cycle_depth + 1);
            scope_free(&inner);
            return;
        }
        case AST_OUT:
            if (!in_station) {
                mi_diag_add(checker->bag, "E0206", flow->span, NULL, 0,
                            "`out` is only allowed inside a station");
            } else {
                ck_expr(checker, flow->a, scope);
            }
            return;
        case AST_RETURN:
            if (!in_station) {
                mi_diag_add(checker->bag, "E0206", flow->span, NULL, 0,
                            "`return` is only allowed inside a station");
            }
            if (flow->a) ck_expr(checker, flow->a, scope);
            return;
        case AST_HALT:
            return;
        case AST_SKIP:
            if (cycle_depth == 0) {
                mi_diag_add(checker->bag, "E0304", flow->span,
                            "use `halt` to leave a station", 0,
                            "`skip` can only be used inside a cycle");
            }
            return;
        case AST_FLOW:
            ck_expr(checker, flow->a, scope);
            return;
        case AST_STATION:
            mi_diag_add(checker->bag, "E0206", flow->span,
                        "move it out of the block", 0,
                        "stations must be declared at the top level");
            return;
        case AST_USE:
            mi_diag_add(checker->bag, "E0206", flow->span, NULL, 0,
                        "`use` is only allowed at the top level");
            return;
        default:
            return;
    }
}

void mi_check(MiAst *program, const char *filename, MiDiagBag *bag) {
    (void)filename;
    MiChecker checker;
    memset(&checker, 0, sizeof(checker));
    checker.program = program;
    checker.bag = bag;

    /* 收集顶层站点与别名 */
    for (size_t i = 0; i < program->count; i++) {
        MiAst *item = program->list[i];
        if (item->kind == AST_STATION) {
            if (names_has(&checker.stations, item->name)) {
                mi_diag_add(bag, "E0201", item->span, NULL, 0,
                            "station `%s` is declared twice", item->name);
                continue;
            }
            names_add(&checker.stations, item->name);
        } else if (item->kind == AST_USE) {
            if (item->name2) {
                names_add(&checker.aliases, item->name2);
            } else {
                checker.flat_import = 1;
            }
        }
    }

    /* 各站点体内检查 */
    for (size_t i = 0; i < program->count; i++) {
        MiAst *station = program->list[i];
        if (station->kind != AST_STATION) continue;
        MiScope scope;
        memset(&scope, 0, sizeof(scope));
        scope.aliases = names_copy(&checker.aliases);
        scope.allow_unknown_stations = checker.flat_import;
        for (size_t k = 0; k < station->param_count; k++) {
            MiAst *param = station->params[k];
            if (param->a) ck_expr(&checker, param->a, &scope);
            names_add(&scope.params, param->name);
        }
        ck_block(&checker, station->b, &scope, 1, 0);
        scope_free(&scope);
    }

    /* 顶层流程检查 */
    {
        MiScope scope;
        memset(&scope, 0, sizeof(scope));
        scope.aliases = names_copy(&checker.aliases);
        scope.allow_unknown_stations = checker.flat_import;
        for (size_t i = 0; i < program->count; i++) {
            MiAst *item = program->list[i];
            if (item->kind == AST_STATION || item->kind == AST_USE) continue;
            ck_flow(&checker, item, &scope, 0, 0);
        }
        scope_free(&scope);
    }

    names_free(&checker.stations);
    names_free(&checker.aliases);
    names_free(&checker.used_wires);
}
