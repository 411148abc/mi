/* bigint_test.c — 任意精度整数模块的自测程序（独立可执行）。
 *
 * 覆盖范围：
 *   1. 十进制字符串 ↔ MiBig 往返（含 0、±1、2^63、2^64、10^30、100 位大数）
 *   2. 加减乘除与 long long 交叉验证（能算的算一遍，溢出用代数恒等式校验）
 *   3. 边界恒等式：a+0 == a、a*1 == a、a*0 == 0、a-a == 0
 *   4. Python 除法语义：-7 // 2 == -4、-7 % 2 == 1 等，含大数版本
 *   5. 幂：2^100、(-3)^21、0^0 等
 *   6. cmp 排序一致性；fits_i64 / to_double / from_double / 非法输入
 *
 * 输出格式：每行 “表达式 = 结果”；失败行以 “FAIL” 开头。
 * 退出码：0 = 全部通过，1 = 有失败项。
 */
#include "bigint.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

/* ------------------------------------------------------------ 测试小工具 */

static void report(const char *kind, const char *expr, const char *got, const char *want) {
    g_checks++;
    printf("%s = %s\n", expr, got ? got : "<NULL>");
    if (!got || !want || strcmp(got, want) != 0) {
        g_failures++;
        printf("FAIL [%s] %s：期望 %s，实际 %s\n", kind, expr,
               want ? want : "<NULL>", got ? got : "<NULL>");
    }
}

static void check_assert(const char *what, int cond) {
    g_checks++;
    printf("%s = %s\n", what, cond ? "通过" : "失败");
    if (!cond) {
        g_failures++;
        printf("FAIL [断言] %s\n", what);
    }
}

/* 十进制往返 */
static void check_roundtrip(const char *text, const char *canon) {
    MiBig *v = mi_big_from_str(text);
    char *got = v ? mi_big_to_str(v) : NULL;
    char expr[1200];
    snprintf(expr, sizeof expr, "from_str(\"%s\")", text);
    report("十进制往返", expr, got, canon);
    free(got);
    mi_big_free(v);
}

/* 一次二元运算（a op b），并与期望文本比较 */
static void check_binop(const char *a_text, const char *b_text, char op, const char *want) {
    MiBig *a = mi_big_from_str(a_text);
    MiBig *b = mi_big_from_str(b_text);
    MiBig *r = NULL;
    if (a && b) {
        if (op == '+') r = mi_big_add(a, b);
        else if (op == '-') r = mi_big_sub(a, b);
        else if (op == '*') r = mi_big_mul(a, b);
    }
    char *got = r ? mi_big_to_str(r) : NULL;
    char expr[1200];
    snprintf(expr, sizeof expr, "(%s) %c (%s)", a_text, op, b_text);
    report("大数运算", expr, got, want);
    free(got);
    mi_big_free(r);
    mi_big_free(a);
    mi_big_free(b);
}

/* 除法：同时验证商、余数以及 a == b*q + r 恒等式 */
static void check_divmod(const char *a_text, const char *b_text, const char *want_q,
                         const char *want_r) {
    MiBig *a = mi_big_from_str(a_text);
    MiBig *b = mi_big_from_str(b_text);
    MiBig *rem = NULL;
    MiBig *q = (a && b) ? mi_big_divmod(a, b, &rem) : NULL;
    char *got_q = q ? mi_big_to_str(q) : NULL;
    char *got_r = rem ? mi_big_to_str(rem) : NULL;
    char expr_q[1300];
    char expr_r[1300];
    snprintf(expr_q, sizeof expr_q, "(%s) // (%s)", a_text, b_text);
    snprintf(expr_r, sizeof expr_r, "(%s) %% (%s)", a_text, b_text);
    report("floor 除法（商）", expr_q, got_q, want_q);
    report("floor 除法（余数）", expr_r, got_r, want_r);
    /* 恒等式：a == b*q + r，且 |r| < |b|，余数符号随除数 */
    int invariant = 0;
    if (q && rem) {
        MiBig *bq = mi_big_mul(b, q);
        MiBig *sum = mi_big_add(bq, rem);
        MiBig *abs_r = mi_big_abs(rem);
        MiBig *abs_b = mi_big_abs(b);
        invariant = mi_big_cmp(sum, a) == 0 && mi_big_cmp(abs_r, abs_b) < 0 &&
                    (mi_big_is_zero(rem) || mi_big_sign(rem) == mi_big_sign(b));
        mi_big_free(bq);
        mi_big_free(sum);
        mi_big_free(abs_r);
        mi_big_free(abs_b);
    }
    check_assert("  └ a == b*q + r 且 |r| < |b| 且余数符号随除数", invariant);
    free(got_q);
    free(got_r);
    mi_big_free(q);
    mi_big_free(rem);
    mi_big_free(a);
    mi_big_free(b);
}

