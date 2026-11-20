"""Runtime values, equality, truthiness, display and operators for MI.

All MI values are immutable.  Lists are tuples, assemblies are ordered tuples
of ``(name, value)`` pairs, stations and builtins are callable handles.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

Kind = str


class MiValueError(Exception):
    """An operation on values failed; the evaluator turns it into E0300."""

    def __init__(self, message: str, code: str = "E0300") -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass(frozen=True)
class Assembly:
    fields: tuple[tuple[str, Any], ...] = ()

    @staticmethod
    def from_pairs(pairs: list[tuple[str, Any]]) -> "Assembly":
        ordered: list[tuple[str, Any]] = []
        index: dict[str, int] = {}
        for name, value in pairs:
            if name in index:
                ordered[index[name]] = (name, value)
            else:
                index[name] = len(ordered)
                ordered.append((name, value))
        return Assembly(tuple(ordered))

    def get(self, name: str) -> Any:
        for key, value in self.fields:
            if key == name:
                return value
        raise KeyError(name)

    def has(self, name: str) -> bool:
        return any(key == name for key, _ in self.fields)

    def keys(self) -> tuple[str, ...]:
        return tuple(key for key, _ in self.fields)

    def with_field(self, name: str, value: Any) -> "Assembly":
        return Assembly.from_pairs(list(self.fields) + [(name, value)])

    def __len__(self) -> int:
        return len(self.fields)


@dataclass(frozen=True)
class StationValue:
    module: Any
    station: Any

    @property
    def name(self) -> str:
        return self.station.name


@dataclass(frozen=True)
class BuiltinValue:
    name: str


@dataclass(frozen=True)
class NothingValue:
    """The single `nothing` value (compared by identity)."""

    def __repr__(self) -> str:  # pragma: no cover
        return "nothing"


NOTHING = NothingValue()


def kind(value: Any) -> Kind:
    if value is NOTHING or value is None:
        return "nothing"
    if isinstance(value, bool):
        return "flag"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "text"
    if isinstance(value, tuple):
        return "list"
    if isinstance(value, Assembly):
        return "assembly"
    if isinstance(value, StationValue):
        return "station"
    if isinstance(value, BuiltinValue):
        return "builtin"
    return "unknown"  # pragma: no cover


def truthy(value: Any) -> bool:
    if value is NOTHING or value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return len(value) > 0
    if isinstance(value, (tuple, Assembly)):
        return len(value) > 0
    return True


def _number_text(value: int | float) -> str:
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if value != value:  # NaN
            return "nan"
        if value in (float("inf"), float("-inf")):
            return "inf" if value > 0 else "-inf"
        if value.is_integer() and abs(value) < 1e16:
            return str(int(value))
        return repr(value)
    return str(value)  # pragma: no cover


def _quote(text: str) -> str:
    out = ['"']
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def display(value: Any, nested: bool = False) -> str:
    if value is NOTHING or value is None:
        return "nothing"
    if isinstance(value, bool):
        return "on" if value else "off"
    if isinstance(value, (int, float)):
        return _number_text(value)
    if isinstance(value, str):
        return _quote(value) if nested else value
    if isinstance(value, tuple):
        return "[" + ", ".join(display(item, True) for item in value) + "]"
    if isinstance(value, Assembly):
        return (
            "assembly{"
            + ", ".join(f"{name}: {display(item, True)}" for name, item in value.fields)
            + "}"
        )
    if isinstance(value, StationValue):
        return f"station:{value.name}"
    if isinstance(value, BuiltinValue):
        return f"builtin:{value.name}"
    return str(value)  # pragma: no cover


def text_of(value: Any) -> str:
    return display(value)


def equal(a: Any, b: Any) -> bool:
    if kind(a) != kind(b):
        # numbers of different python types still compare numerically
        if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and not isinstance(b, bool):
            return a == b
        return False
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b or a == b
    if isinstance(a, float) or isinstance(b, float):
        return a == b
    if isinstance(a, Assembly) and isinstance(b, Assembly):
        if len(a.fields) != len(b.fields):
            return False
        return all(
            ka == kb and equal(va, vb)
            for (ka, va), (kb, vb) in zip(a.fields, b.fields)
        )
    if isinstance(a, tuple) and isinstance(b, tuple):
        return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
    if isinstance(a, StationValue) and isinstance(b, StationValue):
        return a is b
    return a == b


def _merge_assemblies(left: Assembly, right: Assembly) -> Assembly:
    return Assembly.from_pairs(list(left.fields) + list(right.fields))


def binary(op: str, a: Any, b: Any) -> Any:
    if op == "PLUS":
        if isinstance(a, bool) or isinstance(b, bool):
            raise MiValueError("cannot add flags")
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return a + b
        if isinstance(a, str) and isinstance(b, str):
            return a + b
        if isinstance(a, tuple) and isinstance(b, tuple):
            return a + b
        if isinstance(a, Assembly) and isinstance(b, Assembly):
            return _merge_assemblies(a, b)
        raise MiValueError(
            f"cannot add {kind(a)} and {kind(b)}"
        )
    if op == "MINUS":
        if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and not isinstance(b, bool):
            return a - b
        raise MiValueError(f"cannot subtract {kind(b)} from {kind(a)}")
    if op == "TIMES":
        if isinstance(a, bool) or isinstance(b, bool):
            raise MiValueError("cannot multiply flags")
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return a * b
        if isinstance(a, str) and isinstance(b, int):
            return a * b
        if isinstance(a, tuple) and isinstance(b, int):
            return a * b
        if isinstance(a, int) and isinstance(b, str):
            return b * a
        if isinstance(a, int) and isinstance(b, tuple):
            return b * a
        raise MiValueError(f"cannot multiply {kind(a)} by {kind(b)}")
    if op in ("DIV", "IDIV", "MOD"):
        if not (
            isinstance(a, (int, float))
            and isinstance(b, (int, float))
            and not isinstance(a, bool)
            and not isinstance(b, bool)
        ):
            raise MiValueError(f"cannot divide {kind(a)} by {kind(b)}")
        if b == 0:
            raise MiValueError("division by zero", code="E0301")
        if op == "DIV":
            return a / b
        if op == "IDIV":
            result = a // b
            if isinstance(result, float) and result.is_integer():
                return int(result)
            return result
        return a % b
    if op == "POW":
        if not (
            isinstance(a, (int, float))
            and isinstance(b, (int, float))
            and not isinstance(a, bool)
            and not isinstance(b, bool)
        ):
            raise MiValueError(f"cannot raise {kind(a)} to a power")
        try:
            return a**b
        except (OverflowError, ValueError) as exc:
            raise MiValueError(f"power failed: {exc}") from exc
    if op == "EQ":
        return equal(a, b)
    if op == "NE":
        return not equal(a, b)
    if op in ("LT", "LE", "GT", "GE"):
        if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and not isinstance(b, bool):
            pass
        elif isinstance(a, str) and isinstance(b, str):
            pass
        else:
            raise MiValueError(f"cannot compare {kind(a)} with {kind(b)}")
        if op == "LT":
            return a < b
        if op == "LE":
            return a <= b
        if op == "GT":
            return a > b
        return a >= b
    if op == "AND":
        return b if truthy(a) else a
    if op == "OR":
        return a if truthy(a) else b
    raise MiValueError(f"unknown operator {op}")  # pragma: no cover


def unary(op: str, value: Any) -> Any:
    if op == "NOT":
        return not truthy(value)
    if op == "MINUS":
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return -value
        raise MiValueError(f"cannot negate {kind(value)}")
    raise MiValueError(f"unknown unary operator {op}")  # pragma: no cover


def as_int(value: Any, what: str = "value") -> int:
    if isinstance(value, bool):
        raise MiValueError(f"{what} must be a number, not a flag")
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, float):
        raise MiValueError(f"{what} must be a whole number")
    raise MiValueError(f"{what} must be a number, not {kind(value)}")


def as_text(value: Any, what: str = "value") -> str:
    if isinstance(value, str):
        return value
    raise MiValueError(f"{what} must be text, not {kind(value)}")


def as_list(value: Any, what: str = "value") -> tuple:
    if isinstance(value, tuple):
        return value
    raise MiValueError(f"{what} must be a list, not {kind(value)}")
