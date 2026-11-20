/* main.c — mi 命令行入口（C 实现）
 *
 *   mi run FILE.mi [--max-steps N] [--max-depth N]
 *   mi check FILE.mi
 *   mi glyphs [--json]
 *   mi version
 *   mi ast FILE.mi
 */
#include "mi.h"
#include "tables.h"

#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif
#include <string.h>

#define MI_VERSION "1.0.0"

static const char *g_source = "";
static const char *g_filename = "<source>";

static void print_line(const char *source, int line) {
    int current = 1;
    const char *cursor = source;
    while (*cursor) {
        if (current == line) break;
        if (*cursor == '\n') current++;
        cursor++;
    }
    if (current != line) return;
    const char *end = cursor;
    while (*end && *end != '\n') end++;
    const char *gutter = " | ";
    (void)gutter;
    fprintf(stderr, "  %d | %.*s\n", line, (int)(end - cursor), cursor);
}

void mi_render_diag(FILE *fh, const MiDiag *diag, const char *source,
                    const char *filename) {
    fprintf(fh, "%s[%s] %s\n", diag->is_warning ? "warning" : "error", diag->code,
            diag->message);
    int line = diag->span.start.line;
    int col = diag->span.start.col;
    int stop = diag->span.end.col;
    fprintf(fh, " --> %s:%d:%d\n", filename, line, col);
    print_line(source, line);
    if (line > 0) {
        fprintf(fh, "    ");
        for (int i = 1; i < col; i++) fputc(' ', fh);
        int carets = stop > col ? stop - col : 1;
        for (int i = 0; i < carets; i++) fputc('^', fh);
        fputc('\n', fh);
    }
    if (diag->hint) fprintf(fh, "    = hint: %s\n", diag->hint);
    fprintf(fh, "\n");
}

void mi_render_runtime_error(FILE *fh, const MiError *err, const char *source,
                             const char *filename) {
    fprintf(fh, "runtime error[%s] %s\n", err->code, err->message);
    if (err->has_span) {
        fprintf(fh, " --> %s:%d:%d\n", filename, err->span.start.line,
                err->span.start.col);
        print_line(source, err->span.start.line);
    }
    if (err->hint[0]) fprintf(fh, " hint: %s\n", err->hint);
}


/* ------------------------------------------------ 自举运行时（MI 写解释器） */
/* 找 米/自举/eval.mi：先看 MI_HOME，再看 exe 同级/上级，最后看 cwd。 */
static char *selfhost_entry(void) {
    const char *suffix = "米/自举/eval.mi";
    const char *home = getenv("MI_HOME");
    if (home && *home) {
        char *candidate = mi_asprintf("%s/%s", home, suffix);
        char *source = mi_read_source(candidate);
        free(candidate);
        if (source) {
            free(source);
            return mi_asprintf("%s/%s", home, suffix);
        }
    }
#ifdef _WIN32
    {
        wchar_t wide[2048];
        DWORD got = GetModuleFileNameW(NULL, wide, 2048);
        if (got > 0 && got < 2048) {
            int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
            if (need > 0) {
                char *exe = mi_alloc((size_t)need);
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, exe, need, NULL, NULL);
                for (int level = 0; level < 3; level++) {
                    char *slash = strrchr(exe, '/');
                    if (!slash) slash = strrchr(exe, '\\');
                    if (!slash) break;
                    *slash = '\0';
                    char *candidate = mi_asprintf("%s/%s", exe, suffix);
                    char *source = mi_read_source(candidate);
                    free(candidate);
                    if (source) {
                        free(source);
                        return mi_asprintf("%s/%s", exe, suffix);
                    }
                }
                free(exe);
            }
        }
    }
#endif
    {
        char *source = mi_read_source(suffix);
        if (source) {
            free(source);
            return mi_strdup(suffix);
        }
    }
    return NULL;
}