/* 幂 */
static void check_pow(const char *base_text, long long exponent, const char *want) {
    MiBig *base = mi_big_from_str(base_text);
    MiBig *r = base ? mi_big_pow(base, (int64_t)exponent) : NULL;
    char *got = r ? mi_big_to_str(r) : NULL;
    char expr[1200];
    snprintf(expr, sizeof expr, "(%s) ** (%lld)", base_text, exponent);
    report("幂", expr, got, want);
    free(got);
    mi_big_free(r);
    mi_big_free(base);
}

/* ------------------------------------------------ 1. 十进制往返用例 */
static const char *BIG_R1 =
    "8986432616224375529841401596556550893536706509885777646509708413921254562007404120104639405802105032";
static const char *BIG_R2 =
    "9078648868559701811567352095764854708664623408520086338429607236611857149120244726399564889282212";

static const struct {
    const char *text;
    const char *canon;
} ROUND_TRIP[] = {
    {"0", "0"},
    {"-1", "-1"},
    {"1", "1"},
    {"2", "2"},
    {"-2", "-2"},
    {"9223372036854775807", "9223372036854775807"},
    {"-9223372036854775808", "-9223372036854775808"},
    {"9223372036854775808", "9223372036854775808"},        /* 2^63 */
    {"18446744073709551616", "18446744073709551616"},      /* 2^64 */
    {"340282366920938463463374607431768211456",
     "340282366920938463463374607431768211456"},          /* 2^128 */
    {"1000000000000000000000000000000", "1000000000000000000000000000000"},
    {"-1000000000000000000000000000000", "-1000000000000000000000000000000"},
    {"123456789012345678901234567890", "123456789012345678901234567890"},
    {"000123", "123"},
    {"+42", "42"},
    {"-007", "-7"},
    {"0000", "0"},
    {"-0000", "0"},
    {"00000000000000000000000000000000000000000000000000000000000000000001", "1"},
};

/* ------------------------------------------------ 2. long long 交叉验证 */
/* 样本刻意避开会触发边界陷阱的 INT64_MIN/-1 组合 */
static const long long LL_SAMPLES[] = {
    0LL, 1LL, -1LL, 2LL, -2LL, 7LL, -7LL, 1000003LL, 2147483647LL, -4294967296LL
};

static void ll_text(long long value, char out[32]) {
    snprintf(out, 32, "%lld", value);
}

