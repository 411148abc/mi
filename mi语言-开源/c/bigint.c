/* bigint.c — 任意精度整数的实现。
 *
 * 表示：符号（-1 / 0 / +1）+ 小端 uint32_t limb 数组，基 2^32。
 *   数值零一律规范化为 len == 0 且 sign == 0（limbs 可为 NULL）。
 *
 * 算法：
 *   - 加/减：按 limb 逐位带进位/借位处理，先比较绝对值决定符号。
 *   - 乘：O(n*m) 竖式乘法，用 uint64_t 承接中间结果。
 *   - 除：逐位长除法（从最高位到最低位，每步把余数左移一位并试减）。
 *         比 Knuth Algorithm D 简单得多、没有估商修正的边界陷阱，对解释器
 *         的规模完全够用；复杂度 O(位数 * limb 数)。
 *   - 幂：平方求幂（快速幂）。
 *   - 十进制互转：按 10^9 分块，避免逐位乘 10 的 O(n^2) 放大。
 *
 * 内存：每个 limb 数组与对象本身都用 malloc/calloc，mi_big_free 释放。
 *   内存不足时与项目其它模块一致：报错后 exit(70)，不做错误传播。
 */
#include "bigint.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 十进制分块基数：10^9 能放进一个 limb，且求余可用 64 位除法 */
#define BIG_DEC_BASE 1000000000u
/* 每块十进制位数 */
#define BIG_DEC_DIGITS 9

struct MiBig {
    int sign;          /* -1 / 0 / +1 */
    size_t len;        /* limb 个数；0 表示数值为零 */
    uint32_t *limbs;   /* 小端 limb 数组，len == 0 时为 NULL */
};

/* ------------------------------------------------------------ 内存/基础 */

static void big_oom(void) {
    fputs("bigint: out of memory\n", stderr);
    exit(70);
}

/* 新建对象，分配 len 个零初始化的 limb（sign 先置 0，由调用方填） */
static MiBig *big_raw(size_t len) {
    MiBig *out = (MiBig *)malloc(sizeof(MiBig));
    if (!out) big_oom();
    out->sign = 0;
    out->len = len;
    out->limbs = NULL;
    if (len > 0) {
        out->limbs = (uint32_t *)calloc(len, sizeof(uint32_t));
        if (!out->limbs) big_oom();
    }
    return out;
}

/* 接管一段已裁掉高位零的 limb 数组（可能为 NULL / len == 0） */
static MiBig *big_take_mag(uint32_t *mag, size_t len, int sign) {
    MiBig *out = (MiBig *)malloc(sizeof(MiBig));
    if (!out) big_oom();
    out->sign = 0;
    out->len = 0;
    out->limbs = NULL;
    if (len > 0 && mag) {
        out->len = len;
        out->sign = sign < 0 ? -1 : 1;
        out->limbs = mag;
    } else {
        free(mag);
    }
    return out;
}

/* 只要绝对值数组去掉高位零后的长度 */
static size_t mag_trim_len(const uint32_t *a, size_t n) {
    while (n > 0 && a[n - 1] == 0) n--;
    return n;
}

