"""High-level API used by the CLI, the tests and the conformance runner."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .ast import Program
from .checker import check
from .errors import Diagnostic, MiRuntimeError
from .evaluator import ExecutionResult, execute
from .glyphs import TABLE, GlyphTable
from .宿主.python_host import IMPLS as PYTHON_HOST
from .modules import ModuleLoader
from .parser import parse


@dataclass
class CompileResult:
    program: Program
    diagnostics: list[Diagnostic] = field(default_factory=list)

    @property
    def has_errors(self) -> bool:
        return any(d.severity == "error" for d in self.diagnostics)

    @property
    def errors(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if d.severity == "error"]

    @property
    def warnings(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if d.severity == "warning"]


@dataclass
class RunOutcome:
    output: list[str] = field(default_factory=list)
    diagnostics: list[Diagnostic] = field(default_factory=list)
    error: MiRuntimeError | None = None

    @property
    def ok(self) -> bool:
        return self.error is None and not any(
            d.severity == "error" for d in self.diagnostics
        )


def compile_source(
    source: str,
    filename: str = "<source>",
    *,
    loader: ModuleLoader | None = None,
    table: GlyphTable = TABLE,
) -> CompileResult:
    program, diagnostics = parse(source, filename, table)
    diagnostics = diagnostics + check(
        program, filename=filename, loader=loader, table=table
    )
    ordered = sorted(
        diagnostics,
        key=lambda d: (0 if d.severity == "error" else 1, d.span.start.offset),
    )
    return CompileResult(program, ordered)


def run_source(
    source: str,
    filename: str = "<source>",
    *,
    loader: ModuleLoader | None = None,
    hosts: dict[str, Any] | None = None,
    host_name: str = "python",
    argv: list[str] | None = None,
    max_steps: int = 5_000_000,
    max_depth: int = 400,
    on_output: Any = None,
    table: GlyphTable = TABLE,
) -> RunOutcome:
    compiled = compile_source(source, filename, loader=loader, table=table)
    if compiled.has_errors:
        return RunOutcome([], compiled.diagnostics, None)
    if hosts is None and host_name == "python":
        hosts = PYTHON_HOST
    result: ExecutionResult = execute(
        compiled.program,
        filename=filename,
        loader=loader,
        hosts=hosts,
        host_name=host_name,
        argv=argv,
        max_steps=max_steps,
        max_depth=max_depth,
        on_output=on_output,
    )
    return RunOutcome(result.output, compiled.diagnostics, result.error)