static void check_ll_arithmetic(void) {
    size_t n = sizeof(LL_SAMPLES) / sizeof(LL_SAMPLES[0]);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            long long a = LL_SAMPLES[i];
            long long b = LL_SAMPLES[j];
            MiBig *ba = mi_big_from_i64(a);
            MiBig *bb = mi_big_from_i64(b);
            char want[32];
            char expr[128];

            /* 加法 */
            MiBig *r = mi_big_add(ba, bb);
            long long sum = 0;
            char *got = NULL;
            snprintf(expr, sizeof expr, "(%lld) + (%lld)", a, b);
            got = mi_big_to_str(r);
            if (!__builtin_add_overflow(a, b, &sum)) {
                ll_text(sum, want);
                report("ll 加法", expr, got, want);
            } else {
                /* 溢出：用 (a+b)-b == a 自检 */
                g_checks++;
                printf("%s = %s\n", expr, got ? got : "<NULL>");
                MiBig *back = mi_big_sub(r, bb);
                int ok = back && mi_big_cmp(back, ba) == 0;
                mi_big_free(back);
                if (!ok) {
                    g_failures++;
                    printf("FAIL [ll 加法溢出校验] %s\n", expr);
                }
            }
            free(got);
            mi_big_free(r);

            /* 减法 */
            r = mi_big_sub(ba, bb);
            long long diff = 0;
            snprintf(expr, sizeof expr, "(%lld) - (%lld)", a, b);
            got = mi_big_to_str(r);
            if (!__builtin_sub_overflow(a, b, &diff)) {
                ll_text(diff, want);
                report("ll 减法", expr, got, want);
            } else {
                /* 溢出：用 (a-b)+b == a 自检 */
                g_checks++;
                printf("%s = %s\n", expr, got ? got : "<NULL>");
                MiBig *back = mi_big_add(r, bb);
                int ok = back && mi_big_cmp(back, ba) == 0;
                mi_big_free(back);
                if (!ok) {
                    g_failures++;
                    printf("FAIL [ll 减法溢出校验] %s\n", expr);
                }
            }
            free(got);
            mi_big_free(r);

            /* 乘法：不溢出就直接比；溢出用 r / a == b 且余数为 0 校验 */
            r = mi_big_mul(ba, bb);
            long long prod = 0;
            snprintf(expr, sizeof expr, "(%lld) * (%lld)", a, b);
            got = mi_big_to_str(r);
            if (!__builtin_mul_overflow(a, b, &prod)) {
                ll_text(prod, want);
                report("ll 乘法", expr, got, want);
            } else {
                g_checks++;
                printf("%s = %s\n", expr, got ? got : "<NULL>");
                int ok = 0;
                if (got && !mi_big_is_zero(ba)) {
                    MiBig *rem = NULL;
                    MiBig *q = mi_big_divmod(r, ba, &rem);
                    ok = q && rem && mi_big_is_zero(rem) && mi_big_cmp(q, bb) == 0;
                    mi_big_free(q);
                    mi_big_free(rem);
                }
                if (!ok) {
                    g_failures++;
                    printf("FAIL [ll 乘法溢出校验] %s\n", expr);
                }
            }
            free(got);
            mi_big_free(r);

            /* 除法：C 是向零截断，Python 是向下取整，按公式换算后再比 */
            if (b != 0) {
                long long tq = a / b;
                long long tr = a % b;
                long long fq = tq;
                long long fr = tr;
                if (tr != 0 && ((tr < 0) != (b < 0))) {
                    fq -= 1;
                    fr += b;
                }
                MiBig *rem = NULL;
                MiBig *q = mi_big_divmod(ba, bb, &rem);
                char *gq = q ? mi_big_to_str(q) : NULL;
                char *gr = rem ? mi_big_to_str(rem) : NULL;
                snprintf(expr, sizeof expr, "(%lld) // (%lld)", a, b);
                ll_text(fq, want);
                report("ll floor 除法", expr, gq, want);
                snprintf(expr, sizeof expr, "(%lld) %% (%lld)", a, b);
                ll_text(fr, want);
                report("ll floor 取模", expr, gr, want);
                free(gq);
                free(gr);
                mi_big_free(q);
                mi_big_free(rem);
            }

            mi_big_free(ba);
            mi_big_free(bb);
        }
    }
}

/* ------------------------------------------------ 3. 大数运算 / 边界恒等式 */

static const char *A30 = "123456789012345678901234567890";
static const char *B30 = "987654321098765432109876543210";
static const char *X30 = "999999999999999999999999999999";       /* 10^30 - 1 */
static const char *Y30 = "1000000000000000000000000000001";     /* 10^30 + 1 */
static const char *P30 = "1000000000000000000000000000000";     /* 10^30 */

