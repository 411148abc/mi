/* eval.c — MI 求值器 + 内置函数 + JSON + 模块加载
 *
 * 执行模型与 Python 版一致：顶层流程按顺序执行；站点是带具名端口的纯函数；
 * 唯一的可变状态是当前作用域的 wire 帧；所有值不可变。
 */
#include "mi.h"
#include "tables.h"

#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ================================================================ 环境 */
typedef struct {
    char *name;
    MiValue value;
} MiBinding;

typedef struct {
    MiBinding *items;
    size_t len;
    size_t cap;
} MiEnv;

static MiValue *env_slot(MiEnv *env, const char *name, int create) {
    for (size_t i = 0; i < env->len; i++) {
        if (strcmp(env->items[i].name, name) == 0) return &env->items[i].value;
    }
    if (!create) return NULL;
    if (env->len == env->cap) {
        env->cap = env->cap ? env->cap * 2 : 16;
        env->items = mi_realloc(env->items, env->cap * sizeof(MiBinding));
    }
    env->items[env->len].name = mi_strdup(name);
    env->items[env->len].value = MI_NOTHING_VALUE;
    return &env->items[env->len++].value;
}

static void env_set(MiEnv *env, const char *name, MiValue value) {
    MiValue *slot = env_slot(env, name, 1);
    mi_release(*slot);
    *slot = value;
}

static MiValue env_get(MiEnv *env, const char *name, int *found) {
    MiValue *slot = env_slot(env, name, 0);
    if (!slot) {
        if (found) *found = 0;
        return MI_NOTHING_VALUE;
    }
    if (found) *found = 1;
    return *slot;
}

static void env_free(MiEnv *env) {
    for (size_t i = 0; i < env->len; i++) {
        free(env->items[i].name);
        mi_release(env->items[i].value);
    }
    free(env->items);
    env->items = NULL;
    env->len = env->cap = 0;
}

/* ================================================================ 模块 */
MiModule *mi_module_new(const char *path) {
    MiModule *module = mi_calloc(1, sizeof(MiModule));
    module->path = mi_strdup(path ? path : "<source>");
    return module;
}

static MiModule **g_modules = NULL;
static size_t g_module_count = 0;
static size_t g_module_cap = 0;

void mi_module_free(MiModule *module) {
    if (!module) return;
    for (size_t i = 0; i < module->station_count; i++) free(module->stations[i].name);
    free(module->stations);
    for (size_t i = 0; i < module->import_count; i++) free(module->import_names[i]);
    free(module->import_names);
    free(module->imports);
    free(module->flat);
    free(module->path);
    mi_ast_free(module->program);
    free(module);
}

void mi_module_add_station(MiModule *module, const char *name, MiAst *decl) {
    module->stations =
        mi_realloc(module->stations, (module->station_count + 1) * sizeof(MiModuleStation));
    module->stations[module->station_count].name = mi_strdup(name);
    module->stations[module->station_count].decl = decl;
    module->station_count++;
}

MiAst *mi_module_find_station(MiModule *module, const char *name) {
    for (size_t i = 0; i < module->station_count; i++) {
        if (strcmp(module->stations[i].name, name) == 0) return module->stations[i].decl;
    }
    return NULL;
}

MiModule *mi_module_find_owning(MiModule *module, const char *name) {
    if (mi_module_find_station(module, name)) return module;
    for (size_t i = 0; i < module->flat_count; i++) {
        MiModule *owner = mi_module_find_owning(module->flat[i], name);
        if (owner) return owner;
    }
    return NULL;
}

/* ================================================================ 加载器 */
struct MiLoader {
    char *error;
    MiSpan error_span;
    int failed;
};

MiLoader *mi_loader_new(void) { return mi_calloc(1, sizeof(MiLoader)); }

void mi_loader_free(MiLoader *loader) {
    if (!loader) return;
    free(loader->error);
    for (size_t i = 0; i < g_module_count; i++) mi_module_free(g_modules[i]);
    free(g_modules);
    g_modules = NULL;
    g_module_count = g_module_cap = 0;
    free(loader);
}

static int path_is_absolute(const char *path) {
    if (path[0] == '/' || path[0] == '\\') return 1;
    return path[0] && path[1] == ':';
}

static char *path_dirname(const char *path) {
    const char *last = NULL;
    for (const char *cursor = path; *cursor; cursor++) {
        if (*cursor == '/' || *cursor == '\\') last = cursor;
    }
    if (!last) return mi_strdup("");
    return mi_strndup(path, (size_t)(last - path));
}

static int has_suffix(const char *text, const char *suffix) {
    size_t a = strlen(text);
    size_t b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

MiModule *mi_loader_load(MiLoader *loader, const char *path, const char *origin) {
    /* 解析路径 */
    char *resolved = NULL;
    if (path_is_absolute(path)) {
        resolved = mi_strdup(path);
    } else {
        int virtual_origin = (!origin || !*origin || origin[0] == '<');
        char *base = virtual_origin ? mi_strdup("") : path_dirname(origin);
        if (*base) {
            resolved = mi_asprintf("%s/%s", base, path);
        } else {
            resolved = mi_strdup(path);
        }
        free(base);
    }

    /* 已缓存 */
    for (size_t i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i]->path, resolved) == 0) {
            free(resolved);
            return g_modules[i];
        }
    }

    char *source = mi_read_source(resolved);
    if (!source && !has_suffix(resolved, ".mi")) {
        char *with_suffix = mi_asprintf("%s.mi", resolved);
        source = mi_read_source(with_suffix);
        if (source) {
            free(resolved);
            resolved = with_suffix;
        } else {
            free(with_suffix);
        }
    }
    if (!source) {
        loader->failed = 1;
        free(loader->error);
        loader->error = mi_asprintf("cannot load module `%s`", path);
        free(resolved);
        return NULL;
    }

    MiDiagBag bag;
    mi_diag_init(&bag);
    MiAst *program = mi_parse(source, resolved, &bag);
    if (bag.errors > 0) {
        loader->failed = 1;
        free(loader->error);
        loader->error = mi_asprintf("cannot load module `%s`", path);
        mi_ast_free(program);
        mi_diag_free(&bag);
        free(source);
        free(resolved);
        return NULL;
    }
    mi_diag_free(&bag);

    MiModule *module = mi_module_new(resolved);
    module->program = program;
    for (size_t i = 0; i < program->count; i++) {
        MiAst *item = program->list[i];
        if (item->kind == AST_STATION) mi_module_add_station(module, item->name, item);
    }

    /* 先登记再递归，避免循环引用无限展开 */
    if (g_module_count == g_module_cap) {
        g_module_cap = g_module_cap ? g_module_cap * 2 : 8;
        g_modules = mi_realloc(g_modules, g_module_cap * sizeof(MiModule *));
    }
    g_modules[g_module_count++] = module;

    /* 再跟进这个模块自己的 `use`（相对于它自己的路径解析） */
    for (size_t i = 0; i < program->count; i++) {
        MiAst *item = program->list[i];
        if (item->kind != AST_USE) continue;
        MiModule *child = mi_loader_load(loader, item->name, resolved);
        if (!child) {
            mi_ast_free(program);
            module->program = NULL;
            free(source);
            free(resolved);
            return NULL;
        }
        if (item->name2) {
            module->imports =
                mi_realloc(module->imports, (module->import_count + 1) * sizeof(MiModule *));
            module->import_names = mi_realloc(
                module->import_names, (module->import_count + 1) * sizeof(char *));
            module->imports[module->import_count] = child;
            module->import_names[module->import_count] = mi_strdup(item->name2);
            module->import_count++;
        } else {
            module->flat =
                mi_realloc(module->flat, (module->flat_count + 1) * sizeof(MiModule *));
            module->flat[module->flat_count++] = child;
        }
    }

    free(source);
    free(resolved);
    return module;
}

