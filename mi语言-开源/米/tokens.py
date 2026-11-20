"""Tokens and source positions for MI."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class Position:
    line: int
    col: int
    offset: int

    def __str__(self) -> str:  # pragma: no cover - convenience only
        return f"{self.line}:{self.col}"


@dataclass(frozen=True)
class Span:
    start: Position
    end: Position

    @staticmethod
    def at(line: int, col: int, offset: int) -> "Span":
        return Span(Position(line, col, offset), Position(line, col + 1, offset + 1))

    @staticmethod
    def join(a: "Span", b: "Span") -> "Span":
        return Span(a.start, b.end)

    def __str__(self) -> str:
        return f"{self.start.line}:{self.start.col}"


@dataclass(frozen=True)
class Token:
    kind: str
    value: Any
    text: str
    span: Span

    @property
    def line(self) -> int:
        return self.span.start.line

    @property
    def col(self) -> int:
        return self.span.start.col

    def is_punct(self, value: str) -> bool:
        return self.kind == "PUNCT" and self.value == value

    def is_kw(self, *names: str) -> bool:
        return self.kind == "KW" and self.value in names

    def is_op(self, *names: str) -> bool:
        return self.kind == "OP" and self.value in names

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"Token({self.kind}, {self.value!r}, {self.text!r}, {self.span})"


PUNCTUATION = "(){}[]⟨⟩,:.;="

SEP = "SEP"
EOF = "EOF"