/* a + 0 == a、a * 1 == a、a * 0 == 0、a - a == 0 等边界恒等式 */
static void check_identities(const char *text) {
    MiBig *a = mi_big_from_str(text);
    MiBig *zero = mi_big_from_i64(0);
    MiBig *one = mi_big_from_i64(1);
    if (!a) {
        g_checks++;
        g_failures++;
        printf("FAIL 无法解析 %s\n", text);
        return;
    }
    char expr[1200];

    MiBig *r = mi_big_add(a, zero);
    char *got = r ? mi_big_to_str(r) : NULL;
    snprintf(expr, sizeof expr, "(%s) + (0)", text);
    report("a+0==a", expr, got, text);
    free(got);
    mi_big_free(r);

    r = mi_big_add(zero, a);
    got = r ? mi_big_to_str(r) : NULL;
    snprintf(expr, sizeof expr, "(0) + (%s)", text);
    report("0+a==a", expr, got, text);
    free(got);
    mi_big_free(r);

    r = mi_big_mul(a, one);
    got = r ? mi_big_to_str(r) : NULL;
    snprintf(expr, sizeof expr, "(%s) * (1)", text);
    report("a*1==a", expr, got, text);
    free(got);
    mi_big_free(r);

    r = mi_big_mul(a, zero);
    got = r ? mi_big_to_str(r) : NULL;
    snprintf(expr, sizeof expr, "(%s) * (0)", text);
    report("a*0==0", expr, got, "0");
    free(got);
    mi_big_free(r);

    r = mi_big_sub(a, a);
    got = r ? mi_big_to_str(r) : NULL;
    snprintf(expr, sizeof expr, "(%s) - (%s)", text, text);
    report("a-a==0", expr, got, "0");
    free(got);
    mi_big_free(r);

    mi_big_free(a);
    mi_big_free(zero);
    mi_big_free(one);
}

/* ------------------------------------------------- 4. Python 除法语义 */
static void check_python_divmod(void) {
    /* 题目给出的四组 */
    check_divmod("7", "2", "3", "1");
    check_divmod("-7", "2", "-4", "1");
    check_divmod("7", "-2", "-4", "-1");
    check_divmod("-7", "-2", "3", "-1");
    /* 整除与零 */
    check_divmod("6", "3", "2", "0");
    check_divmod("-6", "3", "-2", "0");
    check_divmod("6", "-3", "-2", "0");
    check_divmod("0", "5", "0", "0");
    check_divmod("-1", "2", "-1", "1");
    check_divmod("1", "-2", "-1", "-1");
    /* 大数版本：10^25 + 7 对 ±2 */
    check_divmod("10000000000000000000000007", "2",
                 "5000000000000000000000003", "1");
    check_divmod("10000000000000000000000007", "-2",
                 "-5000000000000000000000004", "-1");
    check_divmod("-10000000000000000000000007", "2",
                 "-5000000000000000000000004", "1");
    check_divmod("-10000000000000000000000007", "-2",
                 "5000000000000000000000003", "-1");
    /* 30 位大数 */
    check_divmod(A30, B30, "0", A30);
    check_divmod("-123456789012345678901234567890", B30, "-1",
                 "864197532086419753208641975320");
    check_divmod(A30, "-987654321098765432109876543210", "-1",
                 "-864197532086419753208641975320");
    check_divmod("-123456789012345678901234567890",
                 "-987654321098765432109876543210", "0",
                 "-123456789012345678901234567890");
    /* 100 位大数 */
    check_divmod(BIG_R1, BIG_R2, "989",
                 "7648885218830438201290373845109586667393958859412257802826856912127841527482085695469730301997364");
    check_divmod("-8986432616224375529841401596556550893536706509885777646509708413921254562007404120104639405802105032",
                 BIG_R2, "-990",
                 "1429763649729263610276978250655268041270664549107828535602750324484015621638159030929834587284848");
}

/* --------------------------------------------------------------- 5. 幂 */
static void check_powers(void) {
    check_pow("2", 100, "1267650600228229401496703205376");
    check_pow("-3", 21, "-10460353203");
    check_pow("0", 0, "1");                    /* 0^0 == 1 */
    check_pow("0", 5, "0");
    check_pow("1", 999, "1");
    check_pow("-1", 1000, "1");
    check_pow("-1", 1001, "-1");
    check_pow("10", 30, P30);
    check_pow("-2", 63, "-9223372036854775808");
    check_pow("2", 0, "1");
    check_pow("-10", 25, "-10000000000000000000000000");
    /* 负指数按契约返回 NULL */
    MiBig *base = mi_big_from_i64(2);
    MiBig *bad = mi_big_pow(base, -1);
    check_assert("(2) ** (-1) 返回 NULL", bad == NULL);
    mi_big_free(bad);
    mi_big_free(base);
}

