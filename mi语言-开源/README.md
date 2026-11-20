# MI（米）— Signals, not statements / 信号，而非语句

MI 是一门小型的通用语言。同一份源码可以**原样**运行在四个独立实现上：

| 运行时 | 语言 | 怎么跑 |
|---|---|---|
| 参考运行时 | Python | `python -m 米 run f.mi` |
| 浏览器运行时 | JavaScript | 双击 `网页浏览器/index.html` |
| 自举运行时 | **MI** | `python -m 米 run f.mi --runtime selfhost` |
| 原生运行时 | **C** | `发行/mi-c.exe run f.mi` |

`发行/` 里是三个已经打包好的可执行文件，**下载即用，不需要装 Python**：

| 文件 | 是什么 | 体积 |
|---|---|---|
| `mi.exe` | 命令行解释器（含 REPL） | 15.7 MB |
| `mi-browser.exe` | 桌面版 Mi Browser（Tkinter 界面） | 15.7 MB |
| `mi-c.exe` | C 原生解释器（最轻，启动十几毫秒） | 0.38 MB |

## 五条法则

1. **没有赋值。** 状态只存在于 `wire`：`emit` 注入，`draw` 读取。`emit` 不是"赋值给左边的目标"，而是流向；同一个 `wire` 可以被多处覆盖。
2. **没有 `if`，没有 `for`。** 条件用门 `when`；重复用循环 `cycle`（计数 / while / until / over 四种形式）。
3. **值永不改变。** 数字、文本、列表、装配体都不可变；`push`、`place`、`sort` 返回新值。只有 `wire` 携带可变状态。
4. **站点有具名端口。** 参数是输入端口，`out` 是输出端口：0 个端口产生 `nothing`，1 个端口直接给出该值，2 个及以上产生字段为端口的装配体。
5. **拼写也是契约。** 每个关键字、运算符、字面量、内建函数都有多种等价写法（英文同义词、ASCII 符号、Unicode 字形）。编译器不偏好任何一种。

## 快速开始

```bash
# 用打包好的 exe（Windows，无需安装任何环境）
mi.exe run hello.mi
mi.exe                          # 进 REPL
mi-c.exe run hello.mi           # C 原生版

# 或者从源码跑（需要 Python 3.11+）
python -m 米 run hello.mi
python -m 米                      # REPL
python -m 米 check hello.mi       # 只做诊断，不执行
python -m 米 glyphs               # 列出全部拼写
```

退出码：`0` 成功，`1` 词法/语法/静态错误，`2` 运行时错误，`3` 用法错误。

### Hello, MI

```mi
station greet(name) {
    out value: "hello, " + name
}
show(greet("mi"))
```

### FizzBuzz

```mi
station 标签(n) {
    when n % 15 is 0 { out value: "FizzBuzz" }
    otherwise when n % 3 is 0 { out value: "Fizz" }
    otherwise when n % 5 is 0 { out value: "Buzz" }
    otherwise { out value: text(n) }
}
cycle over span(1, 101) as n { show(标签(n)) }
```

## 拼写示例

| 含义 | 写法 |
|---|---|
| 声明线 | `wire` `channel` `line` `~` |
| 注入 / 读取 | `emit` `send` `set` `<-` `<<` `⇒` / `draw` `read` `pull` `@` |
| 门 / 否则 | `when` `if` `gate` `?` `▷` / `otherwise` `else` `othw` `⁇` `\|:` |
| 站点 / 输出端口 | `station` `def` `fn` `func` `node` `§` `∫` / `out` `yield` `give` `↑` |
| 循环 | `cycle` `loop` `repeat` `↻` `⟳`；`over` `through`；`for` |
| 装配体 | `assembly` `record` `struct` `obj` `¤` |
| 比较 | `==` `is` `equals` / `<` `below` / `>=` `atleast` / `!=` `isnot` `≠` `<>` |
| 算术 | `+` `plus` / `*` `times` `×` / `//` `intdiv` / `%` `mod` `rem` / `**` `power` `^` |

完整拼写表见 [`规格/glyphs.json`](规格/glyphs.json)（**唯一真相**，四个运行时的拼写表
都由它生成），或运行 `python -m 米 glyphs`。语法细则见 [SPEC.md](SPEC.md) 与
[语法规则手册.md](语法规则手册.md)。

## 目录结构

```
├── SPEC.md                 规范（词法、EBNF、优先级、可选标点）
├── 语法规则手册.md           面向使用者的完整语法手册
├── 规格/glyphs.json         所有拼写的唯一真相
├── 米/                      Python 参考实现（词法→语法→检查→求值，CLI/REPL）
├── 米/自举/                 MI 自举解释器：用 MI 写的完整 MI 实现（8 模块，约 4475 行）
├── c/                      C 原生实现（约 9300 行）
├── 网页浏览器/               JavaScript 运行时 + 网页版 Mi Browser（自包含，双击即用）
├── hello.mi                最小可运行示例
├── 发行/                    三个可直接运行的可执行文件
├── 第三方许可证/             Python / Tcl-Tk / Pillow 的许可证全文（分发 exe 必需）
└── LICENSE                  MIT
```

## 一致性

四个独立实现必须对同一份源码给出逐字节相同的输出。验收用 63 条用例，它们
**已经打包进可执行文件**，不需要额外下载数据：

```bash
发行/mi-browser.exe --selftest               # 63/63（用例在 exe 里）
发行/mi.exe run f.mi --runtime selfhost      # MI 自举实现
发行/mi-c.exe run f.mi                       # C 原生实现
发行/mi-c.exe run f.mi --runtime selfhost    # C 当宿主，驱动 MI 自举实现
```

网页版双击 `网页浏览器/index.html` 后点页内「自测」，跑的是同一批用例
（它们编译在 `网页浏览器/js/conformance.js` 里）。

## 构建 C 版

需要一个 C 编译器（gcc / clang / MSVC，或 `pip install ziglang`）：

```bash
zig cc -std=c11 -O2 -o mi-c.exe c/main.c c/util.c c/value.c c/bigint.c ^
      c/lexer.c c/parser.c c/checker.c c/eval.c -lwinhttp -lws2_32
```

Python 版与网页版不需要构建。`c/tables.h` 是由 `规格/glyphs.json` 生成的拼写表，
已随源码提供。

## 许可证

MI 项目自身代码以 [MIT](LICENSE) 发布。

`发行/mi.exe` 与 `发行/mi-browser.exe` 是 PyInstaller 打包产物，内含 Python、
Tcl/Tk、Pillow 与 PyInstaller bootloader。它们各自的许可证全文在
[`第三方许可证/`](第三方许可证/)，分发这两个 exe 时必须一并保留。