/* 比较两个 magnitude：-1 / 0 / 1 */
static int mag_cmp(const uint32_t *a, size_t an, const uint32_t *b, size_t bn) {
    an = mag_trim_len(a, an);
    bn = mag_trim_len(b, bn);
    if (an != bn) return an < bn ? -1 : 1;
    for (size_t i = an; i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* 绝对值相加，返回 malloc 的数组（长度已裁剪），*out_len 为有效长度 */
static uint32_t *mag_add(const uint32_t *a, size_t an, const uint32_t *b, size_t bn,
                         size_t *out_len) {
    size_t n = (an > bn ? an : bn) + 1;
    uint32_t *out = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!out) big_oom();
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t sum = carry;
        if (i < an) sum += a[i];
        if (i < bn) sum += b[i];
        out[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    *out_len = mag_trim_len(out, n);
    return out;
}

/* 绝对值相减，要求 |a| >= |b|；返回 malloc 的数组 */
static uint32_t *mag_sub(const uint32_t *a, size_t an, const uint32_t *b, size_t bn,
                         size_t *out_len) {
    uint32_t *out = (uint32_t *)calloc(an ? an : 1, sizeof(uint32_t));
    if (!out) big_oom();
    int64_t borrow = 0;
    for (size_t i = 0; i < an; i++) {
        int64_t cur = (int64_t)a[i] - borrow - (i < bn ? (int64_t)b[i] : 0);
        if (cur < 0) {
            cur += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out[i] = (uint32_t)cur;
    }
    *out_len = mag_trim_len(out, an);
    return out;
}

/* 绝对值相乘，返回 malloc 的数组；任一因子为零时长度为 0 */
static uint32_t *mag_mul(const uint32_t *a, size_t an, const uint32_t *b, size_t bn,
                         size_t *out_len) {
    size_t n = an + bn;
    uint32_t *out = (uint32_t *)calloc(n ? n : 1, sizeof(uint32_t));
    if (!out) big_oom();
    for (size_t i = 0; i < an; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < bn; j++) {
            uint64_t cur = (uint64_t)a[i] * b[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        /* 该位置此前必为 0（calloc，且前面的行最多写到 i + bn - 1） */
        out[i + bn] = (uint32_t)carry;
    }
    *out_len = mag_trim_len(out, n);
    return out;
}

/* 绝对值带余除法（逐位长除法）：a = q * b + r，0 <= r < b；b 非零。
 * 商数组容量为 an + 1，调用方可以用它再做一次“加一”。 */
static void mag_divmod(const uint32_t *a, size_t an, const uint32_t *b, size_t bn,
                       uint32_t **quot, size_t *qn, uint32_t **rem, size_t *rn) {
    uint32_t *q = (uint32_t *)calloc(an + 1, sizeof(uint32_t));
    uint32_t *r = (uint32_t *)calloc(bn + 1, sizeof(uint32_t));
    if (!q || !r) big_oom();
    size_t rlen = 0;
    for (size_t i = an; i-- > 0;) {
        for (int bit = 31; bit >= 0; bit--) {
            /* 余数左移一位，把被除数的当前位补进来 */
            uint32_t carry = (a[i] >> (unsigned)bit) & 1u;
            for (size_t k = 0; k < rlen; k++) {
                uint32_t next = r[k] >> 31;
                r[k] = (r[k] << 1) | carry;
                carry = next;
            }
            if (carry) r[rlen++] = carry;
            /* 试减：余数够大就减去除数，并在商的对应位置 1 */
            if (mag_cmp(r, rlen, b, bn) >= 0) {
                int64_t borrow = 0;
                for (size_t k = 0; k < rlen; k++) {
                    int64_t cur = (int64_t)r[k] - borrow - (k < bn ? (int64_t)b[k] : 0);
                    if (cur < 0) {
                        cur += (int64_t)1 << 32;
                        borrow = 1;
                    } else {
                        borrow = 0;
                    }
                    r[k] = (uint32_t)cur;
                }
                q[i] |= (uint32_t)1 << (unsigned)bit;
                rlen = mag_trim_len(r, rlen);
            }
        }
    }
    *quot = q;
    *qn = mag_trim_len(q, an + 1);
    *rem = r;
    *rn = rlen;
}

/* ---------------------------------------------------------------- 构造 */

MiBig *mi_big_from_i64(int64_t value) {
    if (value == 0) return big_raw(0);
    uint64_t mag = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    size_t len = mag > 0xFFFFFFFFu ? 2 : 1;
    MiBig *out = big_raw(len);
    out->limbs[0] = (uint32_t)mag;
    if (len == 2) out->limbs[1] = (uint32_t)(mag >> 32);
    out->sign = value < 0 ? -1 : 1;
    return out;
}

MiBig *mi_big_copy(const MiBig *value) {
    if (!value) return NULL;
    MiBig *out = big_raw(value->len);
    if (value->len > 0) memcpy(out->limbs, value->limbs, value->len * sizeof(uint32_t));
    out->sign = value->sign;
    return out;
}

void mi_big_free(MiBig *value) {
    if (!value) return;
    free(value->limbs);
    free(value);
}

MiBig *mi_big_from_str(const char *text) {
    if (!text) return NULL;
    const char *p = text;
    int sign = 1;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1;
        p++;
    }
    const char *end = p;
    while (*end >= '0' && *end <= '9') end++;
    if (end == p || *end != '\0') return NULL;   /* 没有数字或多出字符 */
    /* 跳过前导零，但至少留一位 */
    while (p + 1 < end && *p == '0') p++;
    size_t digits = (size_t)(end - p);
    size_t groups = (digits + BIG_DEC_DIGITS - 1) / BIG_DEC_DIGITS;
    /* 每 9 位十进制不足 1 个 limb，按组分块最多产生 groups 个 limb */
    size_t cap = groups + 2;
    uint32_t *acc = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!acc) big_oom();
    size_t an = 0;
    while (p < end) {
        size_t take = (size_t)(end - p);
        if (take > BIG_DEC_DIGITS) take = BIG_DEC_DIGITS;
        uint32_t chunk = 0;
        uint32_t scale = 1;
        for (size_t i = 0; i < take; i++) {
            chunk = chunk * 10u + (uint32_t)(p[i] - '0');
            scale *= 10u;
        }
        p += take;
        /* acc = acc * 10^take + chunk */
        uint64_t carry = chunk;
        for (size_t i = 0; i < an; i++) {
            uint64_t cur = (uint64_t)acc[i] * scale + carry;
            acc[i] = (uint32_t)cur;
            carry = cur >> 32;
        }
        if (carry) acc[an++] = (uint32_t)carry;
    }
    an = mag_trim_len(acc, an);
    MiBig *out = big_take_mag(acc, an, sign);
    return out;
}

MiBig *mi_big_from_double(double value) {
    if (!isfinite(value)) return NULL;
    if (value == 0.0) return big_raw(0);   /* 同时吸收 -0.0 */
    int sign = value < 0 ? -1 : 1;
    double mag = fabs(value);
    /* |value| < 2^63 时直接截断成 int64，转换是无损的 */
    if (mag < 9223372036854775808.0) {
        MiBig *out = mi_big_from_i64((int64_t)mag);
        if (out) out->sign = sign;
        return out;
    }
    /* 更大的数：取出 53 位尾数与指数，再左移构造。value = mant * 2^shift */
    int exp = 0;
    double frac = frexp(mag, &exp);              /* mag = frac * 2^exp，0.5 <= frac < 1 */
    uint64_t mant = (uint64_t)ldexp(frac, 53);   /* 精确的整数尾数 */
    int shift = exp - 53;                        /* 此时 shift >= 11，恒为正 */
    size_t ls = (size_t)(shift / 32);
    unsigned bit = (unsigned)(shift % 32);
    size_t n = ls + 3;
    uint32_t *acc = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!acc) big_oom();
    uint32_t part[3] = {(uint32_t)mant, (uint32_t)(mant >> 32), 0};
    if (bit != 0) {
        part[2] = part[1] >> (32 - bit);
        part[1] = (part[1] << bit) | (part[0] >> (32 - bit));
        part[0] = part[0] << bit;
    }
    acc[ls] = part[0];
    acc[ls + 1] = part[1];
    acc[ls + 2] = part[2];
    size_t len = mag_trim_len(acc, n);
    return big_take_mag(acc, len, sign);
}

/* ---------------------------------------------------------------- 转换 */

char *mi_big_to_str(const MiBig *value) {
    if (!value) return NULL;
    if (value->sign == 0 || value->len == 0) {
        char *out = (char *)malloc(2);
        if (!out) big_oom();
        memcpy(out, "0", 2);
        return out;
    }
    /* 复制一份绝对值，反复除以 10^9 收集十进制块 */
    size_t len = value->len;
    uint32_t *tmp = (uint32_t *)malloc(len * sizeof(uint32_t));
    if (!tmp) big_oom();
    memcpy(tmp, value->limbs, len * sizeof(uint32_t));
    size_t cap = len * 2 + 2;
    uint32_t *chunks = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!chunks) big_oom();
    size_t count = 0;
    while (len > 0) {
        uint64_t rem = 0;
        for (size_t i = len; i-- > 0;) {
            uint64_t cur = (rem << 32) | tmp[i];
            tmp[i] = (uint32_t)(cur / BIG_DEC_BASE);
            rem = cur % BIG_DEC_BASE;
        }
        chunks[count++] = (uint32_t)rem;
        len = mag_trim_len(tmp, len);
    }
    free(tmp);
    /* 最高块不带前导零，其余块固定 9 位 */
    size_t total = count * BIG_DEC_DIGITS + 2;
    char *out = (char *)malloc(total);
    if (!out) big_oom();
    size_t pos = 0;
    if (value->sign < 0) out[pos++] = '-';
    pos += (size_t)snprintf(out + pos, total - pos, "%u", (unsigned)chunks[count - 1]);
    for (size_t i = count - 1; i-- > 0;) {
        pos += (size_t)snprintf(out + pos, total - pos, "%09u", (unsigned)chunks[i]);
    }
    out[pos] = '\0';
    free(chunks);
    return out;
}