/* -------------------------------------------------------------- 6. cmp */
static void check_cmp_ordering(void) {
    static const char *order[] = {
        "-1000000000000000000000000000001",
        "-1000000000000000000000000000000",
        "-9223372036854775809",
        "-9223372036854775808",
        "-2",
        "-1",
        "0",
        "1",
        "2",
        "9223372036854775807",
        "9223372036854775808",
        "18446744073709551616",
        "1000000000000000000000000000000"
    };
    size_t n = sizeof(order) / sizeof(order[0]);
    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            MiBig *a = mi_big_from_str(order[i]);
            MiBig *b = mi_big_from_str(order[j]);
            int want = (i < j) ? -1 : (i > j ? 1 : 0);
            int got = mi_big_cmp(a, b);
            if (got != want) {
                ok = 0;
                printf("FAIL [cmp] cmp(%s, %s)：期望 %d，实际 %d\n", order[i], order[j],
                       want, got);
            }
            mi_big_free(a);
            mi_big_free(b);
        }
    }
    char expr[128];
    snprintf(expr, sizeof expr, "cmp 两两比较（%zu 个样本，%zu 组）", n, n * n);
    check_assert(expr, ok);

    /* -0 与 0 相等，且零的规范形 */
    MiBig *neg_zero = mi_big_from_str("-0");
    MiBig *zero = mi_big_from_i64(0);
    char *text = neg_zero ? mi_big_to_str(neg_zero) : NULL;
    report("零规范化", "from_str(\"-0\")", text, "0");
    free(text);
    check_assert("cmp(-0, 0) == 0", mi_big_cmp(neg_zero, zero) == 0);
    check_assert("sign(-0) == 0", mi_big_sign(neg_zero) == 0);
    {
        MiBig *zero_sub = mi_big_sub(zero, zero);
        check_assert("is_zero(0 - 0)", mi_big_is_zero(zero_sub));
        mi_big_free(zero_sub);
    }
    check_assert("is_zero(from_i64(0))", mi_big_is_zero(zero));
    mi_big_free(neg_zero);
    mi_big_free(zero);
}

