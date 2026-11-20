"""Conformance self-test for the Python edition of Mi Browser.

Runs the same ``测试/一致性/*.json`` cases that the pytest suite and the
JavaScript edition run, so the "self-test" tab in the desktop browser reports
the same number.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from ..api import run_source
from ..modules import ModuleLoader, VirtualModuleLoader

ROOT = Path(__file__).resolve().parents[2]


@dataclass
class SelfTestResult:
    passed: int = 0
    total: int = 0
    failures: list[str] = field(default_factory=list)

    @property
    def summary(self) -> str:
        if self.total == 0:
            return "no conformance cases found"
        text = f"{self.passed}/{self.total} passed"
        if self.failures:
            text += f" · {len(self.failures)} failed"
        return text


def cases_directory() -> Path:
    return ROOT / "测试" / "一致性"


def _error_pairs(outcome) -> list[tuple[str, str]]:
    pairs = [
        (diag.code, diag.message)
        for diag in outcome.diagnostics
        if diag.severity == "error"
    ]
    if outcome.error is not None:
        pairs.append((outcome.error.code, outcome.error.message))
    return pairs


def run_selftest(directory: Path | None = None) -> SelfTestResult:
    cases_dir = directory or cases_directory()
    result = SelfTestResult()
    if not cases_dir.is_dir():
        return result
    for path in sorted(cases_dir.glob("*.json")):
        try:
            case = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:  # pragma: no cover
            result.total += 1
            result.failures.append(f"{path.name}: cannot read case ({exc})")
            continue
        result.total += 1
        name = case.get("name", path.stem)
        files = case.get("files")
        if files:
            loader = VirtualModuleLoader(files, root=str(ROOT))
            filename = "main.mi"
        else:
            loader = ModuleLoader()
            filename = "<selftest>"
        outcome = run_source(case["source"], filename, loader=loader)
        expect = case.get("expect", {})
        expected_output = expect.get("output", [])
        if outcome.output != expected_output:
            result.failures.append(
                f"{name}: output {outcome.output!r} != {expected_output!r}"
            )
            continue
        expected_errors = expect.get("errors", [])
        actual = _error_pairs(outcome)
        if len(actual) < len(expected_errors):
            result.failures.append(
                f"{name}: expected {len(expected_errors)} error(s), got {actual!r}"
            )
            continue
        mismatch = None
        for want, got in zip(expected_errors, actual):
            want_code = want["code"]
            want_contains = want.get("contains", "")
            got_code, got_message = got
            if got_code != want_code:
                mismatch = f"{name}: error code {got_code} != {want_code} ({got_message})"
                break
            if want_contains and want_contains.lower() not in got_message.lower():
                mismatch = f"{name}: error message {got_message!r} lacks {want_contains!r}"
                break
        if mismatch is not None:
            result.failures.append(mismatch)
            continue
        if not expected_errors and actual:
            result.failures.append(f"{name}: unexpected errors {actual!r}")
            continue
        result.passed += 1
    return result