double mi_big_to_double(const MiBig *value) {
    if (!value || value->sign == 0) return 0.0;
    double out = 0.0;
    for (size_t i = value->len; i-- > 0;) {
        out = out * 4294967296.0 + (double)value->limbs[i];
    }
    return value->sign < 0 ? -out : out;
}

int mi_big_fits_i64(const MiBig *value, int64_t *out) {
    if (!value) return 0;
    if (value->sign == 0) {
        if (out) *out = 0;
        return 1;
    }
    if (value->len > 2) return 0;
    uint64_t mag = 0;
    for (size_t i = value->len; i-- > 0;) mag = (mag << 32) | value->limbs[i];
    if (value->sign > 0) {
        if (mag > (uint64_t)INT64_MAX) return 0;
        if (out) *out = (int64_t)mag;
    } else {
        if (mag > (uint64_t)INT64_MAX + 1u) return 0;
        if (out) {
            *out = mag == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)mag;
        }
    }
    return 1;
}

int mi_big_sign(const MiBig *value) {
    if (!value) return 0;
    return value->sign;
}

int mi_big_is_zero(const MiBig *value) {
    return !value || value->sign == 0;
}

/* ---------------------------------------------------------------- 运算 */

MiBig *mi_big_neg(const MiBig *a) {
    if (!a) return NULL;
    MiBig *out = mi_big_copy(a);
    if (out) out->sign = -out->sign;
    return out;
}