/* ---------------------------------------- 7. 转换 / 无效输入 / 内存语义 */
static void check_conversions(void) {
    /* from_i64 与 from_str 一致 */
    static const long long values[] = {0, 1, -1, 42, -42, INT64_MAX, INT64_MIN};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char text[32];
        ll_text(values[i], text);
        MiBig *v = mi_big_from_i64(values[i]);
        char *got = v ? mi_big_to_str(v) : NULL;
        char expr[96];
        snprintf(expr, sizeof expr, "from_i64(%lld)", values[i]);
        report("from_i64", expr, got, text);
        free(got);
        mi_big_free(v);
    }

    /* fits_i64 边界 */
    struct {
        const char *text;
        int fits;
        long long want;
    } cases[] = {
        {"0", 1, 0},
        {"1", 1, 1},
        {"-1", 1, -1},
        {"9223372036854775807", 1, INT64_MAX},
        {"-9223372036854775808", 1, INT64_MIN},
        {"9223372036854775808", 0, 0},
        {"-9223372036854775809", 0, 0},
        {"18446744073709551616", 0, 0},
        {"1000000000000000000000000000000", 0, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MiBig *v = mi_big_from_str(cases[i].text);
        int64_t out = 0;
        int fits = mi_big_fits_i64(v, &out);
        char expr[128];
        snprintf(expr, sizeof expr, "fits_i64(%s)", cases[i].text);
        check_assert(expr, fits == cases[i].fits &&
                               (!fits || out == (int64_t)cases[i].want));
        mi_big_free(v);
    }

    /* from_double：向零截断，NaN/Inf 返回 NULL */
    struct {
        double input;
        const char *want;
    } dbl[] = {
        {0.0, "0"},
        {-0.0, "0"},
        {1.9, "1"},
        {-1.9, "-1"},
        {-2.5, "-2"},
        {9007199254740992.0, "9007199254740992"},          /* 2^53 */
        {9223372036854775808.0, "9223372036854775808"},    /* 2^63 */
        {1e30, "1000000000000000019884624838656"},
    };
    for (size_t i = 0; i < sizeof(dbl) / sizeof(dbl[0]); i++) {
        MiBig *v = mi_big_from_double(dbl[i].input);
        char *got = v ? mi_big_to_str(v) : NULL;
        char expr[128];
        snprintf(expr, sizeof expr, "from_double(%.17g)", dbl[i].input);
        report("from_double", expr, got, dbl[i].want);
        free(got);
        mi_big_free(v);
    }
    check_assert("from_double(NAN) == NULL", mi_big_from_double(NAN) == NULL);
    check_assert("from_double(INFINITY) == NULL",
                 mi_big_from_double(INFINITY) == NULL);
    check_assert("from_double(-INFINITY) == NULL",
                 mi_big_from_double(-INFINITY) == NULL);

    /* to_double：大数按相对误差比较 */
    {
        MiBig *v = mi_big_from_str(P30);
        double got = mi_big_to_double(v);
        char expr[128];
        snprintf(expr, sizeof expr, "to_double(%s)", P30);
        g_checks++;
        printf("%s = %.17g\n", expr, got);
        if (fabs(got - 1e30) > 1e30 * 1e-15) {
            g_failures++;
            printf("FAIL [to_double] %s：期望约 1e30，实际 %.17g\n", expr, got);
        }
        mi_big_free(v);
    }
    {
        MiBig *v = mi_big_from_str("-123456789012345678901234567890");
        double got = mi_big_to_double(v);
        double want = -123456789012345678901234567890.0;
        g_checks++;
        printf("to_double(-123456789012345678901234567890) = %.17g\n", got);
        if (fabs(got - want) > fabs(want) * 1e-15) {
            g_failures++;
            printf("FAIL [to_double] 期望约 %.17g，实际 %.17g\n", want, got);
        }
        mi_big_free(v);
    }

    /* 非法字符串 */
    static const char *bads[] = {"", "-", "+", "abc", "12a", " 1", "1 ",
                                 "--1", "0x10", "+-3", "1.5"};
    int all_bad = 1;
    for (size_t i = 0; i < sizeof(bads) / sizeof(bads[0]); i++) {
        MiBig *v = mi_big_from_str(bads[i]);
        if (v) {
            all_bad = 0;
            printf("FAIL [非法输入] from_str(\"%s\") 应当返回 NULL\n", bads[i]);
        }
        mi_big_free(v);
    }
    check_assert("非法十进制字符串全部返回 NULL", all_bad);
    check_assert("from_str(NULL) == NULL", mi_big_from_str(NULL) == NULL);

    /* 除数为 0 */
    {
        MiBig *a = mi_big_from_str("123");
        MiBig *zero = mi_big_from_i64(0);
        MiBig *rem = NULL;
        MiBig *q = mi_big_divmod(a, zero, &rem);
        check_assert("除数为 0 返回 NULL", q == NULL);
        check_assert("除数为 0 时余数置 NULL", rem == NULL);
        mi_big_free(q);
        mi_big_free(rem);
        mi_big_free(a);
        mi_big_free(zero);
    }

    /* rem 参数可为 NULL */
    {
        MiBig *a = mi_big_from_str("-12345678901234567890");
        MiBig *b = mi_big_from_i64(7);
        MiBig *q = mi_big_divmod(a, b, NULL);
        char *got = q ? mi_big_to_str(q) : NULL;
        report("rem==NULL 时商", "(-12345678901234567890) // (7)", got,
               "-1763668414462081128");
        free(got);
        mi_big_free(q);
        mi_big_free(a);
        mi_big_free(b);
    }

    /* copy 独立性：释放原对象后副本仍可用 */
    {
        MiBig *a = mi_big_from_str("123456789012345678901234567890");
        MiBig *c = mi_big_copy(a);
        mi_big_free(a);
        char *got = c ? mi_big_to_str(c) : NULL;
        report("copy 后释放原对象仍可用", "mi_big_copy(a)", got,
               "123456789012345678901234567890");
        free(got);
        mi_big_free(c);
    }

    /* neg / abs / sign / is_zero */
    {
        MiBig *v = mi_big_from_str("-123");
        MiBig *n = mi_big_neg(v);
        MiBig *ab = mi_big_abs(v);
        char *got_n = n ? mi_big_to_str(n) : NULL;
        char *got_ab = ab ? mi_big_to_str(ab) : NULL;
        report("neg", "neg(-123)", got_n, "123");
        report("abs", "abs(-123)", got_ab, "123");
        free(got_n);
        free(got_ab);
        mi_big_free(v);
        mi_big_free(n);
        mi_big_free(ab);
    }
    {
        MiBig *v = mi_big_from_str("5");
        MiBig *n = mi_big_neg(v);
        char *got = n ? mi_big_to_str(n) : NULL;
        report("neg 正数", "neg(5)", got, "-5");
        free(got);
        mi_big_free(v);
        mi_big_free(n);
    }
    {
        MiBig *z = mi_big_from_i64(0);
        MiBig *n = mi_big_neg(z);
        char *got = n ? mi_big_to_str(n) : NULL;
        report("neg 零", "neg(0)", got, "0");
        {
            MiBig *neg5 = mi_big_from_i64(-5);
            check_assert("sign(-5) == -1", mi_big_sign(neg5) == -1);
            mi_big_free(neg5);
        }
        free(got);
        mi_big_free(z);
        mi_big_free(n);
    }
}

