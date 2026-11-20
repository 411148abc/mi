"""MI builtin functions.

A builtin implementation has the signature ``impl(runtime, bound, span)`` where
``bound`` maps parameter names to already-evaluated values (a variadic
parameter holds a list).  Implementations return an MI value.

Host-specific builtins (`ask`, `env`, `argv`, `serve`) live in
``米/宿主/python_host.py``; the browser host is implemented in JavaScript.
"""
from __future__ import annotations

import math
import random as random_module
import time
from typing import Any, Callable

from .values import (
    NOTHING,
    Assembly,
    BuiltinValue,
    MiValueError,
    as_int,
    as_list,
    as_text,
    display,
    kind,
    text_of,
    truthy,
)

Impl = Callable[[Any, dict[str, Any], Any], Any]


def _show(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    values = bound.get("value", [])
    runtime.emit_line(" ".join(display(value) for value in values))
    return NOTHING


def _len(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    value = bound.get("value")
    if isinstance(value, (str, tuple, Assembly)):
        return len(value)
    raise MiValueError(f"`len` needs text, a list or an assembly, not {kind(value)}")


def _push(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = as_list(bound.get("into"), "`push`'s `into`")
    return items + (bound.get("value"),)


def _at(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = bound.get("items")
    index = as_int(bound.get("index"), "`at`'s `index`")
    if not isinstance(items, (tuple, str)):
        raise MiValueError(f"`at` needs a list or text, not {kind(items)}")
    try:
        return items[index]
    except IndexError:
        raise MiValueError(
            f"index {index} is out of range for a {kind(items)} of length {len(items)}"
        ) from None


def _slice(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = bound.get("items")
    if not isinstance(items, (tuple, str)):
        raise MiValueError(f"`slice` needs a list or text, not {kind(items)}")
    start = as_int(bound.get("from", 0), "`slice`'s `from`")
    end = bound.get("to")
    stop = as_int(end, "`slice`'s `to`") if end is not None else len(items)
    return items[start:stop]


def _map(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = as_list(bound.get("items"), "`map`'s `items`")
    fn = bound.get("with")
    return tuple(runtime.call_value(fn, [(None, item)], span) for item in items)


def _filter(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = as_list(bound.get("items"), "`filter`'s `items`")
    fn = bound.get("by")
    return tuple(
        item
        for item in items
        if truthy(runtime.call_value(fn, [(None, item)], span))
    )


def _fold(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = as_list(bound.get("items"), "`fold`'s `items`")
    fn = bound.get("with")
    acc = bound.get("from")
    for item in items:
        acc = runtime.call_value(fn, [(None, acc), (None, item)], span)
    return acc


def _span(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    start = as_int(bound.get("from"), "`span`'s `from`")
    stop = as_int(bound.get("to"), "`span`'s `to`")
    step = as_int(bound.get("step", 1), "`span`'s `step`")
    if step == 0:
        raise MiValueError("`span`'s `step` cannot be zero")
    size = max(0, (stop - start + (step - 1 if step > 0 else step + 1)) // step) if step else 0
    if size > 1_000_000:
        raise MiValueError("`span` is too large", code="E0305")
    return tuple(range(start, stop, step))


def _join(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = as_list(bound.get("items"), "`join`'s `items`")
    separator = bound.get("with", "")
    if not isinstance(separator, str):
        raise MiValueError(f"`join`'s separator must be text, not {kind(separator)}")
    return separator.join(display(item) for item in items)


def _split(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    text = as_text(bound.get("text"), "`split`'s `text`")
    separator = as_text(bound.get("at"), "`split`'s separator")
    if separator == "":
        raise MiValueError("`split` needs a non-empty separator")
    return tuple(text.split(separator))


def _text(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return text_of(bound.get("value"))


def _number(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    value = bound.get("value")
    if isinstance(value, bool):
        raise MiValueError("cannot convert a flag to a number")
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        text = value.strip()
        try:
            return int(text, 0)
        except ValueError:
            pass
        try:
            return float(text)
        except ValueError:
            raise MiValueError(f"cannot read {value!r} as a number") from None
    raise MiValueError(f"cannot convert {kind(value)} to a number")


def _kind(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return kind(bound.get("value"))


def _has(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    container = bound.get("container")
    value = bound.get("value")
    if isinstance(container, tuple):
        from .values import equal

        return any(equal(item, value) for item in container)
    if isinstance(container, Assembly):
        return container.has(as_text(value, "`has`'s `value`"))
    if isinstance(container, str):
        return as_text(value, "`has`'s `value`") in container
    raise MiValueError(f"`has` needs a list, assembly or text, not {kind(container)}")


def _keys(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    assembly = bound.get("target")
    if not isinstance(assembly, Assembly):
        raise MiValueError(f"`keys` needs an assembly, not {kind(assembly)}")
    return assembly.keys()


def _place(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    assembly = bound.get("target")
    if not isinstance(assembly, Assembly):
        raise MiValueError(f"`place` needs an assembly, not {kind(assembly)}")
    field = as_text(bound.get("field"), "`place`'s `field`")
    return assembly.with_field(field, bound.get("value"))


def _list(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return tuple(bound.get("value", []))


def _sort(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = as_list(bound.get("items"), "`sort`'s `items`")
    try:
        return tuple(sorted(items))
    except TypeError:
        raise MiValueError("`sort` needs items that can be compared with each other") from None


def _reverse(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    items = bound.get("items")
    if isinstance(items, tuple):
        return tuple(reversed(items))
    if isinstance(items, str):
        return items[::-1]
    raise MiValueError(f"`reverse` needs a list or text, not {kind(items)}")


def _abs(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    value = bound.get("value")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return abs(value)
    raise MiValueError(f"`abs` needs a number, not {kind(value)}")


def _floor(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return int(math.floor(_as_number(bound.get("value"), "`floor`")))


def _ceil(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return int(math.ceil(_as_number(bound.get("value"), "`ceil`")))


def _round(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    value = _as_number(bound.get("value"), "`round`")
    digits = bound.get("digits")
    if digits is None:
        return int(round(value))
    places = as_int(digits, "`round`'s `digits`")
    return round(value, places)


def _sqrt(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    value = _as_number(bound.get("value"), "`sqrt`")
    if value < 0:
        raise MiValueError("cannot take the square root of a negative number")
    return math.sqrt(value)


def _as_number(value: Any, what: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise MiValueError(f"{what} needs a number, not {kind(value)}")
    return value


def _min(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    values = bound.get("value", [])
    if len(values) == 1 and isinstance(values[0], tuple):
        values = list(values[0])
    if not values:
        raise MiValueError("`min` needs at least one value")
    try:
        return min(values)
    except TypeError:
        raise MiValueError("`min` needs values that can be compared") from None


def _max(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    values = bound.get("value", [])
    if len(values) == 1 and isinstance(values[0], tuple):
        values = list(values[0])
    if not values:
        raise MiValueError("`max` needs at least one value")
    try:
        return max(values)
    except TypeError:
        raise MiValueError("`max` needs values that can be compared") from None


def _random(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    start = bound.get("from", 0)
    stop = bound.get("to", 1)
    if isinstance(start, bool) or isinstance(stop, bool):
        raise MiValueError("`random` needs numbers")
    if not isinstance(start, (int, float)) or not isinstance(stop, (int, float)):
        raise MiValueError("`random` needs numbers")
    if isinstance(start, int) and isinstance(stop, int):
        if stop <= start:
            raise MiValueError("`random` needs `to` greater than `from` for whole numbers")
        return random_module.randrange(start, stop)
    return random_module.uniform(start, stop)


def _clock(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    return time.time()


def _json_parse(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    from .jsonio import parse_json

    return parse_json(as_text(bound.get("text"), "`json.parse`'s `text`"))


def _json_text(runtime: Any, bound: dict[str, Any], span: Any) -> Any:
    from .jsonio import encode_json

    return encode_json(bound.get("value"))


BUILTIN_IMPLS: dict[str, Impl] = {
    "show": _show,
    "len": _len,
    "push": _push,
    "at": _at,
    "slice": _slice,
    "map": _map,
    "filter": _filter,
    "fold": _fold,
    "span": _span,
    "join": _join,
    "split": _split,
    "text": _text,
    "number": _number,
    "kind": _kind,
    "has": _has,
    "keys": _keys,
    "place": _place,
    "list": _list,
    "sort": _sort,
    "reverse": _reverse,
    "abs": _abs,
    "floor": _floor,
    "ceil": _ceil,
    "round": _round,
    "sqrt": _sqrt,
    "min": _min,
    "max": _max,
    "random": _random,
    "clock": _clock,
    "json.parse": _json_parse,
    "json.text": _json_text,
}


def builtin_value(name: str) -> BuiltinValue:
    return BuiltinValue(name)
