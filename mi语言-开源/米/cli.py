"""MI command line interface.

    python -m 米 run FILE.mi [--max-steps N] [-- args...]
    python -m 米 check FILE.mi
    python -m 米 ast FILE.mi
    python -m 米 glyphs [--json]
    python -m 米 repl
    python -m 米 version

Exit codes: 0 success, 1 lexical/syntax/static error, 2 runtime error,
3 usage error.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from . import __version__, ast as mi_ast
from .api import compile_source, run_source
from .errors import DiagnosticBag, MiRuntimeError, render_all, render_runtime_error
from .evaluator import Frame, Runtime
from .glyphs import SPEC_PATH, TABLE, GlyphTable
from .宿主.python_host import IMPLS as PYTHON_HOST
from .modules import ModuleLoader
from .values import NOTHING, display

def _program() -> str:
    """当前是怎么被调用的：冻结成 exe 时是 `mi`，从源码跑时是 `python -m 米`。"""
    if getattr(sys, "frozen", False):
        return "mi"
    return "python -m 米"


USAGE_TEMPLATE = """MI — Signals, not statements.

usage:
  {p}                          # 双击也行：进入交互式 REPL
  {p} 文件.mi [--runtime python|selfhost]   # 拖到图标上 / 直接给文件
  {p} run 文件.mi [--runtime python|selfhost] [--max-steps N] [-- args...]
  {p} check 文件.mi [--runtime python|selfhost]
  {p} ast 文件.mi
  {p} glyphs [--json]
  {p} repl
  {p} browser [--open URL] [--source FILE] [--store FILE] [--selftest]
                       [--mode auto|app|dev] [--fullscreen] [--size WxH]
  {p} version
  {p} runtimes

