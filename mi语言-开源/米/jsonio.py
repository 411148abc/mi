"""JSON <-> MI value conversion.

Shared by the ``json.parse`` / ``json.text`` builtins and by the Python host's
persistent key-value store.

MI mapping: object -> assembly, array -> list, number -> number, string -> text,
true/false -> flag, null -> nothing.  Only those MI kinds can be encoded.
"""
from __future__ import annotations

import json
from typing import Any

from .values import NOTHING, Assembly, MiValueError, kind


def from_json(value: Any) -> Any:
    """Convert a value produced by ``json.loads`` into an MI value."""
    if value is None:
        return NOTHING
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return tuple(from_json(item) for item in value)
    if isinstance(value, dict):
        return Assembly.from_pairs(
            [(str(key), from_json(item)) for key, item in value.items()]
        )
    raise MiValueError(f"cannot convert {type(value).__name__} to an MI value")


def to_json(value: Any) -> Any:
    """Convert an MI value into something ``json.dumps`` accepts."""
    if value is NOTHING:
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value
    if isinstance(value, tuple):
        return [to_json(item) for item in value]
    if isinstance(value, Assembly):
        return {name: to_json(item) for name, item in value.fields}
    raise MiValueError(
        f"cannot write {kind(value)} to JSON; only number, text, flag, nothing, list and assembly"
    )


def parse_json(text: str) -> Any:
    try:
        loaded = json.loads(text)
    except json.JSONDecodeError as exc:
        raise MiValueError(f"invalid JSON: {exc.msg} at line {exc.lineno} column {exc.colno}")
    return from_json(loaded)


def encode_json(value: Any) -> str:
    try:
        return json.dumps(to_json(value), ensure_ascii=False, separators=(",", ":"))
    except (TypeError, ValueError) as exc:  # pragma: no cover - defensive
        raise MiValueError(f"cannot serialise this value as JSON: {exc}")


def dump_pretty(value: Any) -> str:
    """Human-readable JSON, used by the forum demo (optional helper)."""
    return json.dumps(to_json(value), ensure_ascii=False, indent=2)