MiBig *mi_big_abs(const MiBig *a) {
    if (!a) return NULL;
    MiBig *out = mi_big_copy(a);
    if (out && out->sign < 0) out->sign = 1;
    return out;
}

MiBig *mi_big_add(const MiBig *a, const MiBig *b) {
    if (!a || !b) return NULL;
    if (a->sign == 0) return mi_big_copy(b);
    if (b->sign == 0) return mi_big_copy(a);
    if (a->sign == b->sign) {
        size_t n = 0;
        uint32_t *mag = mag_add(a->limbs, a->len, b->limbs, b->len, &n);
        return big_take_mag(mag, n, a->sign);
    }
    int cmp = mag_cmp(a->limbs, a->len, b->limbs, b->len);
    if (cmp == 0) return big_raw(0);
    size_t n = 0;
    if (cmp > 0) {
        uint32_t *mag = mag_sub(a->limbs, a->len, b->limbs, b->len, &n);
        return big_take_mag(mag, n, a->sign);
    }
    uint32_t *mag = mag_sub(b->limbs, b->len, a->limbs, a->len, &n);
    return big_take_mag(mag, n, b->sign);
}

MiBig *mi_big_sub(const MiBig *a, const MiBig *b) {
    if (!a || !b) return NULL;
    if (b->sign == 0) return mi_big_copy(a);
    if (a->sign == 0) return mi_big_neg(b);
    /* a - b = a + (-b)，直接展开可以少一次分配 */
    int bsign = -b->sign;
    if (a->sign == bsign) {
        size_t n = 0;
        uint32_t *mag = mag_add(a->limbs, a->len, b->limbs, b->len, &n);
        return big_take_mag(mag, n, a->sign);
    }
    int cmp = mag_cmp(a->limbs, a->len, b->limbs, b->len);
    if (cmp == 0) return big_raw(0);
    size_t n = 0;
    if (cmp > 0) {
        uint32_t *mag = mag_sub(a->limbs, a->len, b->limbs, b->len, &n);
        return big_take_mag(mag, n, a->sign);
    }
    uint32_t *mag = mag_sub(b->limbs, b->len, a->limbs, a->len, &n);
    return big_take_mag(mag, n, bsign);
}

