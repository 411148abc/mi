/* mi.h — MI 语言 C 实现的公共头文件。
 *
 * 设计要点：
 *   - 值不可变；文本/列表/装配体用引用计数管理。
 *   - 数字分整数（int64）与实数（double），与 Python 版对齐：`kind()` 都是
 *     number；整除结果若是整数值则退回整数；显示整数时不留小数点。
 *   - 诊断分两类：编译期诊断（可收集多条）与运行期错误（长跳转抛）。
 */
#ifndef MI_H
#define MI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bigint.h"

/* ================================================================== 值 */
typedef enum {
    MI_NOTHING = 0,
    MI_FLAG,
    MI_INT,
    MI_REAL,
    MI_TEXT,
    MI_LIST,
    MI_ASSEMBLY,
    MI_STATION,
    MI_BUILTIN,
    MI_BIG
} MiKind;

typedef struct MiValue MiValue;
struct MiAst;
struct MiModule;

typedef struct MiStr {
    char *data;
    size_t len;      /* 字节长度 */
    long chars;      /* Unicode 码点个数；-1 表示未算 */
    int refs;
} MiStr;

typedef struct MiList {
    MiValue *items;
    size_t len;
    int refs;
} MiList;

typedef struct MiAsm {
    char **names;
    MiValue *values;
    size_t len;
    int refs;
} MiAsm;

struct MiValue {
    MiKind kind;
    union {
        int flag;
        int64_t i;
        double r;
        MiStr *text;
        MiList *list;
        MiAsm *asmv;
        struct MiStationRef *station;
        int builtin;
        MiBig *big;
    } as;
};

/* 站点值：指向某个模块里的 Station 声明 */
typedef struct MiStationRef {
    struct MiModule *module;
    struct MiAst *decl;
    int refs;
} MiStationRef;

/* ============================================================ 位置/诊断 */
typedef struct {
    int line;
    int col;
    int offset;
} MiPos;

typedef struct {
    MiPos start;
    MiPos end;
} MiSpan;

typedef struct MiDiag {
    char code[8];
    char *message;
    char *hint;
    MiSpan span;
    int is_warning;
    struct MiDiag *next;
} MiDiag;

typedef struct {
    MiDiag *head;
    MiDiag *tail;
    int errors;
    int warnings;
} MiDiagBag;

void mi_diag_init(MiDiagBag *bag);
void mi_diag_add(MiDiagBag *bag, const char *code, MiSpan span, const char *hint,
                 int is_warning, const char *fmt, ...);
void mi_diag_free(MiDiagBag *bag);

/* ================================================================ token */
typedef enum {
    TOK_EOF = 0,
    TOK_SEP,
    TOK_PUNCT,
    TOK_KW,
    TOK_OP,
    TOK_LITERAL,
    TOK_GLYPH,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_TEXT,
    TOK_FLAG,
    TOK_NIL
} MiTokenKind;

typedef struct {
    MiTokenKind kind;
    int id;       /* KW / OP / LITERAL / GLYPH 枚举值 */
    char *text;   /* 源码原文（NUL 结尾 UTF-8） */
    char *sval;   /* TEXT/IDENT/路径的值；KW 存原文 */
    MiValue num;
    MiSpan span;
} MiTok;

typedef struct {
    MiTok *items;
    size_t len;
    size_t cap;
} MiTokList;

/* ================================================================== AST */
typedef enum {
    /* 条目 */
    AST_PROGRAM = 0, AST_USE, AST_WIRE, AST_STATION, AST_PARAM, AST_BLOCK,
    /* 流程 */
    AST_FLOW, AST_EMIT, AST_WHEN, AST_CYCLE, AST_FOR, AST_OUT, AST_RETURN,
    AST_HALT, AST_SKIP,
    /* 表达式 */
    AST_NUMBER, AST_TEXT, AST_FLAG, AST_NIL, AST_LIST, AST_FIELDINIT,
    AST_ASSEMBLY, AST_NAME, AST_READ, AST_FIELD, AST_ARG, AST_CALL,
    AST_BINARY, AST_UNARY
} MiAstKind;

typedef struct MiAst {
    MiAstKind kind;
    MiSpan span;

    char *name;       /* 名字 / 标签 / 字段 / 路径 / 算子 / 端口 */
    char *name2;      /* 备用（cycle 的 ckind） */
    MiValue literal;  /* 字面量值 */
    int flag;         /* FLAG 值、glyph 标记、keyword_typo 标记 */
    int flag2;        /* 预留 */

    struct MiAst *a;  /* 通用子节点 */
    struct MiAst *b;
    struct MiAst *c;

    struct MiAst **list;  /* 子节点数组 */
    size_t count;

    struct MiAst **params; /* Station 参数 */
    size_t param_count;

    char **ports;          /* Station 输出端口名 */
    size_t port_count;
} MiAst;

MiAst *mi_ast_new(MiAstKind kind, MiSpan span);
void mi_ast_free(MiAst *node);

/* ================================================================ 模块 */
typedef struct MiModuleStation {
    char *name;
    MiAst *decl;
} MiModuleStation;