/* --------------------------------------------------------------- 主流程 */
int main(void) {
    printf("===== 1. 构造/转换往返 =====\n");
    for (size_t i = 0; i < sizeof(ROUND_TRIP) / sizeof(ROUND_TRIP[0]); i++) {
        check_roundtrip(ROUND_TRIP[i].text, ROUND_TRIP[i].canon);
    }
    check_roundtrip(BIG_R1, BIG_R1);
    check_roundtrip(BIG_R2, BIG_R2);

    printf("\n===== 2. 加减乘（大数硬编码向量） =====\n");
    check_binop(A30, B30, '+', "1111111110111111111011111111100");
    check_binop(A30, B30, '-', "-864197532086419753208641975320");
    check_binop(B30, A30, '-', "864197532086419753208641975320");
    check_binop(A30, B30, '*',
                "121932631137021795226185032733622923332237463801111263526900");
    check_binop(X30, Y30, '+', "2000000000000000000000000000000");
    check_binop(Y30, X30, '-', "2");
    check_binop(X30, Y30, '*',
                "999999999999999999999999999999999999999999999999999999999999");
    check_binop(BIG_R1, BIG_R2, '+',
                "8995511265092935231652968948652315748245371133294297732848138021157866419156524364831038970691387244");
    check_binop(BIG_R1, BIG_R2, '-',
                "8977353967355815828029834244460786038828041886477257560171278806684642704858283875378239840912822820");
    check_binop(BIG_R1, BIG_R2, '*',
                "81584666303673427952830263168772386231501220469181924700442323899404961667467546857334539934353160380233478881000217270444624090458230893848235783898096053398372188638537141903375781067645113290784");
    check_binop(BIG_R1, "-9078648868559701811567352095764854708664623408520086338429607236611857149120244726399564889282212", '*',
                "-81584666303673427952830263168772386231501220469181924700442323899404961667467546857334539934353160380233478881000217270444624090458230893848235783898096053398372188638537141903375781067645113290784");

    printf("\n===== 3. 边界恒等式 =====\n");
    check_identities("0");
    check_identities("1");
    check_identities("-1");
    check_identities(A30);
    check_identities("-123456789012345678901234567890");
    check_identities(BIG_R1);

    printf("\n===== 4. Python 除法语义 =====\n");
    check_python_divmod();

    printf("\n===== 5. 幂 =====\n");
    check_powers();

    printf("\n===== 6. cmp 排序一致性 =====\n");
    check_cmp_ordering();

    printf("\n===== 7. long long 交叉验证 =====\n");
    check_ll_arithmetic();

    printf("\n===== 8. 转换 / 无效输入 / 内存语义 =====\n");
    check_conversions();

    printf("\n----------------------------------------\n");
    printf("共 %d 项检查，失败 %d 项\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("全部通过\n");
        return 0;
    }
    printf("存在失败用例\n");
    return 1;
}