/* 把文本转成 MI 文本字面量（含转义） */
static char *mi_quote(const char *text) {
    size_t len = strlen(text);
    char *out = mi_alloc(len * 4 + 8);
    size_t pos = 0;
    out[pos++] = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
        else if (ch == '"') { out[pos++] = '\\'; out[pos++] = '"'; }
        else if (ch == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (ch == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else if (ch == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else out[pos++] = (char)ch;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return out;
}

static int cmd_run_selfhost(const char *path, int max_steps, int max_depth) {
    char *source = mi_read_source(path);
    if (!source) {
        fprintf(stderr, "error: no such file: %s\n", path);
        return 3;
    }
    char *entry = selfhost_entry();
    if (!entry) {
        fprintf(stderr,
                "error: 找不到自举解释器（米/自举/eval.mi）。\n"
                "       设置 MI_HOME 指向仓库根目录，或在仓库根目录下运行。\n");
        free(source);
        return 3;
    }
    char *quoted_source = mi_quote(source);
    char *quoted_origin = mi_quote(path);
    char *quoted_entry = mi_quote(entry);
    char *driver = mi_asprintf(
        "use %s as selfhost\n"
        "~__result: selfhost.run_source(%s, %s)\n"
        "cycle over __result.log as __line { show(__line) }\n"
        "cycle over __result.errors as __e { show(\"!!error[\" + __e.code + \"] \" + __e.message) }\n",
        quoted_entry, quoted_source, quoted_origin);
    free(quoted_entry);
    free(quoted_source);
    free(quoted_origin);
    free(entry);

    MiDiagBag bag;
    mi_diag_init(&bag);
    MiAst *program = mi_parse(driver, path, &bag);
    if (bag.errors > 0) {
        for (MiDiag *diag = bag.head; diag; diag = diag->next) {
            mi_render_diag(stderr, diag, driver, path);
        }
        mi_diag_free(&bag);
        mi_ast_free(program);
        free(driver);
        free(source);
        return 1;
    }
    mi_diag_free(&bag);

    MiRunResult result = mi_execute(program, path, path, max_steps, max_depth);
    int failures = 0;
    for (size_t i = 0; i < result.output.count; i++) {
        const char *line = result.output.lines[i];
        if (strncmp(line, "!!error[", 8) == 0) {
            fprintf(stderr, "%s\n", line + 2);
            failures++;
        } else {
            printf("%s\n", line);
        }
    }
    int code = 0;
    if (result.failed) {
        mi_render_runtime_error(stderr, &result.error, driver, path);
        code = 2;
    } else if (failures) {
        code = 1;
    }
    mi_run_result_free(&result);
    mi_ast_free(program);
    free(driver);
    free(source);
    return code;
}


static void print_usage(FILE *fh);

/* ------------------------------------------------------------------ REPL */
/* 判断输入是不是还没写完：括号/花括号不配对，或者最后一行以开启符结尾。 */
static int looks_incomplete(const char *text) {
    int braces = 0;
    int parens = 0;
    int brackets = 0;
    int in_text = 0;
    char quote = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        char ch = *cursor;
        if (in_text) {
            if (ch == '\\') {
                cursor++;
                continue;
            }
            if (ch == quote) in_text = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            in_text = 1;
            quote = ch;
            continue;
        }
        if (ch == '#') {
            while (*cursor && *cursor != '\n') cursor++;
            if (!*cursor) break;
            continue;
        }
        if (ch == '-' && cursor[1] == '-') {
            while (*cursor && *cursor != '\n') cursor++;
            if (!*cursor) break;
            continue;
        }
        if (ch == '{') braces++;
        else if (ch == '}') braces--;
        else if (ch == '(') parens++;
        else if (ch == ')') parens--;
        else if (ch == '[') brackets++;
        else if (ch == ']') brackets--;
    }
    if (in_text) return 1;
    if (braces > 0 || parens > 0 || brackets > 0) return 1;
    /* 末尾是 do 块开启符 */
    const char *tail = text + strlen(text);
    while (tail > text && (tail[-1] == '\n' || tail[-1] == ' ' || tail[-1] == '\t')) tail--;
    if (tail > text + 3) {
        if (strncmp(tail - 2, "do", 2) == 0 || strncmp(tail - 5, "begin", 5) == 0) {
            int has_end = 0;
            for (const char *cursor = text; *cursor; cursor++) {
                if (strncmp(cursor, "end", 3) == 0) has_end = 1;
            }
            if (!has_end) return 1;
        }
    }
    return 0;
}

static int cmd_repl(void) {
    printf("MI %s — Signals, not statements.\n", MI_VERSION);
    printf("输入 MI 代码回车执行；`:quit` 退出；`:help` 看用法。\n");

    char *session = mi_strdup("");
    size_t session_len = 0;
    size_t printed = 0;
    int pending = 0;

    for (;;) {
        fputs(pending ? "... " : "mi> ", stdout);
        fflush(stdout);

        char line[4096];
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        size_t line_len = strlen(line);
        while (line_len && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }

        if (!pending) {
            if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0 ||
                strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
                break;
            }
            if (strcmp(line, ":help") == 0 || strcmp(line, ":h") == 0) {
                print_usage(stdout);
                printf("\n`%s glyphs` 看全部拼写。\n",
                       "mi");
                continue;
            }
        }
        if (line_len == 0 && !pending) continue;

        /* 拼上新的输入 */
        size_t need = session_len + line_len + 2;
        char *candidate = mi_alloc(need);
        memcpy(candidate, session, session_len);
        memcpy(candidate + session_len, line, line_len);
        candidate[session_len + line_len] = '\n';
        candidate[session_len + line_len + 1] = '\0';

        MiDiagBag bag;
        mi_diag_init(&bag);
        MiAst *program = mi_parse(candidate, "<repl>", &bag);

        if (bag.errors > 0) {
            if (looks_incomplete(candidate)) {
                /* 还没写完，继续累积 */
                mi_diag_free(&bag);
                mi_ast_free(program);
                free(session);
                session = candidate;
                session_len += line_len + 1;
                pending = 1;
                continue;
            }
            for (MiDiag *diag = bag.head; diag; diag = diag->next) {
                if (diag->is_warning) continue;
                mi_render_diag(stderr, diag, candidate, "<repl>");
            }
            mi_diag_free(&bag);
            mi_ast_free(program);
            free(candidate);
            continue;
        }
        mi_diag_free(&bag);

        MiRunResult result = mi_execute(program, "<repl>", "<repl>", 5000000, 400);
        for (size_t i = printed; i < result.output.count; i++) {
            printf("%s\n", result.output.lines[i]);
        }
        printed = result.output.count;
        if (result.failed) {
            mi_render_runtime_error(stderr, &result.error, candidate, "<repl>");
        }
        mi_run_result_free(&result);
        mi_ast_free(program);

        free(session);
        session = candidate;
        session_len += line_len + 1;
        pending = 0;
    }

    free(session);
    return 0;
}

/* ------------------------------------------------------------- 子命令 */
static int cmd_run(const char *path, int max_steps, int max_depth) {
    char *source = mi_read_source(path);
    if (!source) {
        fprintf(stderr, "error: no such file: %s\n", path);
        return 3;
    }
    g_source = source;
    g_filename = path;

    MiDiagBag bag;
    mi_diag_init(&bag);
    MiAst *program = mi_parse(source, path, &bag);
    mi_check(program, path, &bag);
    if (bag.errors > 0) {
        for (MiDiag *diag = bag.head; diag; diag = diag->next) {
            if (diag->is_warning) continue;
            mi_render_diag(stderr, diag, source, path);
        }
        mi_diag_free(&bag);
        mi_ast_free(program);
        free(source);
        return 1;
    }
    mi_diag_free(&bag);

    /* 流式输出：程序可能长时间不返回（比如 serve），输出要立刻可见 */
    mi_set_stream_output(1);
    MiRunResult result = mi_execute(program, path, path, max_steps, max_depth);
    mi_set_stream_output(0);
    int code = 0;
    if (result.failed) {
        mi_render_runtime_error(stderr, &result.error, source, path);
        code = 2;
    }
    mi_run_result_free(&result);
    mi_ast_free(program);
    free(source);
    return code;
}

static int cmd_check(const char *path) {
    char *source = mi_read_source(path);
    if (!source) {
        fprintf(stderr, "error: no such file: %s\n", path);
        return 3;
    }
    MiDiagBag bag;
    mi_diag_init(&bag);
    MiAst *program = mi_parse(source, path, &bag);
    mi_check(program, path, &bag);
    for (MiDiag *diag = bag.head; diag; diag = diag->next) {
        mi_render_diag(stderr, diag, source, path);
    }
    int code = bag.errors > 0 ? 1 : 0;
    mi_diag_free(&bag);
    mi_ast_free(program);
    free(source);
    return code;
}

/* 按 id 收集一张拼写表：返回 "a | b | c" 形式 */
typedef struct {
    char *buffer;
    size_t len;
    size_t cap;
} MiTextJoiner;

static void join_add(MiTextJoiner *joiner, const char *text) {
    size_t need = strlen(text) + 4;
    if (joiner->len + need > joiner->cap) {
        size_t cap = joiner->cap ? joiner->cap : 64;
        while (cap < joiner->len + need) cap *= 2;
        joiner->buffer = mi_realloc(joiner->buffer, cap);
        joiner->cap = cap;
    }
    if (joiner->len) {
        memcpy(joiner->buffer + joiner->len, " | ", 3);
        joiner->len += 3;
    }
    size_t size = strlen(text);
    memcpy(joiner->buffer + joiner->len, text, size);
    joiner->len += size;
    joiner->buffer[joiner->len] = '\0';
}

static int cmd_glyphs(void) {
    printf("MI %s — Signals, not statements.\n", MI_VERSION);

    printf("keywords (%d groups):\n", KW_COUNT);
    for (int i = 0; i < KW_COUNT; i++) {
        MiTextJoiner joiner = {NULL, 0, 0};
        join_add(&joiner, "");
        joiner.len = 0;
        if (joiner.buffer) joiner.buffer[0] = '\0';
        for (size_t k = 0; k < MI_SPELLING_COUNT; k++) {
            if (MI_SPELLINGS[k].kind == 'K' && MI_SPELLINGS[k].id == i) {
                join_add(&joiner, MI_SPELLINGS[k].spelling);
            }
        }
        printf("  %-12s %s\n", MI_KEYWORD_NAMES[i], joiner.buffer ? joiner.buffer : "");
        free(joiner.buffer);
    }

    printf("operators (%d):\n", OPK_COUNT);
    for (int i = 0; i < OPK_COUNT; i++) {
        MiTextJoiner joiner = {NULL, 0, 0};
        for (size_t k = 0; k < MI_SPELLING_COUNT; k++) {
            if (MI_SPELLINGS[k].kind == 'O' && MI_SPELLINGS[k].id == i) {
                join_add(&joiner, MI_SPELLINGS[k].spelling);
            }
        }
        printf("  %-12s %s\n", MI_OPERATOR_NAMES[i], joiner.buffer ? joiner.buffer : "");
        free(joiner.buffer);
    }

    printf("builtins (%d):\n", BI_COUNT);
    for (int i = 0; i < BI_COUNT; i++) {
        MiTextJoiner joiner = {NULL, 0, 0};
        for (size_t k = 0; k < MI_BUILTIN_ALL_ALIAS_COUNT; k++) {
            if (MI_BUILTIN_ALL[k].builtin == i) {
                join_add(&joiner, MI_BUILTIN_ALL[k].spelling);
            }
        }
        printf("  %-16s %s\n", MI_BUILTIN_NAMES[i], joiner.buffer ? joiner.buffer : "");
        free(joiner.buffer);
    }
    return 0;
}

static int cmd_version(void) {
    printf("MI %s — Signals, not statements.\n", MI_VERSION);
    printf("runtime: C（原生实现）\n");
    printf("spellings: %d 组关键字 · %d 个运算符 · %d 个内置函数\n", KW_COUNT,
           OPK_COUNT, BI_COUNT);
    return 0;
}

static void print_usage(FILE *fh) {
    fputs("MI — Signals, not statements.\n\n"
          "用法：\n"
          "  mi                          进入交互式 REPL\n"
          "  mi 文件.mi                  运行一个程序\n"
          "  mi run 文件.mi              同上\n"
          "  mi run 文件.mi --runtime selfhost  用 MI 自举版解释器运行\n"
          "  mi check 文件.mi            只做诊断，不执行\n"
          "  mi glyphs                   列出所有拼写\n"
          "  mi ast 文件.mi              输出语法树\n"
          "  mi version                  版本\n",
          fh);
}

#ifdef _WIN32
/* Windows 的 argv 是 ANSI 代码页（中文系统是 GBK），统一转成 UTF-8。 */
static char **g_argv_utf8 = NULL;
static int g_argc_utf8 = 0;

static void convert_argv(void) {
    int argc = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) return;
    g_argv_utf8 = mi_calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        if (need <= 0) {
            g_argv_utf8[i] = mi_strdup("");
            continue;
        }
        g_argv_utf8[i] = mi_alloc((size_t)need);
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, g_argv_utf8[i], need, NULL, NULL);
    }
    g_argc_utf8 = argc;
    LocalFree(wide);
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    convert_argv();
    if (g_argv_utf8) {
        argv = g_argv_utf8;
        argc = g_argc_utf8;
    }
