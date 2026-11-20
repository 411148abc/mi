"""Drive the MI-written interpreter from Python.

The self-hosted runtime lives in ``米/自举/*.mi``.  This module runs it with
the *Python* runtime (the bootstrap) and returns the same shape of outcome the
bootstrap produces, so callers can treat both runtimes uniformly:

    from 米.自举 import run
    result = run('show(1 + 2)')
    result.output   # ['3']
    result.code     # 0

Ordering note: the MI entry point is ``eval.mi`` → ``run_source``.  It returns
``{log, errors}``; this bridge maps that onto ``output`` / diagnostics.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..api import run_source as bootstrap_run
from ..errors import MiRuntimeError
from ..modules import VIRTUAL_ORIGINS, ModuleLoader

HERE = Path(__file__).resolve().parent
ENTRY = HERE / "eval.mi"
RUNTIME_FILES = (
    "tables.mi",
    "lexer.mi",
    "parser.mi",
    "values.mi",
    "builtins.mi",
    "json.mi",
    "diag.mi",
    "eval.mi",
)

DRIVER = """use "{entry}" as selfhost
~result: selfhost.run_source({source}, {origin})
show(json.text(value: result))
"""


@dataclass
class SelfHostOutcome:
    """Mirror of ``米.api.RunOutcome`` for the self-hosted runtime."""

    output: list[str] = field(default_factory=list)
    errors: list[dict[str, str]] = field(default_factory=list)
    error: MiRuntimeError | None = None

    @property
    def code(self) -> int:
        return 2 if self.error is not None else (1 if self.errors else 0)

    @property
    def ok(self) -> bool:
        return self.error is None and not self.errors

    def messages(self) -> list[str]:
        if self.error is not None:
            return [f"{self.error.code}: {self.error.message}"]
        return [f"{item.get('code', '?')}: {item.get('message', '')}" for item in self.errors]


def selfhost_available() -> bool:
    return all((HERE / name).is_file() for name in RUNTIME_FILES)


def runtime_files() -> list[Path]:
    return [HERE / name for name in RUNTIME_FILES]


def run(
    source: str,
    filename: str = "<source>",
    *,
    max_steps: int = 400_000_000,
    max_depth: int = 20_000,
) -> SelfHostOutcome:
    """Interpret ``source`` with the MI-written interpreter."""
    if not selfhost_available():
        raise MiRuntimeError("E0000", "the self-hosted runtime files are missing")
    # ``use "lib/x.mi"`` is relative to the file that contains it, so the driver
    # passes the importing file's path down to the MI side (``origin``).
    origin = "nothing" if filename in VIRTUAL_ORIGINS else json.dumps(filename, ensure_ascii=False)
    program = DRIVER.format(
        entry=ENTRY.as_posix(),
        source=json.dumps(source, ensure_ascii=False),
        origin=origin,
    )
    outcome = bootstrap_run(
        program,
        filename,
        loader=ModuleLoader(),
        max_steps=max_steps,
        max_depth=max_depth,
    )
    if outcome.error is not None:
        return SelfHostOutcome(error=outcome.error)
    fatals = [d for d in outcome.diagnostics if d.severity == "error"]
    if fatals:
        return SelfHostOutcome(
            errors=[{"code": d.code, "message": d.message} for d in fatals]
        )
    if not outcome.output:
        return SelfHostOutcome()
    try:
        payload = json.loads(outcome.output[0])
    except json.JSONDecodeError:  # pragma: no cover - defensive
        return SelfHostOutcome(output=list(outcome.output))
    errors: list[dict[str, Any]] = []
    for item in payload.get("errors", []):
        if isinstance(item, dict):
            errors.append(
                {"code": str(item.get("code", "E0000")), "message": str(item.get("message", ""))}
            )
    return SelfHostOutcome(output=list(payload.get("log", [])), errors=errors)
