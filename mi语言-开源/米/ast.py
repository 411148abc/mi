"""AST node definitions for MI.

The shape of these nodes is part of the cross-runtime contract documented in
``文档/architecture.md``; ``to_dict`` produces exactly that JSON form.
"""
from __future__ import annotations

from dataclasses import dataclass, field, fields, is_dataclass
from typing import Any, Iterator

from .tokens import Position, Span


@dataclass
class Node:
    span: Span


# --------------------------------------------------------------------------
# items
# --------------------------------------------------------------------------
@dataclass
class Program(Node):
    items: list[Node]


@dataclass
class Use(Node):
    path: str
    alias: str | None


@dataclass
class WireDecl(Node):
    name: str
    init: "Expr | None"


@dataclass
class Param(Node):
    name: str
    default: "Expr | None"


@dataclass
class Station(Node):
    name: str
    params: list[Param]
    body: "Block"
    out_ports: tuple[str, ...] = field(default=(), repr=False, compare=False)


@dataclass
class Block(Node):
    flows: list[Node]


# --------------------------------------------------------------------------
# flows
# --------------------------------------------------------------------------
@dataclass
class Flow(Node):
    expr: "Expr"


@dataclass
class Emit(Node):
    target: str
    value: "Expr"


@dataclass
class When(Node):
    cond: "Expr"
    body: Block
    otherwise: "Block | When | None"


@dataclass
class Cycle(Node):
    kind: str  # "count" | "while" | "until" | "over"
    head: "Expr | None"
    var: str | None
    body: Block


@dataclass
class For(Node):
    var: str
    items: "Expr"
    body: Block


@dataclass
class Out(Node):
    name: str
    value: "Expr"


@dataclass
class Return(Node):
    value: "Expr | None"


@dataclass
class Halt(Node):
    pass


@dataclass
class Skip(Node):
    pass


# --------------------------------------------------------------------------
# expressions
# --------------------------------------------------------------------------
@dataclass
class NumberLit(Node):
    value: Any


@dataclass
class TextLit(Node):
    value: str


@dataclass
class FlagLit(Node):
    value: bool


@dataclass
class NilLit(Node):
    pass


@dataclass
class ListLit(Node):
    items: list["Expr"]


@dataclass
class FieldInit(Node):
    name: str
    value: "Expr"


@dataclass
class AssemblyLit(Node):
    fields: list[FieldInit]


@dataclass
class Name(Node):
    name: str
    glyph: bool = False


@dataclass
class Read(Node):
    name: str


@dataclass
class Field(Node):
    base: "Expr"
    name: str


@dataclass
class Arg(Node):
    label: str | None
    value: "Expr"


@dataclass
class Call(Node):
    name: str | None
    callee: "Expr | None"
    args: list[Arg]
    glyph: bool = False
    keyword_typo: bool = False


@dataclass
class Binary(Node):
    op: str
    left: "Expr"
    right: "Expr"


@dataclass
class Unary(Node):
    op: str
    operand: "Expr"


Expr = (
    NumberLit
    | TextLit
    | FlagLit
    | NilLit
    | ListLit
    | AssemblyLit
    | Name
    | Read
    | Field
    | Call
    | Binary
    | Unary
)


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def _span_dict(span: Span) -> dict[str, Any]:
    return {
        "start": {"line": span.start.line, "col": span.start.col, "offset": span.start.offset},
        "end": {"line": span.end.line, "col": span.end.col, "offset": span.end.offset},
    }


def to_dict(node: Any) -> Any:
    if node is None:
        return None
    if isinstance(node, Span):
        return _span_dict(node)
    if isinstance(node, Position):  # pragma: no cover - defensive
        return {"line": node.line, "col": node.col, "offset": node.offset}
    if isinstance(node, list):
        return [to_dict(item) for item in node]
    if isinstance(node, tuple):
        return [to_dict(item) for item in node]
    if is_dataclass(node) and not isinstance(node, type):
        out: dict[str, Any] = {"type": type(node).__name__}
        for f in fields(node):
            if f.name == "out_ports":
                continue
            out[f.name] = to_dict(getattr(node, f.name))
        return out
    return node


def children(node: Node) -> list[Node]:
    """Direct child nodes, in source order."""
    result: list[Node] = []
    for f in fields(node):
        value = getattr(node, f.name)
        if isinstance(value, Node):
            result.append(value)
        elif isinstance(value, (list, tuple)):
            for item in value:
                if isinstance(item, Node):
                    result.append(item)
    return result


def walk(node: Node) -> Iterator[Node]:
    yield node
    for child in children(node):
        yield from walk(child)


def walk_flows(block: Block) -> Iterator[Node]:
    """All flows in a block, descending into gates, cycles and for-loops."""
    for flow in block.flows:
        yield flow
        if isinstance(flow, When):
            yield from walk_flows(flow.body)
            if isinstance(flow.otherwise, Block):
                yield from walk_flows(flow.otherwise)
            elif isinstance(flow.otherwise, When):
                yield from walk_flows(Block(flow.otherwise.span, [flow.otherwise]))
        elif isinstance(flow, (Cycle, For)):
            yield from walk_flows(flow.body)


def station_out_ports(station: Station) -> tuple[str, ...]:
    """Statically collected output-port names, in first-appearance order."""
    names: list[str] = []
    for flow in walk_flows(station.body):
        if isinstance(flow, Out) and flow.name not in names:
            names.append(flow.name)
        elif isinstance(flow, Return) and flow.value is not None and "value" not in names:
            names.append("value")
    return tuple(names)