运行 `{p} glyphs` 可查看每个关键字与内置函数的全部拼写。
`--runtime` 选择解释器实现：python = 参考实现（默认），selfhost = 用 MI 自己写的实现。
"""


def usage_text() -> str:
    return USAGE_TEMPLATE.replace("{p}", _program())


def _utf8() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(encoding="utf-8")
            except (ValueError, OSError):  # pragma: no cover
                pass


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def _render(diagnostics: list, sources: dict[str, str]) -> None:
    for diag in diagnostics:
        filename = diag.filename or "<source>"
        source = sources.get(filename, sources.get("<source>", ""))
        print(diag.render(source, filename), file=sys.stderr)


RUNTIMES = ("python", "selfhost")


def _split_runtime(args: list[str]) -> tuple[str, list[str]]:
    """Pull `--runtime python|selfhost` out of an argument list."""
    runtime = "python"
    rest: list[str] = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "--runtime":
            index += 1
            if index >= len(args):
                raise ValueError("--runtime needs `python` or `selfhost`")
            runtime = args[index]
        elif arg.startswith("--runtime="):
            runtime = arg.split("=", 1)[1]
        else:
            rest.append(arg)
        index += 1
    if runtime not in RUNTIMES:
        raise ValueError(f"unknown runtime `{runtime}`; pick one of {', '.join(RUNTIMES)}")
    return runtime, rest


def _run_selfhost(path: Path, source: str, max_steps: int) -> int:
    """Run one file with the MI-written interpreter."""
    from .自举 import run as selfhost_run

    outcome = selfhost_run(source, str(path), max_steps=max_steps)
    for line in outcome.output:
        print(line, flush=True)
    for message in outcome.messages():
        print(message, file=sys.stderr)
    if outcome.error is not None:
        return 2
    return 1 if outcome.errors else 0


def cmd_run(args: list[str]) -> int:
    try:
        runtime, args = _split_runtime(args)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3
    if not args:
        print("error: `run` needs a file", file=sys.stderr)
        return 3
    path = Path(args[0])
    max_steps = 5_000_000
    rest = args[1:]
    script_args: list[str] = []
    index = 0
    while index < len(rest):
        arg = rest[index]
        if arg == "--":
            script_args = rest[index + 1 :]
            break
        if arg == "--max-steps":
            index += 1
            if index >= len(rest):
                print("error: --max-steps needs a number", file=sys.stderr)
                return 3
            try:
                max_steps = int(rest[index])
            except ValueError:
                print("error: --max-steps needs a number", file=sys.stderr)
                return 3
        elif arg.startswith("--max-steps="):
            try:
                max_steps = int(arg.split("=", 1)[1])
            except ValueError:
                print("error: --max-steps needs a number", file=sys.stderr)
                return 3
        else:
            script_args.append(arg)
        index += 1
    if not path.exists():
        print(f"error: no such file: {path}", file=sys.stderr)
        return 3
    source = _read(path)
    if runtime == "selfhost":
        return _run_selfhost(path, source, max_steps)
    sources = {str(path): source}
    loader = ModuleLoader()
    outcome = run_source(
        source,
        str(path),
        loader=loader,
        hosts=PYTHON_HOST,
        argv=script_args,
        max_steps=max_steps,
        on_output=lambda line: print(line, flush=True),
    )
    for module_path, module in loader.cache.items():
        try:
            sources[module_path] = Path(module_path).read_text(encoding="utf-8-sig")
        except OSError:  # pragma: no cover
            pass
    _render(outcome.diagnostics, sources)
    if outcome.error is not None:
        print(
            render_runtime_error(outcome.error, sources.get(str(path), source), str(path)),
            file=sys.stderr,
        )
        return 2
    if any(d.severity == "error" for d in outcome.diagnostics):
        return 1
    return 0


def cmd_check(args: list[str]) -> int:
    try:
        runtime, args = _split_runtime(args)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3
    if not args:
        print("error: `check` needs a file", file=sys.stderr)
        return 3
    path = Path(args[0])
    if not path.exists():
        print(f"error: no such file: {path}", file=sys.stderr)
        return 3
    source = _read(path)
    if runtime == "selfhost":
        from .自举 import run as selfhost_run

        outcome = selfhost_run(source, str(path))
        for message in outcome.messages():
            print(message, file=sys.stderr)
        return 1 if not outcome.ok else 0
    sources = {str(path): source}
    loader = ModuleLoader()
    compiled = compile_source(source, str(path), loader=loader)
    for module_path in loader.cache:
        try:
            sources[module_path] = Path(module_path).read_text(encoding="utf-8-sig")
        except OSError:  # pragma: no cover
            pass
    _render(compiled.diagnostics, sources)
    return 1 if compiled.has_errors else 0


def cmd_ast(args: list[str]) -> int:
    if not args:
        print("error: `ast` needs a file", file=sys.stderr)
        return 3
    path = Path(args[0])
    if not path.exists():
        print(f"error: no such file: {path}", file=sys.stderr)
        return 3
    source = _read(path)
    compiled = compile_source(source, str(path))
    _render(compiled.diagnostics, {str(path): source})
    print(json.dumps(mi_ast.to_dict(compiled.program), ensure_ascii=False, indent=2))
    return 1 if compiled.has_errors else 0


def cmd_glyphs(args: list[str]) -> int:
    if "--json" in args:
        data = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
        print(json.dumps(data, ensure_ascii=False, indent=2))
        return 0
    print(f"MI {__version__} — every spelling (canonical first)")
    print()
    groups: dict[str, list[tuple[str, str, str]]] = {}
    for group, canonical, spellings, summary in TABLE.all_spelling_rows():
        groups.setdefault(group, []).append((canonical, spellings, summary))
    for group in ("keyword", "operator", "literal", "builtin"):
        print(f"[{group}s]")
        width = max((len(row[0]) for row in groups.get(group, [])), default=4)
        for canonical, spellings, summary in groups.get(group, []):
            print(f"  {canonical:<{width}}  {'  '.join(spellings.split())}")
            if summary:
                print(f"  {'':<{width}}  {summary}")
        print()
    print("Every spelling is equivalent; pick the style you like.")
    return 0


def _incomplete(source: str, diagnostics: list) -> bool:
    for diag in diagnostics:
        if diag.code == "E0102" and (
            "missing `}`" in diag.message
            or "missing `end`" in diag.message
            or "missing `)`" in diag.message
            or "missing `]" in diag.message
            or "missing `⟩" in diag.message
        ):
            return True
        if diag.code == "E0002" and "unterminated" in diag.message:
            return True
    return False


def cmd_repl(args: list[str]) -> int:
    from .modules import Module

    print(f"MI {__version__} — Signals, not statements.")
    print("输入 MI 代码回车执行；`:quit` 退出；`:help` 看语法速查。")
    loader = ModuleLoader()
    runtime = Runtime(
        hosts=PYTHON_HOST,
        loader=loader,
        on_output=lambda line: print(line, flush=True),
    )
    module = Module(path="<repl>", name="repl", program=mi_ast.Program(_zero_span(), []))
    frame = Frame(module)
    session = ""
    session_items = 0
    buffer = ""
    prompt = "mi> "
    while True:
        try:
            line = input(prompt)
        except EOFError:
            print()
            break
        if not buffer and line.strip() in (":quit", ":q", "quit", "exit"):
            break
        if not buffer and line.strip() in (":help", ":h", ":?"):
            print(usage_text())
            continue
        buffer += line + "\n"
        candidate = session + buffer
        compiled = compile_source(candidate, "<repl>", loader=loader)
        if _incomplete(candidate, compiled.diagnostics) and line.strip() != "":
            prompt = "... "
            continue
        prompt = "mi> "
        if compiled.diagnostics:
            for diag in compiled.diagnostics:
                if diag.code.startswith("W"):
                    continue  # keep the interactive session quiet
                print(diag.render(candidate, "<repl>"), file=sys.stderr)
        if compiled.has_errors:
            buffer = ""
            continue
        items = compiled.program.items[session_items:]
        try:
            for item in items:
                if isinstance(item, mi_ast.Station):
                    module.stations[item.name] = item
                elif isinstance(item, mi_ast.Use):
                    child = loader.load(item.path, "<repl>")
                    if item.alias:
                        module.imports[item.alias] = child
                    else:
                        module.flat.append(child)
                elif isinstance(item, mi_ast.Flow):
                    value = runtime.eval(item.expr, frame)
                    if value is not NOTHING:
                        print(display(value))
                else:
                    runtime.exec_flow(item, frame)
        except MiRuntimeError as err:
            print(render_runtime_error(err, candidate, "<repl>"), file=sys.stderr)
        session = candidate
        session_items = len(compiled.program.items)
        buffer = ""
    return 0


def _zero_span():
    from .tokens import Position, Span

    return Span(Position(1, 1, 0), Position(1, 1, 0))


def cmd_version(args: list[str]) -> int:
    print(f"MI {__version__} — Signals, not statements.")
    # 只在用户显式想看实现细节时才提 Python：用 --verbose
    if getattr(sys, "frozen", False):
        print("build: 独立可执行文件（无需安装其他环境）")
    elif "--verbose" in args or "-v" in args:
        print(f"build: 源码运行 · CPython {sys.version.split()[0]}")
    else:
        print("build: 源码运行")
    print(
        f"spellings: {len(TABLE.keywords)} 组关键字 · {len(TABLE.operators)} 个运算符 · "
        f"{len(TABLE.builtins)} 个内置函数"
    )
    print("runtimes: python（参考实现，默认）· selfhost（用 MI 自己写的实现）")
    return 0


def cmd_runtimes(args: list[str]) -> int:
    """List the available runtimes and their files."""
    from .自举 import runtime_files, selfhost_available

    print("python    参考实现（默认运行时）")
    if selfhost_available():
        total = sum(len(path.read_text(encoding="utf-8").splitlines()) for path in runtime_files())
        print(f"selfhost  用 MI 自己写的实现（米/自举/*.mi，共 {total} 行）")
        for path in runtime_files():
            lines = len(path.read_text(encoding="utf-8").splitlines())
            print(f"          {path.name:14} {lines:5d} 行")
    else:
        print("selfhost  未安装")
    return 0


def cmd_browser(args: list[str]) -> int:
    from .浏览器.tk_app import main as browser_main

    return browser_main(args)


def _looks_like_source_file(token: str) -> bool:
    """该参数是不是一个 `.mi` 文件路径（用于支持把文件拖到 exe 图标上）。"""
    if token.startswith("-"):
        return False
    lowered = token.lower()
    if lowered.endswith(".mi"):
        return True
    # 拖放时资源管理器给的是绝对路径；没有扩展名的路径也按文件处理
    return (Path(token).suffix == "") and ("/" in token or "\\" in token) and Path(token).is_file()


def main(argv: list[str] | None = None) -> int:
    _utf8()
    args = list(sys.argv[1:] if argv is None else argv)

    # 双击启动（无参数）：直接进 REPL，而不是打印用法后秒退
    if not args:
        return cmd_repl([])

    if args[0] in ("-h", "--help", "help"):
        print(usage_text())
        return 0

    # 拖放到 exe 图标：第一个参数就是 .mi 文件 ⇒ 等价 `run 该文件`
    if _looks_like_source_file(args[0]):
        return cmd_run(args)

    command = args[0]
    rest = args[1:]
    dispatch = {
        "run": cmd_run,
        "check": cmd_check,
        "ast": cmd_ast,
        "glyphs": cmd_glyphs,
        "repl": cmd_repl,
        "browser": cmd_browser,
        "runtimes": cmd_runtimes,
        "version": cmd_version,
        "--version": cmd_version,
        "-V": cmd_version,
    }
    handler = dispatch.get(command)
    if handler is None:
        print(f"error: unknown command `{command}`", file=sys.stderr)
        print(usage_text(), file=sys.stderr)
        return 3
    try:
        return handler(rest)
    except BrokenPipeError:  # pragma: no cover
        return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
