/* bigint.h — MI 语言 C 实现的任意精度整数（bignum）模块。
 *
 * 设计要点：
 *   - 对齐 Python 的 int 语义：无上限、整除向下取整、取模符号随除数。
 *   - 对象不可变：所有运算都返回新对象，参数不被修改。
 *   - 调用方拥有返回的对象，用完必须 mi_big_free。
 *   - 除数为 0、字符串非法、NaN/Inf 等无效输入返回 NULL。
 *
 * 仅依赖 C11 标准库，不依赖 mi.h，可以单独编译与测试。
 * 内存不足时与项目其它模块一致：报告后退出（exit 70），不做错误传播。
 */
#ifndef MI_BIGINT_H
#define MI_BIGINT_H
#include <stddef.h>
#include <stdint.h>

typedef struct MiBig MiBig;

/* 构造：调用方拥有返回值，用完必须 mi_big_free */
MiBig *mi_big_from_i64(int64_t value);
MiBig *mi_big_from_str(const char *text);          /* 十进制，允许前导 -/+，非法返回 NULL */
MiBig *mi_big_from_double(double value);           /* 向零截断；NaN/Inf 返回 NULL */
MiBig *mi_big_copy(const MiBig *value);
void   mi_big_free(MiBig *value);

/* 转换 */
char  *mi_big_to_str(const MiBig *value);          /* 十进制，调用方 free */
double mi_big_to_double(const MiBig *value);
int    mi_big_fits_i64(const MiBig *value, int64_t *out);   /* 1 = 能放进 int64 */
int    mi_big_sign(const MiBig *value);            /* -1 / 0 / 1 */

/* 运算：都返回**新对象**（调用方拥有），参数不被修改 */
MiBig *mi_big_add(const MiBig *a, const MiBig *b);
MiBig *mi_big_sub(const MiBig *a, const MiBig *b);
MiBig *mi_big_mul(const MiBig *a, const MiBig *b);
MiBig *mi_big_neg(const MiBig *a);
MiBig *mi_big_abs(const MiBig *a);
int    mi_big_cmp(const MiBig *a, const MiBig *b);   /* -1 / 0 / 1 */
int    mi_big_is_zero(const MiBig *value);

/* 除法：必须与 Python 语义一致
 *   floor 除法：a // b，结果向下取整（-7 // 2 == -4）
 *   mod：a % b，结果符号随除数（-7 % 2 == 1，7 % -2 == -1）
 * 返回商，*rem 写入余数（可为 NULL）。除数为 0 时返回 NULL。 */
MiBig *mi_big_divmod(const MiBig *a, const MiBig *b, MiBig **rem);
MiBig *mi_big_pow(const MiBig *base, int64_t exponent);   /* exponent >= 0；负数返回 NULL */
#endif