MiBig *mi_big_mul(const MiBig *a, const MiBig *b) {
    if (!a || !b) return NULL;
    if (a->sign == 0 || b->sign == 0) return big_raw(0);
    size_t n = 0;
    uint32_t *mag = mag_mul(a->limbs, a->len, b->limbs, b->len, &n);
    return big_take_mag(mag, n, a->sign * b->sign);
}

int mi_big_cmp(const MiBig *a, const MiBig *b) {
    int sa = a ? a->sign : 0;
    int sb = b ? b->sign : 0;
    if (sa != sb) return sa < sb ? -1 : 1;
    if (sa == 0) return 0;
    int cmp = mag_cmp(a->limbs, a->len, b->limbs, b->len);
    return sa > 0 ? cmp : -cmp;
}

MiBig *mi_big_divmod(const MiBig *a, const MiBig *b, MiBig **rem) {
    if (rem) *rem = NULL;
    if (!a || !b || b->sign == 0) return NULL;   /* 除数为零 */
    if (a->sign == 0) {
        if (rem) *rem = big_raw(0);
        return big_raw(0);
    }
    uint32_t *qmag = NULL;
    uint32_t *rmag = NULL;
    size_t qn = 0;
    size_t rn = 0;
    mag_divmod(a->limbs, a->len, b->limbs, b->len, &qmag, &qn, &rmag, &rn);

    MiBig *quot;
    MiBig *rest;
    if (rn == 0) {
        /* 整除：商符号正常，余数为 0 */
        quot = big_take_mag(qmag, qn, a->sign * b->sign);
        free(rmag);
        rest = big_raw(0);
    } else if (a->sign == b->sign) {
        /* 同号：商为正，余数符号随除数 */
        quot = big_take_mag(qmag, qn, 1);
        rest = big_take_mag(rmag, rn, b->sign);
    } else {
        /* 异号：向下取整，商 = -(Q + 1)，余数 = 除数的符号 * (|b| - R)
         * 例：-7 // 2 -> Q = 3, R = 1 -> 商 -4，余数 +1 */
        uint64_t carry = 1;
        for (size_t i = 0; i < qn && carry; i++) {
            uint64_t cur = (uint64_t)qmag[i] + carry;
            qmag[i] = (uint32_t)cur;
            carry = cur >> 32;
        }
        if (carry) qmag[qn++] = 1;   /* qmag 容量为 a->len + 1，够用 */
        quot = big_take_mag(qmag, qn, -1);
        size_t dn = 0;
        uint32_t *dmag = mag_sub(b->limbs, b->len, rmag, rn, &dn);
        free(rmag);
        rest = big_take_mag(dmag, dn, b->sign);
    }
    if (rem) {
        *rem = rest;
    } else {
        mi_big_free(rest);
    }
    return quot;
}

MiBig *mi_big_pow(const MiBig *base, int64_t exponent) {
    if (!base || exponent < 0) return NULL;
    MiBig *result = mi_big_from_i64(1);
    MiBig *cur = mi_big_copy(base);
    uint64_t e = (uint64_t)exponent;
    while (e > 0) {
        if (e & 1u) {
            MiBig *next = mi_big_mul(result, cur);
            mi_big_free(result);
            result = next;
        }
        e >>= 1;
        if (e > 0) {
            MiBig *next = mi_big_mul(cur, cur);
            mi_big_free(cur);
            cur = next;
        }
    }
    mi_big_free(cur);
    return result;
}