#endif
    if (argc < 2) {
        return cmd_repl();
    }
    const char *command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 ||
        strcmp(command, "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(command, "version") == 0 || strcmp(command, "-V") == 0 ||
        strcmp(command, "--version") == 0) {
        return cmd_version();
    }
    if (strcmp(command, "glyphs") == 0) return cmd_glyphs();
    if (strcmp(command, "repl") == 0) return cmd_repl();
    if (strcmp(command, "check") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: `check` needs a file\n");
            return 3;
        }
        return cmd_check(argv[2]);
    }
    if (strcmp(command, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: `run` needs a file\n");
            return 3;
        }
        int selfhost = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
                selfhost = strcmp(argv[i + 1], "selfhost") == 0;
                i++;
            }
        }
        if (selfhost) return cmd_run_selfhost(argv[2], 50000000, 20000);
        return cmd_run(argv[2], 5000000, 400);
    }
    if (strcmp(command, "selfhost") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: `selfhost` needs a file\n");
            return 3;
        }
        return cmd_run_selfhost(argv[2], 50000000, 20000);
    }
    /* 第一个参数是 .mi 文件：等价 run（支持把文件拖到 exe 上） */
    if (command[0] != '-' && (strstr(command, ".mi") || mi_read_source(command))) {
        return cmd_run(command, 5000000, 400);
    }
    fprintf(stderr, "error: unknown command `%s`\n", command);
    print_usage(stderr);
    return 3;
}
