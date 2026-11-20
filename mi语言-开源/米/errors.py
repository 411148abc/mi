"""Diagnostics, source frames and runtime errors for MI."""
from __future__ import annotations

import difflib
from dataclasses import dataclass, field
from typing import Iterable, Sequence

from .tokens import Span


@dataclass
class Diagnostic:
    severity: str
    code: str
    message: str
    span: Span
    hint: str | None = None
    filename: str | None = None

    def render(self, source: str, filename: str | None = None) -> str:
        return render_diagnostic(self, source, filename or self.filename or "<source>")


@dataclass
class TraceFrame:
    station: str
    span: Span | None = None


class MiRuntimeError(Exception):
    """A runtime fault, carrying an MI error code and a call trace."""

    def __init__(
        self,
        code: str,
        message: str,
        span: Span | None = None,
        *,
        hint: str | None = None,
        trace: Sequence[TraceFrame] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.span = span
        self.hint = hint
        self.trace: list[TraceFrame] = list(trace or [])

    @property
    def severity(self) -> str:
        return "runtime error"


class ModuleError(Exception):
    """A module could not be loaded; carries diagnostics for the importer."""

    def __init__(self, diagnostics: Sequence[Diagnostic]) -> None:
        super().__init__("module error")
        self.diagnostics = list(diagnostics)


@dataclass
class DiagnosticBag:
    diagnostics: list[Diagnostic] = field(default_factory=list)
    filename: str | None = None

    def error(self, code: str, message: str, span: Span, hint: str | None = None) -> None:
        self.diagnostics.append(
            Diagnostic("error", code, message, span, hint, self.filename)
        )

    def warning(
        self, code: str, message: str, span: Span, hint: str | None = None
    ) -> None:
        self.diagnostics.append(
            Diagnostic("warning", code, message, span, hint, self.filename)
        )

    def extend(self, others: Iterable[Diagnostic]) -> None:
        self.diagnostics.extend(others)

    @property
    def has_errors(self) -> bool:
        return any(d.severity == "error" for d in self.diagnostics)

    def errors(self) -> list[Diagnostic]:
        return [d for d in self.diagnostics if d.severity == "error"]

    def sorted(self) -> list[Diagnostic]:
        rank = {"error": 0, "warning": 1}
        return sorted(
            self.diagnostics, key=lambda d: (rank.get(d.severity, 2), d.span.start.offset)
        )


def suggest(
    word: str, candidates: Sequence[str], n: int = 3, cutoff: float = 0.6
) -> list[str]:
    """Closest candidate spellings for a typo, or an empty list."""
    if not word:
        return []
    lowered = [c.lower() for c in candidates]
    matches = difflib.get_close_matches(word.lower(), lowered, n=n, cutoff=cutoff)
    if not matches:
        relaxed = max(0.4, cutoff - 0.1)
        matches = difflib.get_close_matches(word.lower(), lowered, n=n, cutoff=relaxed)
    # map back to the original casing of the candidate
    by_lower = {c.lower(): c for c in candidates}
    return [by_lower[m] for m in matches]


def _line_text(source: str, line: int) -> str:
    lines = source.splitlines()
    if 1 <= line <= len(lines):
        return lines[line - 1]
    return ""


def render_diagnostic(diag: Diagnostic, source: str, filename: str) -> str:
    start = diag.span.start
    end = diag.span.end
    line_text = _line_text(source, start.line)
    gutter = len(str(max(start.line, end.line)))
    caret_len = max(1, end.col - start.col) if end.line == start.line else max(1, len(line_text) - start.col + 1)
    caret = " " * (start.col - 1) + "^" * caret_len
    header = f'{diag.severity}[{diag.code}] {diag.message}'
    parts = [
        header,
        f" --> {filename}:{start.line}:{start.col}",
        f" {' ' * gutter} |",
        f" {start.line:>{gutter}} | {line_text}",
        f" {' ' * gutter} | {caret}",
    ]
    if diag.hint:
        parts.append(f" {' ' * gutter} = hint: {diag.hint}")
    return "\n".join(parts)


def render_runtime_error(
    err: MiRuntimeError, source: str, filename: str
) -> str:
    lines = [f"runtime error[{err.code}] {err.message}"]
    if err.span is not None:
        start = err.span.start
        end = err.span.end
        line_text = _line_text(source, start.line)
        gutter = len(str(start.line))
        caret_len = max(1, end.col - start.col) if end.line == start.line else 1
        lines[0] = f"runtime error[{err.code}] {err.message}"
        lines.append(f" --> {filename}:{start.line}:{start.col}")
        lines.append(f" {' ' * gutter} |")
        lines.append(f" {start.line:>{gutter}} | {line_text}")
        lines.append(f" {' ' * gutter} | {' ' * (start.col - 1)}{'^' * caret_len}")
    if err.hint:
        lines.append(f" hint: {err.hint}")
    for frame in reversed(err.trace):
        if frame.span is not None:
            lines.append(
                f" in station `{frame.station}` called at "
                f"{filename}:{frame.span.start.line}:{frame.span.start.col}"
            )
        else:
            lines.append(f" in station `{frame.station}`")
    return "\n".join(lines)


def render_all(
    diagnostics: Sequence[Diagnostic], source: str, filename: str
) -> str:
    return "\n\n".join(d.render(source, filename) for d in diagnostics)