/* ================================================================ 运行时 */
typedef struct {
    MiModule *module;
    MiAst *station;
    MiEnv params;
    MiEnv wires;
    MiEnv variables;
    MiEnv ports;
} MiFrame;

typedef struct {
    MiLoader *loader;
    MiOutput *output;
    int steps;
    int max_steps;
    int depth;
    int max_depth;
    jmp_buf unwind;
    int failed;
    MiError error;
} MiState;

static MiState *g_state = NULL;

void mi_unwind(void) {
    if (g_state) longjmp(g_state->unwind, 1);
    abort();
}

static void tick(MiSpan span) {
    MiState *state = g_state;
    state->steps++;
    if (state->steps > state->max_steps) {
        mi_raise("E0305", span, "raise it with --max-steps, or make the cycle terminate",
                 "step budget of %d exhausted", state->max_steps);
    }
}

static int g_stream_output = 0;

void mi_set_stream_output(int enabled) { g_stream_output = enabled; }

static void output_line(const char *text) {
    MiOutput *output = g_state->output;
    if (output->count == output->cap) {
        output->cap = output->cap ? output->cap * 2 : 64;
        output->lines = mi_realloc(output->lines, output->cap * sizeof(char *));
    }
    output->lines[output->count++] = mi_strdup(text);
    if (g_stream_output) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

static void frame_free(MiFrame *frame) {
    env_free(&frame->params);
    env_free(&frame->wires);
    env_free(&frame->variables);
    env_free(&frame->ports);
}

static void frame_init(MiFrame *frame, MiModule *module, MiAst *station) {
    memset(frame, 0, sizeof(*frame));
    frame->module = module;
    frame->station = station;
}

typedef struct {
    int kind; /* 0 none, 1 halt, 2 skip, 3 return */
    MiValue value;
} MiSignal;

static MiSignal g_signal;

/* 前置声明 */
static MiValue eval_expr(MiFrame *frame, MiAst *expr);
static void exec_flow(MiFrame *frame, MiAst *flow);
static void exec_block(MiFrame *frame, MiAst *block);
static MiValue call_station(MiModule *module, MiAst *station, MiBinding *args, size_t count,
                            MiSpan span);
static MiValue call_builtin(int id, MiBinding *args, size_t count, MiSpan span);

/* ================================================================ JSON */
typedef struct {
    const char *text;
    size_t pos;
    size_t len;
    int failed;
} MiJson;

static void json_skip_space(MiJson *json) {
    while (json->pos < json->len) {
        char ch = json->text[json->pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            json->pos++;
        } else {
            break;
        }
    }
}

static MiValue json_value(MiJson *json);

static void json_utf8_push(char **buffer, size_t *len, size_t *cap, unsigned code) {
    char bytes[4];
    size_t count = mi_utf8_encode(code, bytes);
    if (*len + count + 1 > *cap) {
        size_t newcap = *cap ? *cap : 32;
        while (newcap < *len + count + 1) newcap *= 2;
        *buffer = mi_realloc(*buffer, newcap);
        *cap = newcap;
    }
    memcpy(*buffer + *len, bytes, count);
    *len += count;
    (*buffer)[*len] = '\0';
}

static MiValue json_string(MiJson *json) {
    char *buffer = NULL;
    size_t len = 0;
    size_t cap = 0;
    json_utf8_push(&buffer, &len, &cap, 0);
    len = 0;
    buffer[0] = '\0';
    json->pos++; /* 开引号 */
    while (json->pos < json->len) {
        unsigned char ch = (unsigned char)json->text[json->pos++];
        if (ch == '"') {
            MiValue out = mi_text_new(buffer ? buffer : "");
            free(buffer);
            return out;
        }
        if (ch == '\\') {
            if (json->pos >= json->len) break;
            unsigned char esc = (unsigned char)json->text[json->pos++];
            unsigned code = 0;
            switch (esc) {
                case '"': code = '"'; break;
                case '\\': code = '\\'; break;
                case '/': code = '/'; break;
                case 'b': code = '\b'; break;
                case 'f': code = '\f'; break;
                case 'n': code = '\n'; break;
                case 'r': code = '\r'; break;
                case 't': code = '\t'; break;
                case 'u': {
                    unsigned value = 0;
                    for (int i = 0; i < 4 && json->pos < json->len; i++) {
                        char digit = json->text[json->pos++];
                        unsigned nibble = 0;
                        if (digit >= '0' && digit <= '9') nibble = (unsigned)(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') nibble = (unsigned)(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') nibble = (unsigned)(digit - 'A' + 10);
                        value = value * 16 + nibble;
                    }
                    json_utf8_push(&buffer, &len, &cap, value);
                    continue;
                }
                default: code = esc; break;
            }
            json_utf8_push(&buffer, &len, &cap, code);
            continue;
        }
        json_utf8_push(&buffer, &len, &cap, ch);
    }
    json->failed = 1;
    free(buffer);
    return MI_NOTHING_VALUE;
}

static MiValue json_value(MiJson *json) {
    json_skip_space(json);
    if (json->pos >= json->len) {
        json->failed = 1;
        return MI_NOTHING_VALUE;
    }
    char ch = json->text[json->pos];
    if (ch == '{') {
        json->pos++;
        MiAsm *asmv = mi_asm_alloc(0);
        json_skip_space(json);
        if (json->pos < json->len && json->text[json->pos] == '}') {
            json->pos++;
            return mi_asm_take(asmv);
        }
        for (;;) {
            json_skip_space(json);
            if (json->pos >= json->len || json->text[json->pos] != '"') {
                json->failed = 1;
                mi_release(mi_asm_take(asmv));
                return MI_NOTHING_VALUE;
            }
            MiValue key = json_string(json);
            char *key_text = mi_strdup(key.as.text->data);
            mi_release(key);
            json_skip_space(json);
            if (json->pos >= json->len || json->text[json->pos] != ':') {
                json->failed = 1;
                free(key_text);
                mi_release(mi_asm_take(asmv));
                return MI_NOTHING_VALUE;
            }
            json->pos++;
            MiValue value = json_value(json);
            mi_asm_set(asmv, key_text, value);
            free(key_text);
            json_skip_space(json);
            if (json->pos < json->len && json->text[json->pos] == ',') {
                json->pos++;
                continue;
            }
            if (json->pos < json->len && json->text[json->pos] == '}') {
                json->pos++;
                return mi_asm_take(asmv);
            }
            json->failed = 1;
            mi_release(mi_asm_take(asmv));
            return MI_NOTHING_VALUE;
        }
    }
    if (ch == '[') {
        json->pos++;
        MiList *list = mi_list_alloc(0);
        json_skip_space(json);
        if (json->pos < json->len && json->text[json->pos] == ']') {
            json->pos++;
            return mi_list_take(list);
        }
        for (;;) {
            mi_list_push(list, json_value(json));
            json_skip_space(json);
            if (json->pos < json->len && json->text[json->pos] == ',') {
                json->pos++;
                continue;
            }
            if (json->pos < json->len && json->text[json->pos] == ']') {
                json->pos++;
                return mi_list_take(list);
            }
            json->failed = 1;
            mi_release(mi_list_take(list));
            return MI_NOTHING_VALUE;
        }
    }
    if (ch == '"') return json_string(json);
    if (json->len - json->pos >= 4 && strncmp(json->text + json->pos, "true", 4) == 0) {
        json->pos += 4;
        return mi_flag(1);
    }
    if (json->len - json->pos >= 5 && strncmp(json->text + json->pos, "false", 5) == 0) {
        json->pos += 5;
        return mi_flag(0);
    }
    if (json->len - json->pos >= 4 && strncmp(json->text + json->pos, "null", 4) == 0) {
        json->pos += 4;
        return MI_NOTHING_VALUE;
    }
    /* 数字 */
    {
        const char *start = json->text + json->pos;
        char *end = NULL;
        int is_real = 0;
        for (const char *cursor = start;
             *cursor && (cursor - start) < 64; cursor++) {
            if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') is_real = 1;
            if (*cursor == ',' || *cursor == '}' || *cursor == ']' ||
                *cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
                break;
            }
        }
        if (is_real) {
            double value = strtod(start, &end);
            if (!end || end == start) {
                json->failed = 1;
                return MI_NOTHING_VALUE;
            }
            json->pos = (size_t)(end - json->text);
            return mi_real(value);
        }
        long long value = strtoll(start, &end, 10);
        if (!end || end == start) {
            json->failed = 1;
            return MI_NOTHING_VALUE;
        }
        json->pos = (size_t)(end - json->text);
        return mi_int(value);
    }
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MiJsonBuf;

static void jb_append(MiJsonBuf *buf, const char *text) {
    size_t len = strlen(text);
    if (buf->len + len + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 64;
        while (cap < buf->len + len + 1) cap *= 2;
        buf->data = mi_realloc(buf->data, cap);
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void json_encode_into(MiJsonBuf *buf, MiValue value) {
    switch (value.kind) {
        case MI_NOTHING:
            jb_append(buf, "null");
            return;
        case MI_FLAG:
            jb_append(buf, value.as.flag ? "true" : "false");
            return;
        case MI_INT: {
            char text[32];
            snprintf(text, sizeof(text), "%lld", (long long)value.as.i);
            jb_append(buf, text);
            return;
        }
        case MI_REAL: {
            char *text = mi_number_text(value);
            jb_append(buf, text);
            free(text);
            return;
        }
        case MI_TEXT: {
            jb_append(buf, "\"");
            for (size_t i = 0; i < value.as.text->len; i++) {
                unsigned char ch = (unsigned char)value.as.text->data[i];
                char piece[8];
                if (ch == '"') jb_append(buf, "\\\"");
                else if (ch == '\\') jb_append(buf, "\\\\");
                else if (ch == '\n') jb_append(buf, "\\n");
                else if (ch == '\r') jb_append(buf, "\\r");
                else if (ch == '\t') jb_append(buf, "\\t");
                else if (ch < 0x20) {
                    snprintf(piece, sizeof(piece), "\\u%04x", ch);
                    jb_append(buf, piece);
                } else {
                    piece[0] = (char)ch;
                    piece[1] = '\0';
                    jb_append(buf, piece);
                }
            }
            jb_append(buf, "\"");
            return;
        }
        case MI_LIST:
            jb_append(buf, "[");
            for (size_t i = 0; i < value.as.list->len; i++) {
                if (i) jb_append(buf, ",");
                json_encode_into(buf, value.as.list->items[i]);
            }
            jb_append(buf, "]");
            return;
        case MI_ASSEMBLY:
            jb_append(buf, "{");
            for (size_t i = 0; i < value.as.asmv->len; i++) {
                if (i) jb_append(buf, ",");
                MiValue key = mi_text_new(value.as.asmv->names[i]);
                json_encode_into(buf, key);
                mi_release(key);
                jb_append(buf, ":");
                json_encode_into(buf, value.as.asmv->values[i]);
            }
            jb_append(buf, "}");
            return;
        default:
            mi_raise("E0300", (MiSpan){{0, 0, 0}, {0, 0, 0}}, NULL,
                     "cannot write %s to JSON; only number, text, flag, nothing, list and assembly",
                     mi_kind_name(value));
    }
}

static char *json_encode(MiValue value) {
    MiJsonBuf buf = {NULL, 0, 0};
    jb_append(&buf, "");
    buf.len = 0;
    buf.data[0] = '\0';
    json_encode_into(&buf, value);
    return buf.data;
}

/* ================================================================ 比较 */
static int value_compare(MiValue a, MiValue b) {
    int a_int = (a.kind == MI_INT || a.kind == MI_BIG);
    int b_int = (b.kind == MI_INT || b.kind == MI_BIG);
    if (a_int && b_int) {
        if (a.kind == MI_BIG || b.kind == MI_BIG) {
            MiBig *x = mi_value_to_big(a);
            MiBig *y = mi_value_to_big(b);
            int cmp = mi_big_cmp(x, y);
            mi_big_free(x);
            mi_big_free(y);
            return cmp;
        }
        return a.as.i < b.as.i ? -1 : (a.as.i > b.as.i ? 1 : 0);
    }
    if ((a.kind == MI_INT || a.kind == MI_REAL) &&
        (b.kind == MI_INT || b.kind == MI_REAL)) {
        double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
        double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.kind == MI_TEXT && b.kind == MI_TEXT) {
        int cmp = strcmp(a.as.text->data, b.as.text->data);
        return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    if (a.kind == MI_LIST && b.kind == MI_LIST) {
        size_t n = a.as.list->len < b.as.list->len ? a.as.list->len : b.as.list->len;
        for (size_t i = 0; i < n; i++) {
            int cmp = value_compare(a.as.list->items[i], b.as.list->items[i]);
            if (cmp) return cmp;
        }
        if (a.as.list->len == b.as.list->len) return 0;
        return a.as.list->len < b.as.list->len ? -1 : 1;
    }
    mi_raise("E0300", (MiSpan){{0, 0, 0}, {0, 0, 0}}, NULL,
             "cannot compare %s with %s", mi_kind_name(a), mi_kind_name(b));
    return 0;
}

/* ================================================================ 运算符 */
static MiValue binary_op(const char *op, MiValue a, MiValue b, MiSpan span) {
    if (strcmp(op, "PLUS") == 0) {
        if (a.kind == MI_FLAG || b.kind == MI_FLAG) {
            mi_raise("E0300", span, NULL, "cannot add flags");
        }
        if ((a.kind == MI_INT || a.kind == MI_REAL || a.kind == MI_BIG) &&
            (b.kind == MI_INT || b.kind == MI_REAL || b.kind == MI_BIG)) {
            int a_int = (a.kind == MI_INT || a.kind == MI_BIG);
            int b_int = (b.kind == MI_INT || b.kind == MI_BIG);
            if (a_int && b_int) {
                if (a.kind == MI_INT && b.kind == MI_INT) {
                    int64_t out;
                    if (!__builtin_add_overflow(a.as.i, b.as.i, &out)) return mi_int(out);
                }
                MiBig *x = mi_value_to_big(a);
                MiBig *y = mi_value_to_big(b);
                MiBig *sum = mi_big_add(x, y);
                mi_big_free(x);
                mi_big_free(y);
                return mi_value_from_big(sum);
            }
            double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
            double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
            return mi_real(x + y);
        }
        if (a.kind == MI_TEXT && b.kind == MI_TEXT) {
            size_t len = a.as.text->len + b.as.text->len;
            MiStr *str = mi_str_alloc(len);
            memcpy(str->data, a.as.text->data, a.as.text->len);
            memcpy(str->data + a.as.text->len, b.as.text->data, b.as.text->len);
            return mi_text_take(str);
        }
        if (a.kind == MI_LIST && b.kind == MI_LIST) {
            MiList *list = mi_list_alloc(a.as.list->len + b.as.list->len);
            for (size_t i = 0; i < a.as.list->len; i++) {
                mi_list_push(list, mi_retain(a.as.list->items[i]));
            }
            for (size_t i = 0; i < b.as.list->len; i++) {
                mi_list_push(list, mi_retain(b.as.list->items[i]));
            }
            return mi_list_take(list);
        }
        if (a.kind == MI_ASSEMBLY && b.kind == MI_ASSEMBLY) {
            MiAsm *asmv = mi_asm_copy(a.as.asmv);
            for (size_t i = 0; i < b.as.asmv->len; i++) {
                mi_asm_set(asmv, b.as.asmv->names[i], mi_retain(b.as.asmv->values[i]));
            }
            return mi_asm_take(asmv);
        }
        mi_raise("E0300", span, NULL, "cannot add %s and %s", mi_kind_name(a),
                 mi_kind_name(b));
    }
    if (strcmp(op, "MINUS") == 0) {
        if ((a.kind == MI_INT || a.kind == MI_REAL || a.kind == MI_BIG) &&
            (b.kind == MI_INT || b.kind == MI_REAL || b.kind == MI_BIG)) {
            int a_int = (a.kind == MI_INT || a.kind == MI_BIG);
            int b_int = (b.kind == MI_INT || b.kind == MI_BIG);
            if (a_int && b_int) {
                if (a.kind == MI_INT && b.kind == MI_INT) {
                    int64_t out;
                    if (!__builtin_sub_overflow(a.as.i, b.as.i, &out)) return mi_int(out);
                }
                MiBig *x = mi_value_to_big(a);
                MiBig *y = mi_value_to_big(b);
                MiBig *diff = mi_big_sub(x, y);
                mi_big_free(x);
                mi_big_free(y);
                return mi_value_from_big(diff);
            }
            double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
            double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
            return mi_real(x - y);
        }
        mi_raise("E0300", span, NULL, "cannot subtract %s from %s", mi_kind_name(b),
                 mi_kind_name(a));
    }
    if (strcmp(op, "TIMES") == 0) {
        if (a.kind == MI_FLAG || b.kind == MI_FLAG) {
            mi_raise("E0300", span, NULL, "cannot multiply flags");
        }
        if ((a.kind == MI_INT || a.kind == MI_REAL || a.kind == MI_BIG) &&
            (b.kind == MI_INT || b.kind == MI_REAL || b.kind == MI_BIG)) {
            int a_int = (a.kind == MI_INT || a.kind == MI_BIG);
            int b_int = (b.kind == MI_INT || b.kind == MI_BIG);
            if (a_int && b_int) {
                if (a.kind == MI_INT && b.kind == MI_INT) {
                    int64_t out;
                    if (!__builtin_mul_overflow(a.as.i, b.as.i, &out)) return mi_int(out);
                }
                MiBig *x = mi_value_to_big(a);
                MiBig *y = mi_value_to_big(b);
                MiBig *product = mi_big_mul(x, y);
                mi_big_free(x);
                mi_big_free(y);
                return mi_value_from_big(product);
            }
            double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
            double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
            return mi_real(x * y);
        }
        /* 文本/列表重复 */
        MiValue repeated = MI_NOTHING_VALUE;
        int64_t times = 0;
        if (a.kind == MI_TEXT && b.kind == MI_INT) {
            repeated = a;
            times = b.as.i;
        } else if (b.kind == MI_TEXT && a.kind == MI_INT) {
            repeated = b;
            times = a.as.i;
        } else if (a.kind == MI_LIST && b.kind == MI_INT) {
            repeated = a;
            times = b.as.i;
        } else if (b.kind == MI_LIST && a.kind == MI_INT) {
            repeated = b;
            times = a.as.i;
        }
        if (repeated.kind == MI_TEXT) {
            MiStr *str = mi_str_alloc(repeated.as.text->len * (size_t)(times > 0 ? times : 0));
            size_t pos = 0;
            for (int64_t i = 0; i < times; i++) {
                memcpy(str->data + pos, repeated.as.text->data, repeated.as.text->len);
                pos += repeated.as.text->len;
            }
            return mi_text_take(str);
        }
        if (repeated.kind == MI_LIST) {
            MiList *list = mi_list_alloc(0);
            for (int64_t i = 0; i < times; i++) {
                for (size_t k = 0; k < repeated.as.list->len; k++) {
                    mi_list_push(list, mi_retain(repeated.as.list->items[k]));
                }
            }
            return mi_list_take(list);
        }
        mi_raise("E0300", span, NULL, "cannot multiply %s by %s", mi_kind_name(a),
                 mi_kind_name(b));
    }
    if (strcmp(op, "DIV") == 0 || strcmp(op, "IDIV") == 0 || strcmp(op, "MOD") == 0) {
        if (!((a.kind == MI_INT || a.kind == MI_REAL || a.kind == MI_BIG) &&
              (b.kind == MI_INT || b.kind == MI_REAL || b.kind == MI_BIG))) {
            mi_raise("E0300", span, NULL, "cannot divide %s by %s", mi_kind_name(a),
                     mi_kind_name(b));
        }
        /* 两边都是整数、且任意一边超出 int64 时走大整数除法 */
        if (strcmp(op, "DIV") != 0 &&
            ((a.kind == MI_BIG) || (b.kind == MI_BIG))) {
            MiBig *x = mi_value_to_big(a);
            MiBig *y = mi_value_to_big(b);
            if (mi_big_is_zero(y)) {
                mi_big_free(x);
                mi_big_free(y);
                mi_raise("E0301", span, NULL, "division by zero");
            }
            MiBig *rem = NULL;
            MiBig *quotient = mi_big_divmod(x, y, &rem);
            mi_big_free(x);
            mi_big_free(y);
            if (!quotient) {
                mi_big_free(rem);
                mi_raise("E0300", span, NULL, "cannot divide these numbers");
            }
            if (strcmp(op, "IDIV") == 0) {
                mi_big_free(rem);
                return mi_value_from_big(quotient);
            }
            mi_big_free(quotient);
            return mi_value_from_big(rem);
        }
        double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
        if (y == 0.0) mi_raise("E0301", span, NULL, "division by zero");
        if (strcmp(op, "DIV") == 0) {
            double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
            return mi_real(x / y);
        }
        if (a.kind == MI_INT && b.kind == MI_INT) {
            int64_t x = a.as.i;
            int64_t divisor = b.as.i;
            int64_t quotient = x / divisor;
            int64_t remainder = x % divisor;
            if (remainder != 0 && ((x < 0) != (divisor < 0))) {
                quotient -= 1;
                remainder += divisor;
            }
            if (strcmp(op, "IDIV") == 0) return mi_int(quotient);
            return mi_int(remainder);
        }
        double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
        double quotient = floor(x / y);
        if (strcmp(op, "IDIV") == 0) return mi_real(quotient);
        return mi_real(x - quotient * y);
    }
    if (strcmp(op, "POW") == 0) {
        if (!((a.kind == MI_INT || a.kind == MI_REAL || a.kind == MI_BIG) &&
              (b.kind == MI_INT || b.kind == MI_REAL || b.kind == MI_BIG))) {
            mi_raise("E0300", span, NULL, "cannot raise %s to a power", mi_kind_name(a));
        }
        int a_int = (a.kind == MI_INT || a.kind == MI_BIG);
        int b_int = (b.kind == MI_INT || b.kind == MI_BIG);
        if (a_int && b_int && b.kind == MI_INT && b.as.i >= 0) {
            if (b.as.i < 63) {
                int64_t result = 1;
                int overflow = 0;
                for (int64_t i = 0; i < b.as.i; i++) {
                    if (__builtin_mul_overflow(result, a.as.i, &result)) {
                        overflow = 1;
                        break;
                    }
                }
                if (!overflow) return mi_int(result);
            }
            if (b.as.i <= 100000) {
                MiBig *base = mi_value_to_big(a);
                MiBig *powered = mi_big_pow(base, b.as.i);
                mi_big_free(base);
                if (powered) return mi_value_from_big(powered);
            }
        }
        double x = a.kind == MI_INT ? (double)a.as.i : a.as.r;
        double y = b.kind == MI_INT ? (double)b.as.i : b.as.r;
        return mi_real(pow(x, y));
    }
    if (strcmp(op, "EQ") == 0) return mi_flag(mi_equal(a, b));
    if (strcmp(op, "NE") == 0) return mi_flag(!mi_equal(a, b));
    if (strcmp(op, "LT") == 0) return mi_flag(value_compare(a, b) < 0);
    if (strcmp(op, "LE") == 0) return mi_flag(value_compare(a, b) <= 0);
    if (strcmp(op, "GT") == 0) return mi_flag(value_compare(a, b) > 0);
    if (strcmp(op, "GE") == 0) return mi_flag(value_compare(a, b) >= 0);
    mi_raise("E0300", span, NULL, "unknown operator `%s`", op);
    return MI_NOTHING_VALUE;
}

static MiValue unary_op(const char *op, MiValue value, MiSpan span) {
    if (strcmp(op, "NOT") == 0) return mi_flag(!mi_truthy(value));
    if (strcmp(op, "MINUS") == 0) {
        if (value.kind == MI_INT) return mi_int(-value.as.i);
        if (value.kind == MI_REAL) return mi_real(-value.as.r);
        mi_raise("E0300", span, NULL, "cannot negate %s", mi_kind_name(value));
    }
    mi_raise("E0300", span, NULL, "unknown unary operator `%s`", op);
    return MI_NOTHING_VALUE;
}

MiValue mi_invoke(MiValue callee, MiValue *values, size_t count, MiSpan span);

#include "builtins_impl.h"

/* ================================================================ 调用 */
static MiValue call_station(MiModule *module, MiAst *station, MiBinding *args, size_t count,
                            MiSpan span) {
    MiState *state = g_state;
    state->depth++;
    if (state->depth > state->max_depth) {
        state->depth--;
        mi_raise("E0306", span, "check the recursion in `%s`", station->name,
                 "call depth limit of %d reached", state->max_depth);
    }
    MiFrame frame;
    frame_init(&frame, module, station);

    /* 绑定实参 */
    size_t next_positional = 0;
    for (size_t i = 0; i < count; i++) {
        if (!args[i].name) {
            if (next_positional < station->param_count) {
                MiAst *param = station->params[next_positional];
                env_set(&frame.params, param->name, mi_retain(args[i].value));
                next_positional++;
            } else {
                frame_free(&frame);
                state->depth--;
                mi_raise("E0205", span, NULL, "too many arguments for station `%s`",
                         station->name);
            }
        } else {
            int found = 0;
            for (size_t k = 0; k < station->param_count; k++) {
                if (strcmp(station->params[k]->name, args[i].name) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                frame_free(&frame);
                state->depth--;
                mi_raise("E0204", span, NULL, "unknown argument label `%s`", args[i].name);
            }
            MiValue *slot = env_slot(&frame.params, args[i].name, 1);
            if ((*slot).kind != MI_NOTHING) {
                frame_free(&frame);
                state->depth--;
                mi_raise("E0204", span, NULL, "argument `%s` is given twice", args[i].name);
            }
            mi_release(*slot);
            *slot = mi_retain(args[i].value);
        }
    }
    /* 默认值 / 缺失实参 */
    for (size_t k = 0; k < station->param_count; k++) {
        MiAst *param = station->params[k];
        int found = 0;
        env_get(&frame.params, param->name, &found);
        if (found) continue;
        if (!param->a) {
            frame_free(&frame);
            state->depth--;
            mi_raise("E0205", span, NULL, "missing argument `%s`", param->name);
        }
        env_set(&frame.params, param->name, eval_expr(&frame, param->a));
    }

    /* 执行站点体：`return 值` 等价于往 value 端口写值，再离开 */
    exec_block(&frame, station->b);
    if (g_signal.kind == 3) {
        if (g_signal.value.kind != MI_NOTHING) {
            env_set(&frame.ports, "value", g_signal.value);
            g_signal.value = MI_NOTHING_VALUE;
        } else {
            mi_release(g_signal.value);
            g_signal.value = MI_NOTHING_VALUE;
        }
        g_signal.kind = 0;
    } else if (g_signal.kind == 1) {
        g_signal.kind = 0;
    }

    /* 按端口数收敛 */
    MiValue result = MI_NOTHING_VALUE;
    if (station->port_count == 0) {
        result = MI_NOTHING_VALUE;
    } else if (station->port_count == 1) {
        int found = 0;
        result = mi_retain(env_get(&frame.ports, station->ports[0], &found));
    } else {
        MiAsm *asmv = mi_asm_alloc(station->port_count);
        for (size_t i = 0; i < station->port_count; i++) {
            int found = 0;
            MiValue value = env_get(&frame.ports, station->ports[i], &found);
            if (found) mi_asm_set(asmv, station->ports[i], mi_retain(value));
        }
        result = mi_asm_take(asmv);
    }
    frame_free(&frame);
    state->depth--;
    return result;
}

/* 用值调用（内置 map/filter/fold 需要） */
MiValue mi_invoke(MiValue callee, MiValue *values, size_t count, MiSpan span) {
    MiBinding args[4];
    memset(args, 0, sizeof(args));
    for (size_t i = 0; i < count && i < 4; i++) args[i].value = values[i];
    if (callee.kind == MI_STATION) {
        return call_station(callee.as.station->module, callee.as.station->decl, args, count,
                            span);
    }
    if (callee.kind == MI_BUILTIN) {
        return call_builtin(callee.as.builtin, args, count, span);
    }
    mi_raise("E0303", span, "put a station or a builtin here", "this %s is not callable",
             mi_kind_name(callee));
    return MI_NOTHING_VALUE;
}

static MiValue eval_call(MiFrame *frame, MiAst *call) {
    MiBinding *args = NULL;
    size_t count = call->count;
    if (count) args = mi_calloc(count, sizeof(MiBinding));
    for (size_t i = 0; i < count; i++) {
        MiAst *arg = call->list[i];
        args[i].name = arg->name;
        args[i].value = eval_expr(frame, arg->a);
    }

    MiValue result = MI_NOTHING_VALUE;
    if (!call->name) {
        MiValue callee = eval_expr(frame, call->a);
        result = count ? mi_invoke(callee, &args[0].value, count, call->span)
                       : mi_invoke(callee, NULL, 0, call->span);
        /* mi_invoke 用的是按位置的实参，这里补上标签无关性 */
    } else if (call->flag) {
        int id = bi_lookup_name(call->name);
        result = call_builtin(id, args, count, call->span);
    } else if (strchr(call->name, '.')) {
        char prefix[128];
        const char *dot = strchr(call->name, '.');
        size_t len = (size_t)(dot - call->name);
        if (len >= sizeof(prefix)) len = sizeof(prefix) - 1;
        memcpy(prefix, call->name, len);
        prefix[len] = '\0';
        MiModule *imported = NULL;
        for (size_t i = 0; i < frame->module->import_count; i++) {
            if (strcmp(frame->module->import_names[i], prefix) == 0) {
                imported = frame->module->imports[i];
                break;
            }
        }
        if (imported) {
            MiAst *station = mi_module_find_station(imported, dot + 1);
            if (station) {
                result = call_station(imported, station, args, count, call->span);
                goto done;
            }
        }
        int id = bi_lookup_name(call->name);
        if (id < 0) {
            mi_raise("E0203", call->span, "import the module with `use … as` first",
                     "unknown name `%s`", call->name);
        }
        result = call_builtin(id, args, count, call->span);
    } else {
        int found = 0;
        MiValue stored = env_get(&frame->params, call->name, &found);
        if (!found) stored = env_get(&frame->wires, call->name, &found);
        if (!found) stored = env_get(&frame->variables, call->name, &found);
        if (found) {
            result = count ? mi_invoke(stored, &args[0].value, count, call->span)
                           : mi_invoke(stored, NULL, 0, call->span);
            goto done;
        }
        MiModule *owner = mi_module_find_owning(frame->module, call->name);
        if (owner) {
            result = call_station(owner, mi_module_find_station(owner, call->name), args,
                                  count, call->span);
            goto done;
        }
        int id = bi_lookup_name(call->name);
        if (id < 0) {
            mi_raise("E0203", call->span, NULL, "unknown station or builtin `%s`",
                     call->name);
        }
        result = call_builtin(id, args, count, call->span);
    }
done:
    for (size_t i = 0; i < count; i++) mi_release(args[i].value);
    free(args);
    return result;
}

/* ================================================================ 表达式 */
static MiValue eval_name(MiFrame *frame, MiAst *expr) {
    if (expr->flag) {
        int id = bi_lookup_name(expr->name);
        if (id < 0) mi_raise("E0203", expr->span, NULL, "unknown builtin `%s`", expr->name);
        return mi_builtin(id);
    }
    int found = 0;
    MiValue value = env_get(&frame->params, expr->name, &found);
    if (found) return mi_retain(value);
    value = env_get(&frame->wires, expr->name, &found);
    if (found) return mi_retain(value);
    value = env_get(&frame->variables, expr->name, &found);
    if (found) return mi_retain(value);
    MiModule *owner = mi_module_find_owning(frame->module, expr->name);
    if (owner) {
        MiStationRef *ref = mi_alloc(sizeof(MiStationRef));
        ref->module = owner;
        ref->decl = mi_module_find_station(owner, expr->name);
        ref->refs = 1;
        return mi_station_ref(ref);
    }
    int id = bi_lookup_name(expr->name);
    if (id >= 0) return mi_builtin(id);
    mi_raise("E0203", expr->span, NULL, "unknown name `%s`", expr->name);
    return MI_NOTHING_VALUE;
}

static MiValue eval_expr(MiFrame *frame, MiAst *expr) {
    if (!expr) return MI_NOTHING_VALUE;
    tick(expr->span);
    switch (expr->kind) {
        case AST_NUMBER:
        case AST_TEXT:
        case AST_FLAG:
            return mi_retain(expr->literal);
        case AST_NIL:
            return MI_NOTHING_VALUE;
        case AST_LIST: {
            MiList *list = mi_list_alloc(expr->count);
            for (size_t i = 0; i < expr->count; i++) {
                mi_list_push(list, eval_expr(frame, expr->list[i]));
            }
            return mi_list_take(list);
        }
        case AST_ASSEMBLY: {
            MiAsm *asmv = mi_asm_alloc(expr->count);
            for (size_t i = 0; i < expr->count; i++) {
                MiAst *field = expr->list[i];
                mi_asm_set(asmv, field->name, eval_expr(frame, field->a));
            }
            return mi_asm_take(asmv);
        }
        case AST_NAME:
            return eval_name(frame, expr);
        case AST_READ: {
            int found = 0;
            MiValue value = env_get(&frame->params, expr->name, &found);
            if (found) return mi_retain(value);
            value = env_get(&frame->wires, expr->name, &found);
            if (found) return mi_retain(value);
            value = env_get(&frame->variables, expr->name, &found);
            if (found) return mi_retain(value);
            mi_raise("E0200", expr->span, "declare it with `wire`",
                     "`draw` needs a declared wire, but `%s` is not one", expr->name);
            return MI_NOTHING_VALUE;
        }
        case AST_FIELD: {
            MiValue base = eval_expr(frame, expr->a);
            if (base.kind == MI_ASSEMBLY) {
                int index = mi_asm_find(base.as.asmv, expr->name);
                if (index < 0) {
                    mi_value_release(base);
                    mi_raise("E0302", expr->span, NULL,
                             "this assembly has no field `%s`", expr->name);
                }
                MiValue out = mi_retain(base.as.asmv->values[index]);
                mi_value_release(base);
                return out;
            }
            mi_raise("E0300", expr->span, NULL, "cannot read field `%s` of %s", expr->name,
                     mi_kind_name(base));
            return MI_NOTHING_VALUE;
        }
        case AST_UNARY: {
            MiValue operand = eval_expr(frame, expr->a);
            MiValue out = unary_op(expr->name, operand, expr->span);
            mi_value_release(operand);
            return out;
        }
        case AST_BINARY: {
            if (strcmp(expr->name, "AND") == 0) {
                MiValue left = eval_expr(frame, expr->a);
                if (!mi_truthy(left)) return left;
                mi_value_release(left);
                return eval_expr(frame, expr->b);
            }
            if (strcmp(expr->name, "OR") == 0) {
                MiValue left = eval_expr(frame, expr->a);
                if (mi_truthy(left)) return left;
                mi_value_release(left);
                return eval_expr(frame, expr->b);
            }
            MiValue left = eval_expr(frame, expr->a);
            MiValue right = eval_expr(frame, expr->b);
            MiValue out = binary_op(expr->name, left, right, expr->span);
            mi_value_release(left);
            mi_value_release(right);
            return out;
        }
        case AST_CALL:
            return eval_call(frame, expr);
        default:
            mi_raise("E0300", expr->span, NULL, "cannot evaluate this expression");
            return MI_NOTHING_VALUE;
    }
}

/* ================================================================ 流程 */
static void exec_block(MiFrame *frame, MiAst *block) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        exec_flow(frame, block->list[i]);
        if (g_signal.kind) return;
    }
}

static int as_int_value(MiValue value, int64_t *out, MiSpan span) {
    if (mi_as_int(value, out)) return 1;
    mi_raise("E0300", span, NULL, "the count must be a whole number, not %s",
             mi_kind_name(value));
    return 0;
}

static void exec_cycle(MiFrame *frame, MiAst *flow) {
    const char *kind = flow->name2;
    if (strcmp(kind, "count") == 0) {
        MiValue raw = flow->a ? eval_expr(frame, flow->a) : mi_int(0);
        int64_t count = 0;
        if (!mi_as_int(raw, &count)) {
            mi_value_release(raw);
            mi_raise("E0300", flow->span, NULL, "the cycle count must be a whole number");
        }
        mi_value_release(raw);
        if (count < 0) mi_raise("E0300", flow->span, NULL, "the cycle count cannot be negative");
        for (int64_t i = 0; i < count; i++) {
            if (flow->name) env_set(&frame->variables, flow->name, mi_int(i));
            tick(flow->span);
            exec_block(frame, flow->b);
            if (g_signal.kind == 2) {
                g_signal.kind = 0;
                continue;
            }
            if (g_signal.kind == 1) {
                g_signal.kind = 0;
                return;
            }
            if (g_signal.kind == 3) return;
        }
        return;
    }
    if (strcmp(kind, "while") == 0 || strcmp(kind, "until") == 0) {
        int64_t index = 0;
        for (;;) {
            MiValue condition = eval_expr(frame, flow->a);
            int truth = mi_truthy(condition);
            mi_value_release(condition);
            if (strcmp(kind, "while") == 0 && !truth) break;
            if (strcmp(kind, "until") == 0 && truth) break;
            if (flow->name) env_set(&frame->variables, flow->name, mi_int(index));
            tick(flow->span);
            exec_block(frame, flow->b);
            if (g_signal.kind == 2) {
                g_signal.kind = 0;
                index++;
                continue;
            }
            if (g_signal.kind == 1) {
                g_signal.kind = 0;
                return;
            }
            if (g_signal.kind == 3) return;
            index++;
        }
        return;
    }
    /* over */
    {
        MiValue items = eval_expr(frame, flow->a);
        if (items.kind != MI_LIST) {
            MiValue placeholder = items;
            mi_raise("E0300", flow->span, NULL, "`cycle over`'s list must be a list, not %s",
                     mi_kind_name(placeholder));
        }
        MiList *list = items.as.list;
        for (size_t i = 0; i < list->len; i++) {
            if (flow->name) env_set(&frame->variables, flow->name, mi_retain(list->items[i]));
            tick(flow->span);
            exec_block(frame, flow->b);
            if (g_signal.kind == 2) {
                g_signal.kind = 0;
                continue;
            }
            if (g_signal.kind == 1) {
                g_signal.kind = 0;
                break;
            }
            if (g_signal.kind == 3) break;
        }
        mi_value_release(items);
    }
}

static void exec_flow(MiFrame *frame, MiAst *flow) {
    tick(flow->span);
    switch (flow->kind) {
        case AST_WIRE:
            env_set(&frame->wires, flow->name,
                    flow->a ? eval_expr(frame, flow->a) : MI_NOTHING_VALUE);
            return;
        case AST_EMIT: {
            int found = 0;
            env_get(&frame->params, flow->name, &found);
            if (found) {
                mi_raise("E0202", flow->span, NULL, "`%s` is a read-only port", flow->name);
            }
            env_get(&frame->wires, flow->name, &found);
            if (!found) {
                mi_raise("E0200", flow->span, NULL,
                         "wire `%s` is used before it is declared", flow->name);
            }
            env_set(&frame->wires, flow->name, eval_expr(frame, flow->a));
            return;
        }
        case AST_WHEN: {
            MiValue condition = eval_expr(frame, flow->a);
            int truth = mi_truthy(condition);
            mi_value_release(condition);
            if (truth) {
                exec_block(frame, flow->b);
            } else if (flow->c) {
                if (flow->c->kind == AST_BLOCK) {
                    exec_block(frame, flow->c);
                } else {
                    exec_flow(frame, flow->c);
                }
            }
            return;
        }
        case AST_CYCLE:
            exec_cycle(frame, flow);
            return;
        case AST_FOR: {
            MiValue items = eval_expr(frame, flow->a);
            if (items.kind != MI_LIST) {
                mi_raise("E0300", flow->span, NULL, "`for`'s list must be a list, not %s",
                         mi_kind_name(items));
            }
            MiList *list = items.as.list;
            for (size_t i = 0; i < list->len; i++) {
                env_set(&frame->variables, flow->name, mi_retain(list->items[i]));
                tick(flow->span);
                exec_block(frame, flow->b);
                if (g_signal.kind == 2) {
                    g_signal.kind = 0;
                    continue;
                }
                if (g_signal.kind == 1) {
                    g_signal.kind = 0;
                    break;
                }
                if (g_signal.kind == 3) break;
            }
            mi_value_release(items);
            return;
        }
        case AST_OUT:
            env_set(&frame->ports, flow->name, eval_expr(frame, flow->a));
            return;
        case AST_RETURN:
            g_signal.kind = 3;
            g_signal.value = flow->a ? eval_expr(frame, flow->a) : MI_NOTHING_VALUE;
            return;
        case AST_HALT:
            g_signal.kind = 1;
            return;
        case AST_SKIP:
            g_signal.kind = 2;
            return;
        case AST_FLOW:
            mi_value_release(eval_expr(frame, flow->a));
            return;
        default:
            return;
    }
}

/* ================================================================ 入口 */
MiRunResult mi_execute(MiAst *program, const char *filename, const char *origin,
                       int max_steps, int max_depth) {
    MiRunResult result;
    memset(&result, 0, sizeof(result));

    MiLoader *loader = mi_loader_new();
    MiState state;
    memset(&state, 0, sizeof(state));
    state.loader = loader;
    state.output = &result.output;
    state.max_steps = max_steps;
    state.max_depth = max_depth;
    g_state = &state;
    g_signal.kind = 0;

    MiModule *module = mi_module_new(filename);
    for (size_t i = 0; i < program->count; i++) {
        MiAst *item = program->list[i];
        if (item->kind == AST_STATION) mi_module_add_station(module, item->name, item);
    }
    /* 处理 use */
    for (size_t i = 0; i < program->count; i++) {
        MiAst *item = program->list[i];
        if (item->kind != AST_USE) continue;
        MiModule *child = mi_loader_load(loader, item->name, origin ? origin : filename);
        if (!child) {
            result.failed = 1;
            snprintf(result.error.code, sizeof(result.error.code), "E0307");
            snprintf(result.error.message, sizeof(result.error.message), "%s",
                     loader->error ? loader->error : "cannot load module");
            result.error.span = item->span;
            result.error.has_span = 1;
            goto cleanup;
        }
        if (item->name2) {
            module->imports = mi_realloc(module->imports,
                                         (module->import_count + 1) * sizeof(MiModule *));
            module->import_names = mi_realloc(
                module->import_names, (module->import_count + 1) * sizeof(char *));
            module->imports[module->import_count] = child;
            module->import_names[module->import_count] = mi_strdup(item->name2);
            module->import_count++;
        } else {
            module->flat = mi_realloc(module->flat,
                                      (module->flat_count + 1) * sizeof(MiModule *));
            module->flat[module->flat_count++] = child;
        }
    }

    if (setjmp(state.unwind) == 0) {
        MiFrame frame;
        frame_init(&frame, module, NULL);
        for (size_t i = 0; i < program->count; i++) {
            exec_flow(&frame, program->list[i]);
            if (g_signal.kind == 1 || g_signal.kind == 2 || g_signal.kind == 3) {
                g_signal.kind = 0;
            }
        }
        frame_free(&frame);
    } else {
        result.failed = 1;
        if (!result.error.has_span) result.error.has_span = 0;
        snprintf(result.error.code, sizeof(result.error.code), "%s", mi_err.code);
        snprintf(result.error.message, sizeof(result.error.message), "%s", mi_err.message);
        snprintf(result.error.hint, sizeof(result.error.hint), "%s", mi_err.hint);
        result.error.span = mi_err.span;
        result.error.has_span = mi_err.has_span;
    }

cleanup:
    mi_release(g_signal.value);
    g_signal.kind = 0;
    g_state = NULL;
    /* module 归 loader 管，loader 负责释放全部模块 */
    mi_loader_free(loader);
    return result;
}

void mi_run_result_free(MiRunResult *result) {
    for (size_t i = 0; i < result->output.count; i++) free(result->output.lines[i]);
    free(result->output.lines);
    result->output.lines = NULL;
    result->output.count = result->output.cap = 0;
}
