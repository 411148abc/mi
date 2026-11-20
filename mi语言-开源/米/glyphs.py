"""Glyph tables for MI.

`规格/glyphs.json` is the single source of truth for every spelling of every
keyword, operator, literal and builtin.  This module loads it once and exposes
lookup helpers used by the lexer, parser, checker, evaluator and CLI.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SPEC_PATH = Path(__file__).resolve().parent.parent / "规格" / "glyphs.json"


def _is_word(spelling: str) -> bool:
    return all(ch.isalnum() or ch == "_" for ch in spelling) and any(
        ch.isalpha() or ch == "_" for ch in spelling
    )


def _is_symbol(spelling: str) -> bool:
    return not any(ch.isalnum() or ch == "_" for ch in spelling)


@dataclass(frozen=True)
class Keyword:
    name: str
    canonical: str
    spellings: tuple[str, ...]
    summary: str
    example: str


@dataclass(frozen=True)
class Operator:
    name: str
    canonical: str
    spellings: tuple[str, ...]
    precedence: int
    assoc: str
    arity: str
    summary: str


@dataclass(frozen=True)
class Literal:
    name: str
    canonical: str
    kind: str
    value: Any
    spellings: tuple[str, ...]


@dataclass(frozen=True)
class Param:
    name: str
    optional: bool = False
    variadic: bool = False


@dataclass(frozen=True)
class Builtin:
    canonical: str
    spellings: tuple[str, ...]
    params: tuple[Param, ...]
    returns: str
    hosts: tuple[str, ...]
    summary: str

    @property
    def required(self) -> list[str]:
        return [p.name for p in self.params if not p.optional and not p.variadic]

    @property
    def variadic(self) -> Param | None:
        for p in self.params:
            if p.variadic:
                return p
        return None

    def param(self, name: str) -> Param | None:
        for p in self.params:
            if p.name == name:
                return p
        return None

    def accepts(self, label: str) -> bool:
        return self.param(label) is not None


class GlyphTable:
    """Immutable view over ``规格/glyphs.json``."""

    def __init__(self, data: dict[str, Any]) -> None:
        self.language: str = data.get("language", "MI")
        self.version: int = int(data.get("version", 1))
        self.tagline: str = data.get("tagline", "")

        self.keywords: list[Keyword] = [
            Keyword(
                name=k["name"],
                canonical=k["canonical"],
                spellings=tuple(k["spellings"]),
                summary=k.get("summary", ""),
                example=k.get("example", ""),
            )
            for k in data["keywords"]
        ]
        self.operators: list[Operator] = [
            Operator(
                name=o["name"],
                canonical=o["canonical"],
                spellings=tuple(o["spellings"]),
                precedence=int(o["precedence"]),
                assoc=o["assoc"],
                arity=o["arity"],
                summary=o.get("summary", ""),
            )
            for o in data["operators"]
        ]
        self.literals: list[Literal] = [
            Literal(
                name=l["name"],
                canonical=l["canonical"],
                kind=l["kind"],
                value=l["value"],
                spellings=tuple(l["spellings"]),
            )
            for l in data["literals"]
        ]
        self.builtins: list[Builtin] = [
            Builtin(
                canonical=b["canonical"],
                spellings=tuple(b["spellings"]),
                params=tuple(
                    Param(
                        name=p["name"],
                        optional=bool(p.get("optional", False)),
                        variadic=bool(p.get("variadic", False)),
                    )
                    for p in b.get("params", [])
                ),
                returns=b.get("returns", "any"),
                hosts=tuple(b.get("hosts", ["python", "browser"])),
                summary=b.get("summary", ""),
            )
            for b in data["builtins"]
        ]

        self.keyword_by_name = {k.name: k for k in self.keywords}
        self.operator_by_name = {o.name: o for o in self.operators}
        self.literal_by_name = {l.name: l for l in self.literals}
        self.builtin_by_name = {b.canonical: b for b in self.builtins}
        self.precedence = {o.name: o.precedence for o in self.operators}
        self.assoc = {o.name: o.assoc for o in self.operators}
        self.prefix_operators = {o.name for o in self.operators if o.arity == "prefix"}

        # ---- spelling indexes -------------------------------------------------
        self.keyword_spellings: dict[str, str] = {}
        self.operator_spellings: dict[str, str] = {}
        self.literal_spellings: dict[str, str] = {}
        self.builtin_word_aliases: dict[str, str] = {}
        self.builtin_symbol_aliases: dict[str, str] = {}

        for k in self.keywords:
            for sp in k.spellings:
                self.keyword_spellings[sp.lower() if _is_word(sp) else sp] = k.name
        for o in self.operators:
            for sp in o.spellings:
                self.operator_spellings[sp.lower() if _is_word(sp) else sp] = o.name
        for l in self.literals:
            for sp in l.spellings:
                self.literal_spellings[sp.lower() if _is_word(sp) else sp] = l.name
        for b in self.builtins:
            for sp in b.spellings:
                if "." in sp:
                    continue
                if _is_word(sp):
                    self.builtin_word_aliases[sp.lower()] = b.canonical
                elif _is_symbol(sp):
                    self.builtin_symbol_aliases[sp] = b.canonical

        # ---- lexer match table ------------------------------------------------
        entries: list[tuple[str, bool, str, str]] = []
        for spelling, token_name in self.keyword_spellings.items():
            entries.append((spelling, _is_word(spelling), "KW", token_name))
        for spelling, token_name in self.operator_spellings.items():
            entries.append((spelling, _is_word(spelling), "OP", token_name))
        for spelling, token_name in self.literal_spellings.items():
            entries.append((spelling, _is_word(spelling), "LITERAL", token_name))
        for spelling, canonical in self.builtin_symbol_aliases.items():
            entries.append((spelling, False, "GLYPH", canonical))
        self.match_entries = sorted(
            entries, key=lambda e: (-len(e[0]), e[0])
        )
        self.max_spelling_len = max((len(e[0]) for e in entries), default=1)

        # ---- suggestion corpus ------------------------------------------------
        self.builtin_all_aliases: dict[str, str] = {}
        for b in self.builtins:
            self.builtin_all_aliases[b.canonical.lower()] = b.canonical
            for spelling in b.spellings:
                self.builtin_all_aliases[spelling.lower()] = b.canonical
        words: set[str] = set()
        for k in self.keywords:
            words.update(s for s in k.spellings if _is_word(s))
        for o in self.operators:
            words.update(s for s in o.spellings if _is_word(s))
        for l in self.literals:
            words.update(s for s in l.spellings if _is_word(s))
        words.update(self.builtin_word_aliases)
        words.update(b.canonical.lower() for b in self.builtins)
        self.suggestion_words = sorted(words)
        self.keyword_words = sorted(
            spelling
            for keyword in self.keywords
            for spelling in keyword.spellings
            if _is_word(spelling)
        )
        self.spelling_examples = {
            name: " | ".join(k.spellings) for name, k in self.keyword_by_name.items()
        }
        for b in self.builtins:
            self.spelling_examples[b.canonical] = " | ".join(b.spellings)

    # ---- lookups ----------------------------------------------------------
    def keyword(self, name: str) -> Keyword | None:
        return self.keyword_by_name.get(name)

    def operator(self, name: str) -> Operator | None:
        return self.operator_by_name.get(name)

    def literal(self, name: str) -> Literal | None:
        return self.literal_by_name.get(name)

    def builtin(self, name: str) -> Builtin | None:
        """Case-insensitive builtin lookup by canonical or any spelling (dotted too)."""
        if not name:
            return None
        if name in self.builtin_by_name:
            return self.builtin_by_name[name]
        low = name.lower()
        canonical = self.builtin_all_aliases.get(low)
        if canonical:
            return self.builtin_by_name[canonical]
        return None

    def builtin_glyph(self, spelling: str) -> str | None:
        return self.builtin_symbol_aliases.get(spelling)

    def is_keyword_spelling(self, text: str) -> str | None:
        key = text.lower() if _is_word(text) else text
        return self.keyword_spellings.get(key)

    # ---- reference rendering ---------------------------------------------
    def all_spelling_rows(self) -> list[tuple[str, str, str, str]]:
        """(group, canonical, spellings, summary) rows for 文档/CLI."""
        rows: list[tuple[str, str, str, str]] = []
        for k in self.keywords:
            rows.append(("keyword", k.canonical, " ".join(k.spellings), k.summary))
        for o in self.operators:
            rows.append(("operator", o.canonical, " ".join(o.spellings), o.summary))
        for l in self.literals:
            rows.append(("literal", l.canonical, " ".join(l.spellings), ""))
        for b in self.builtins:
            rows.append(("builtin", b.canonical, " ".join(b.spellings), b.summary))
        return rows


def load(path: Path | None = None) -> GlyphTable:
    with open(path or SPEC_PATH, "r", encoding="utf-8") as fh:
        return GlyphTable(json.load(fh))


TABLE = load()