typedef struct MiModule {
    char *path;
    MiAst *program;
    MiModuleStation *stations;
    size_t station_count;
    struct MiModule **imports;  /* 与 import_names 对应 */
    char **import_names;
    size_t import_count;
    struct MiModule **flat;
    size_t flat_count;
} MiModule;

/* ============================================================ 内存/工具 */
void *mi_alloc(size_t bytes);
void *mi_calloc(size_t count, size_t size);
void *mi_realloc(void *ptr, size_t bytes);
char *mi_strdup(const char *text);
char *mi_strndup(const char *text, size_t len);
char *mi_asprintf(const char *fmt, ...);
char *mi_read_source(const char *path);
FILE *mi_fopen_utf8(const char *path, const char *mode);

size_t mi_utf8_len(const char *text);
size_t mi_utf8_offset(const char *text, size_t index);
int mi_utf8_next(const char *text, size_t len, size_t *consumed);
size_t mi_utf8_encode(unsigned code, char out[4]);

/* 编辑距离建议（近似 difflib.get_close_matches） */
const char *mi_suggest(const char *word, const char *const *candidates, int count,
                       double cutoff);

/* ================================================================ 值 API */
extern MiValue MI_NOTHING_VALUE;

MiValue mi_flag(int value);
MiValue mi_int(int64_t value);
MiValue mi_real(double value);
MiValue mi_text_take(MiStr *text);
MiValue mi_text_new(const char *text);
MiValue mi_text_n(const char *text, size_t len);
MiValue mi_list_take(MiList *list);
MiValue mi_list_new(void);
MiValue mi_asm_take(MiAsm *asmv);
MiValue mi_asm_new(void);
MiValue mi_builtin(int id);
MiValue mi_value_take_big(MiBig *value);
MiValue mi_station_ref(MiStationRef *ref);

MiValue mi_retain(MiValue value);
void mi_release(MiValue value);

MiStr *mi_str_alloc(size_t len);
MiList *mi_list_alloc(size_t cap);
MiAsm *mi_asm_alloc(size_t cap);
void mi_list_push(MiList *list, MiValue value);
int mi_asm_find(const MiAsm *asmv, const char *name);
MiValue mi_asm_get(const MiAsm *asmv, const char *name);
void mi_asm_set(MiAsm *asmv, const char *name, MiValue value);
MiAsm *mi_asm_copy(const MiAsm *asmv);

const char *mi_kind_name(MiValue value);
int mi_truthy(MiValue value);
int mi_equal(MiValue a, MiValue b);
int mi_compare(MiValue a, MiValue b);  /* -1/0/1，不可比时置错误 */
char *mi_display(MiValue value, int nested);
char *mi_number_text(MiValue value);

/* 整数提升：内部用，返回调用方拥有的 MiBig（非整数返回 NULL） */
MiBig *mi_value_to_big(MiValue value);
MiValue mi_value_from_big(MiBig *value);

/* 数值读取 */
int mi_as_int(MiValue value, int64_t *out);
int mi_as_float(MiValue value, double *out);
MiValue mi_number_from_double(double value); /* 整数值退回整数 */

/* ============================================================ 运行时错误 */
typedef struct {
    char code[8];
    char message[512];
    char hint[256];
    MiSpan span;
    int has_span;
    int raised;
} MiError;

extern MiError mi_err;

void mi_raise(const char *code, MiSpan span, const char *hint, const char *fmt, ...);

/* ============================================================ 词法/语法 */
int mi_lex(const char *source, const char *filename, MiTokList *out, MiDiagBag *bag);
void mi_toklist_free(MiTokList *tokens);
MiAst *mi_parse(const char *source, const char *filename, MiDiagBag *bag);
MiAst *mi_parse_tokens(MiTokList *tokens, const char *filename, MiDiagBag *bag);

/* 检查 */
void mi_check(MiAst *program, const char *filename, MiDiagBag *bag);

/* 求值 */
typedef struct {
    char **lines;
    size_t count;
    size_t cap;
} MiOutput;

typedef struct {
    MiOutput output;
    int failed;        /* 运行期错误 */
    MiError error;
} MiRunResult;

MiRunResult mi_execute(MiAst *program, const char *filename, const char *origin,
                       int max_steps, int max_depth);
void mi_run_result_free(MiRunResult *result);
/* 打开后 show 的输出立即写 stdout（服务类程序需要） */
void mi_set_stream_output(int enabled);

/* 站点查找 */
MiModule *mi_module_new(const char *path);
void mi_module_free(MiModule *module);
MiAst *mi_module_find_station(MiModule *module, const char *name);
MiModule *mi_module_find_owning(MiModule *module, const char *name);
void mi_module_add_station(MiModule *module, const char *name, MiAst *decl);

/* 载入模块（相对 origin 解析路径） */
typedef struct MiLoader MiLoader;
MiLoader *mi_loader_new(void);
void mi_loader_free(MiLoader *loader);
MiModule *mi_loader_load(MiLoader *loader, const char *path, const char *origin);

/* 诊断渲染 */
void mi_render_diag(FILE *fh, const MiDiag *diag, const char *source,
                    const char *filename);
void mi_render_runtime_error(FILE *fh, const MiError *err, const char *source,
                             const char *filename);

#endif /* MI_H */
