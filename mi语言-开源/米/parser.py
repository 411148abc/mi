"""The MI parser: recursive descent with error recovery.

The parser accepts every surface form documented in ``SPEC.md`` (alternative
spellings, optional punctuation, optional parentheses, implicit calls, infix
emit, ``{}`` and ``do…end`` blocks, ``otherwise when`` chains).  Errors are
recovered at statement boundaries so one broken line does not hide the next.
"""
from __future__ import annotations

from collections.abc import Callable

from . import ast
from .ast import (
    Arg,
    AssemblyLit,
    Binary,
    Block,
    Call,
    Cycle,
    Emit,
    Field,
    FieldInit,
    FlagLit,
    Flow,
    For,
    Halt,
    ListLit,
    Name,
    NilLit,
    NumberLit,
    Out,
    Param,
    Program,
    Read,
    Return,
    Skip,
    Station,
    TextLit,
    Unary,
    Use,
    When,
    WireDecl,
    station_out_ports,
)
from .errors import Diagnostic, DiagnosticBag
from .glyphs import TABLE, GlyphTable
from .tokens import EOF, SEP, Position, Span, Token
from .lexer import tokenize

EXPRESSION_START_KINDS = ("IDENT", "NUMBER", "TEXT", "FLAG", "NIL", "GLYPH")


class _Recover(Exception):
    """Internal: abandon the current statement and resynchronise."""


class Parser:
    def __init__(
        self,
        tokens: list[Token],
        source: str = "",
        filename: str = "<source>",
        table: GlyphTable = TABLE,
    ) -> None:
        self.table = table
        self.source = source
        self.filename = filename
        self.tokens = self._strip_inner_seps(tokens)
        self.i = 0
        self.bag = DiagnosticBag(filename=filename)

    # ------------------------------------------------------------- helpers
    @staticmethod
    def _strip_inner_seps(tokens: list[Token]) -> list[Token]:
        depth = 0
        out: list[Token] = []
        for tok in tokens:
            if tok.kind == "PUNCT" and tok.value in "([⟨":
                depth += 1
            elif tok.kind == "PUNCT" and tok.value in ")]⟩":
                depth = max(0, depth - 1)
            if tok.kind == SEP and depth > 0:
                continue
            out.append(tok)
        return out

    def peek(self, k: int = 0) -> Token:
        idx = min(self.i + k, len(self.tokens) - 1)
        return self.tokens[idx]

    def advance(self) -> Token:
        tok = self.peek()
        if tok.kind != EOF:
            self.i += 1
        return tok

    def at_eof(self) -> bool:
        return self.peek().kind == EOF

    def at_sep(self) -> bool:
        return self.peek().kind == SEP

    def skip_seps(self) -> None:
        while self.peek().kind == SEP:
            self.advance()

    def at_punct(self, value: str) -> bool:
        tok = self.peek()
        return tok.kind == "PUNCT" and tok.value == value

    def at_kw(self, *names: str) -> bool:
        tok = self.peek()
        return tok.kind == "KW" and tok.value in names

    def at_op(self, *names: str) -> bool:
        tok = self.peek()
        return tok.kind == "OP" and tok.value in names

    def error(self, code: str, message: str, span: Span, hint: str | None = None) -> None:
        self.bag.error(code, message, span, hint)

    def _unexpected(self, expected: str, tok: Token | None = None) -> None:
        tok = tok or self.peek()
        shown = tok.text if tok.text else "end of input"
        hint = None
        words = self.table.suggestion_words
        from .errors import suggest

        close = suggest(tok.text, words, n=2) if tok.text else []
        if close:
            hint = "maybe you meant " + " or ".join(f"`{c}`" for c in close) + "?"
        self.error("E0100", f"unexpected {shown!r}; expected {expected}", tok.span, hint)

    def expect_punct(self, value: str) -> Token:
        if self.at_punct(value):
            return self.advance()
        self._unexpected(f"`{value}`")
        raise _Recover()

    def expect_kw(self, *names: str) -> Token:
        tok = self.peek()
        if tok.kind == "KW" and tok.value in names:
            return self.advance()
        self._unexpected(" or ".join(f"`{n.lower()}`" for n in names))
        raise _Recover()

    def expect_ident(self) -> Token:
        tok = self.peek()
        if tok.kind == "IDENT":
            return self.advance()
        # Tolerate a keyword used as a name, but say so.
        if tok.kind == "KW":
            self.error(
                "E0100",
                f"`{tok.text}` is a keyword and cannot be used as a name",
                tok.span,
                "pick a different name",
            )
            advance = self.advance()
            return Token("IDENT", advance.text, advance.text, advance.span)
        self._unexpected("a name")
        raise _Recover()

    def _recover(self) -> None:
        while not self.at_eof():
            if self.at_sep():
                self.advance()
                return
            if self.at_punct("}") or self.at_kw("END"):
                return
            self.advance()

    @staticmethod
    def _span(start: Span, end: Span) -> Span:
        return Span(start.start, end.end)

    # ---------------------------------------------------------------- program
    def parse_program(self) -> tuple[Program, list[Diagnostic]]:
        start = self.peek().span
        items: list[ast.Node] = []
        self.skip_seps()
        while not self.at_eof():
            before = self.i
            try:
                item = self.parse_item()
            except _Recover:
                if self.i == before:
                    self.advance()
                self._recover()
                self.skip_seps()
                continue
            if item is not None:
                items.append(item)
            if self.i == before:
                self.advance()
            self.skip_seps()
        end = self.peek().span
        return Program(Span(start.start, end.end), items), self.bag.diagnostics

    def parse_item(self) -> ast.Node:
        tok = self.peek()
        if tok.kind == "KW":
            if tok.value == "WIRE":
                return self.parse_wire()
            if tok.value == "STATION":
                return self.parse_station()
            if tok.value == "USE":
                return self.parse_use()
        return self.parse_flow()

    # ------------------------------------------------------------------ decls
    def parse_wire(self) -> WireDecl:
        tok = self.expect_kw("WIRE")
        name = self.expect_ident()
        init: ast.Expr | None = None
        if self.at_punct(":") or self.at_punct("="):
            self.advance()
            init = self.parse_expr()
        end = init.span if init is not None else name.span
        return WireDecl(Span(tok.span.start, end.end), name.value, init)

    def parse_use(self) -> Use:
        tok = self.expect_kw("USE")
        if self.peek().kind != "TEXT":
            self.error("E0100", "expected a module path text after `use`", self.peek().span)
            raise _Recover()
        path = self.advance()
        alias = None
        end = path.span
        if self.at_kw("AS"):
            self.advance()
            alias_tok = self.expect_ident()
            alias = alias_tok.value
            end = alias_tok.span
        return Use(Span(tok.span.start, end.end), path.value, alias)

    def parse_station(self) -> Station:
        tok = self.expect_kw("STATION")
        name = self.expect_ident()
        self.expect_punct("(")
        params: list[Param] = []
        if not self.at_punct(")"):
            while True:
                param_tok = self.expect_ident()
                default: ast.Expr | None = None
                if self.at_punct(":") or self.at_punct("="):
                    self.advance()
                    default = self.parse_expr()
                span = param_tok.span
                if default is not None:
                    span = Span(param_tok.span.start, default.span.end)
                params.append(Param(span, param_tok.value, default))
                if self.at_punct(","):
                    self.advance()
                    continue
                if self.at_punct(")"):
                    break
                self.error(
                    "E0103",
                    "expected `,` or `)` in the parameter list",
                    self.peek().span,
                )
                break
        self.expect_punct(")")
        body = self.parse_block()
        station = Station(Span(tok.span.start, body.span.end), name.value, params, body)
        station.out_ports = station_out_ports(station)
        return station

    # ----------------------------------------------------------------- blocks
    def parse_block(self) -> Block:
        tok = self.peek()
        if tok.kind == "PUNCT" and tok.value == "{":
            self.advance()
            flows = self.parse_flows_until(lambda t: t.kind == "PUNCT" and t.value == "}")
            if self.at_punct("}"):
                closer = self.advance()
                return Block(Span(tok.span.start, closer.span.end), flows)
            self.error("E0102", "missing `}` to close this block", self.peek().span)
            return Block(Span(tok.span.start, self.peek().span.end), flows)
        if tok.kind == "KW" and tok.value in ("DO", "BEGIN"):
            opener = self.advance()
            flows = self.parse_flows_until(lambda t: t.kind == "KW" and t.value == "END")
            if self.at_kw("END"):
                closer = self.advance()
                return Block(Span(opener.span.start, closer.span.end), flows)
            self.error("E0102", "missing `end` to close this block", self.peek().span)
            return Block(Span(opener.span.start, self.peek().span.end), flows)
        self.error(
            "E0102",
            "expected a block: `{ … }`, or `do … end` / `begin … end`",
            tok.span,
            "a block is required here",
        )
        raise _Recover()

    def parse_flows_until(self, stop: Callable[[Token], bool]) -> list[ast.Node]:
        flows: list[ast.Node] = []
        self.skip_seps()
        while not self.at_eof() and not stop(self.peek()):
            before = self.i
            try:
                flow = self.parse_flow()
                if flow is not None:
                    flows.append(flow)
            except _Recover:
                self._recover()
            if self.i == before:
                self.advance()
            self.skip_seps()
        return flows

    # ------------------------------------------------------------------ flows
    def parse_flow(self) -> ast.Node:
        tok = self.peek()
        if tok.kind == "KW":
            value = tok.value
            if value == "EMIT":
                return self.parse_emit()
            if value == "WHEN":
                return self.parse_when()
            if value == "CYCLE":
                return self.parse_cycle()
            if value == "FOR":
                return self.parse_for()
            if value == "OUT":
                return self.parse_out()
            if value == "RETURN":
                return self.parse_return()
            if value == "HALT":
                return Halt(self.advance().span)
            if value == "SKIP":
                return Skip(self.advance().span)
            if value in ("OTHERWISE",):
                self.error(
                    "E0100",
                    "`otherwise` must directly follow a `when` block",
                    tok.span,
                    "use `when … { } otherwise { }`",
                )
                raise _Recover()
            if value in ("STATION", "USE", "WIRE"):
                return self.parse_item()
            if value in ("END",):
                self.error("E0100", "unexpected `end`", tok.span)
                raise _Recover()
        if tok.kind == "IDENT" and self.peek(1).kind == "KW" and self.peek(1).value == "EMIT":
            return self.parse_emit_infix()
        expr = self.parse_expr()
        flow = Flow(Span(tok.span.start, expr.span.end), expr)
        # `wenn cond { … }` — an identifier where a flow keyword belongs.
        if tok.kind == "IDENT" and self._at_block_start():
            hint = self._keyword_typo_hint(tok.value)
            if hint is not None:
                self.bag.error(
                    "E0101", f"unknown keyword `{tok.value}`", tok.span, hint
                )
                if isinstance(expr, Call):
                    expr.keyword_typo = True
        return flow

    def _at_block_start(self) -> bool:
        return (
            self.at_punct("{")
            or self.at_kw("DO", "BEGIN")
        )

    def _keyword_typo_hint(self, name: str) -> str | None:
        from .errors import suggest

        matches = suggest(name, self.table.keyword_words, n=1, cutoff=0.7)
        if not matches:
            return None
        word = matches[0]
        token_name = self.table.is_keyword_spelling(word)
        keyword = self.table.keyword(token_name) if token_name else None
        if keyword is None:  # pragma: no cover - defensive
            return None
        return (
            f"maybe you meant `{word}` (also written: {' | '.join(keyword.spellings)})"
        )

    def parse_emit(self) -> Emit:
        tok = self.expect_kw("EMIT")
        target = self.expect_ident()
        if self.at_punct(":") or self.at_punct("="):
            self.advance()
        elif self.at_kw("EMIT"):
            self.advance()
        if self.at_sep() or self.at_eof() or self.at_punct("}"):
            self.error("E0102", "`emit` needs a value", self.peek().span)
            raise _Recover()
        value = self.parse_expr()
        return Emit(Span(tok.span.start, value.span.end), target.value, value)

    def parse_emit_infix(self) -> Emit:
        target = self.advance()
        self.advance()  # the emit spelling
        value = self.parse_expr()
        return Emit(Span(target.span.start, value.span.end), target.value, value)

    def parse_when(self) -> When:
        tok = self.expect_kw("WHEN")
        cond = self.parse_expr()
        if self.at_kw("THEN"):
            self.advance()
        body = self.parse_block()
        otherwise: Block | When | None = None
        end = body.span
        self.skip_seps()
        if self.at_kw("OTHERWISE"):
            self.advance()
            self.skip_seps()
            if self.at_kw("WHEN"):
                otherwise = self.parse_when()
            else:
                otherwise = self.parse_block()
            end = otherwise.span
        return When(Span(tok.span.start, end.end), cond, body, otherwise)

    def parse_cycle(self) -> Cycle:
        tok = self.expect_kw("CYCLE")
        kind = "count"
        head: ast.Expr | None = None
        if self.at_kw("WHILE"):
            self.advance()
            kind = "while"
            head = self.parse_expr()
        elif self.at_kw("UNTIL"):
            self.advance()
            kind = "until"
            head = self.parse_expr()
        elif self.at_kw("OVER"):
            self.advance()
            kind = "over"
            head = self.parse_expr()
        else:
            head = self.parse_expr()
        var: str | None = None
        if self.at_kw("AS"):
            self.advance()
            var = self.expect_ident().value
        body = self.parse_block()
        return Cycle(Span(tok.span.start, body.span.end), kind, head, var, body)

    def parse_for(self) -> For:
        tok = self.expect_kw("FOR")
        var = self.expect_ident()
        self.expect_kw("IN")
        items = self.parse_expr()
        body = self.parse_block()
        return For(Span(tok.span.start, body.span.end), var.value, items, body)

    def parse_out(self) -> Out:
        tok = self.expect_kw("OUT")
        name = self.expect_ident()
        if self.at_punct(":") or self.at_punct("="):
            self.advance()
        if self.at_sep() or self.at_eof() or self.at_punct("}"):
            self.error("E0102", f"output port `{name.value}` needs a value", self.peek().span)
            raise _Recover()
        value = self.parse_expr()
        return Out(Span(tok.span.start, value.span.end), name.value, value)

    def parse_return(self) -> Return:
        tok = self.expect_kw("RETURN")
        if self.at_sep() or self.at_eof() or self.at_punct("}") or self.at_kw("END"):
            return Return(tok.span, None)
        value = self.parse_expr()
        return Return(Span(tok.span.start, value.span.end), value)

    # ------------------------------------------------------------- expressions
    def parse_expr(self, min_prec: int = 0) -> ast.Expr:
        left = self.parse_unary()
        while True:
            tok = self.peek()
            if tok.kind != "OP":
                break
            op = self.table.operator(tok.value)
            if op is None or op.arity != "infix":
                break
            if op.precedence < min_prec:
                break
            self.advance()
            next_min = op.precedence if op.assoc == "right" else op.precedence + 1
            right = self.parse_expr(next_min)
            left = Binary(Span(left.span.start, right.span.end), op.name, left, right)
        return left

    def parse_unary(self) -> ast.Expr:
        tok = self.peek()
        if tok.kind == "OP" and tok.value in ("NOT", "MINUS"):
            self.advance()
            operand = self.parse_unary()
            return Unary(Span(tok.span.start, operand.span.end), tok.value, operand)
        return self.parse_postfix()

    def _callable_name(self, node: ast.Expr) -> tuple[str, bool] | None:
        if isinstance(node, Name):
            return node.name, node.glyph
        if isinstance(node, Field) and isinstance(node.base, Name) and not node.base.glyph:
            return node.base.name + "." + node.name, False
        return None

    def _starts_implicit_arg(self, tok: Token) -> bool:
        if tok.kind in EXPRESSION_START_KINDS:
            return True
        if tok.kind == "PUNCT" and tok.value in ("(", "[", "⟨"):
            return True
        if tok.kind == "KW" and tok.value in ("ASSEMBLY", "DRAW"):
            return True
        return False

    def _field_name(self) -> Token:
        tok = self.peek()
        if tok.kind == "IDENT":
            return self.advance()
        # Tolerate keywords/operators as member names: store.read, json.text, …
        if tok.kind in ("KW", "OP", "FLAG", "NIL") and tok.text and all(
            ch.isalnum() or ch == "_" for ch in tok.text
        ):
            self.advance()
            return Token("IDENT", tok.text, tok.text, tok.span)
        self.error("E0100", "expected a field name after `.`", tok.span)
        raise _Recover()

    def _key_name(self) -> Token:
        """A field name inside an assembly literal; keywords are tolerated."""
        tok = self.peek()
        if tok.kind == "IDENT":
            return self.advance()
        if tok.kind in ("KW", "OP", "FLAG", "NIL") and tok.text and all(
            ch.isalnum() or ch == "_" for ch in tok.text
        ):
            self.advance()
            return Token("IDENT", tok.text, tok.text, tok.span)
        return self.expect_ident()

    def parse_postfix(self) -> ast.Expr:
        node = self.parse_primary()
        while True:
            tok = self.peek()
            if tok.kind == "PUNCT" and tok.value == ".":
                self.advance()
                ident = self._field_name()
                nxt = self.peek()
                # Dotted names (store.read, json.text, ui.value) only merge when
                # the member is actually called, so `m.v.a` stays nested fields.
                if (
                    isinstance(node, Name)
                    and not node.glyph
                    and (nxt.kind == "PUNCT" and nxt.value == "(")
                ):
                    node = Name(
                        Span(node.span.start, ident.span.end),
                        node.name + "." + ident.text,
                        False,
                    )
                    continue
                node = Field(Span(node.span.start, ident.span.end), node, ident.text)
                continue
            if tok.kind == "PUNCT" and tok.value == "(":
                args = self.parse_paren_args()
                end = self.tokens[self.i - 1].span
                node = self._make_call(node, args, Span(node.span.start, end.end))
                continue
            callable_info = self._callable_name(node)
            if callable_info is not None and self._starts_implicit_arg(tok):
                args = self.parse_implicit_args()
                node = self._make_call(
                    node, args, Span(node.span.start, args[-1].value.span.end)
                )
                continue
            break
        return node

    def _make_call(self, node: ast.Expr, args: list[Arg], span: Span) -> Call:
        info = self._callable_name(node)
        if info is not None:
            name, glyph = info
            return Call(span, name, None, args, glyph)
        return Call(span, None, node, args, False)

    def parse_paren_args(self) -> list[Arg]:
        self.expect_punct("(")
        args: list[Arg] = []
        if self.at_punct(")"):
            self.advance()
            return args
        while True:
            if self.at_eof():
                self.error("E0102", "missing `)` to close this argument list", self.peek().span)
                break
            if self.at_punct(")"):
                self.advance()
                break
            args.append(self.parse_arg())
            if self.at_punct(","):
                self.advance()
                continue
            if self.at_punct(")"):
                self.advance()
                break
            if self._starts_implicit_arg(self.peek()):
                continue
            self.error("E0103", "expected `,` or `)` in the argument list", self.peek().span)
            raise _Recover()
        return args

    def parse_implicit_args(self) -> list[Arg]:
        args = [self.parse_arg()]
        while self.at_punct(","):
            self.advance()
            args.append(self.parse_arg())
        return args

    def parse_arg(self) -> Arg:
        tok = self.peek()
        if (
            tok.kind == "IDENT"
            and self.peek(1).kind == "PUNCT"
            and self.peek(1).value in (":", "=")
        ):
            label = self.advance()
            self.advance()
            value = self.parse_expr()
            return Arg(Span(label.span.start, value.span.end), label.value, value)
        value = self.parse_expr()
        return Arg(value.span, None, value)

    def parse_primary(self) -> ast.Expr:
        tok = self.peek()
        if tok.kind == "NUMBER":
            self.advance()
            return NumberLit(tok.span, tok.value)
        if tok.kind == "TEXT":
            self.advance()
            return TextLit(tok.span, tok.value)
        if tok.kind == "FLAG":
            self.advance()
            return FlagLit(tok.span, tok.value)
        if tok.kind == "NIL":
            self.advance()
            return NilLit(tok.span)
        if tok.kind == "IDENT":
            self.advance()
            return Name(tok.span, tok.value, False)
        if tok.kind == "GLYPH":
            self.advance()
            return Name(tok.span, tok.value, True)
        if tok.kind == "PUNCT" and tok.value == "(":
            self.advance()
            expr = self.parse_expr()
            if self.at_punct(")"):
                self.advance()
            else:
                self.error("E0102", "missing `)`", self.peek().span)
            return expr
        if tok.kind == "PUNCT" and tok.value == "[":
            return self.parse_list_literal("]")
        if tok.kind == "PUNCT" and tok.value == "⟨":
            return self.parse_list_literal("⟩")
        if tok.kind == "KW" and tok.value == "ASSEMBLY":
            return self.parse_assembly_literal()
        if tok.kind == "KW" and tok.value == "DRAW":
            self.advance()
            name = self.expect_ident()
            return Read(Span(tok.span.start, name.span.end), name.value)
        self._unexpected("an expression")
        raise _Recover()

    def parse_list_literal(self, closer: str) -> ListLit:
        opener = self.advance()
        items: list[ast.Expr] = []
        while True:
            self.skip_seps()
            if self.at_eof():
                self.error("E0102", f"missing `{closer}` to close this list", self.peek().span)
                break
            if self.at_punct(closer):
                end = self.advance()
                return ListLit(Span(opener.span.start, end.span.end), items)
            items.append(self.parse_expr())
            if self.at_punct(","):
                self.advance()
                continue
            self.skip_seps()
            if self.at_punct(closer):
                continue
            if self._starts_implicit_arg(self.peek()):
                continue
            self.error("E0103", f"expected `,` or `{closer}` in this list", self.peek().span)
            raise _Recover()
        # EOF while reading a list
        if items:
            return ListLit(Span(opener.span.start, self.peek().span.end), items)
        raise _Recover()

    def parse_assembly_literal(self) -> AssemblyLit:
        tok = self.advance()
        if not self.at_punct("{"):
            self.error("E0100", "expected `{` after `assembly`", self.peek().span)
            raise _Recover()
        self.advance()
        fields: list[FieldInit] = []
        while True:
            self.skip_seps()
            if self.at_eof():
                self.error("E0102", "missing `}` to close this assembly", self.peek().span)
                break
            if self.at_punct("}"):
                end = self.advance()
                return AssemblyLit(Span(tok.span.start, end.span.end), fields)
            name = self._key_name()
            if self.at_punct(":") or self.at_punct("="):
                self.advance()
            value = self.parse_expr()
            fields.append(FieldInit(Span(name.span.start, value.span.end), name.value, value))
            if self.at_punct(","):
                self.advance()
        return AssemblyLit(Span(tok.span.start, self.peek().span.end), fields)


def parse_tokens(
    tokens: list[Token],
    source: str = "",
    filename: str = "<source>",
    table: GlyphTable = TABLE,
) -> tuple[Program, list[Diagnostic]]:
    return Parser(tokens, source, filename, table).parse_program()


def parse(
    source: str, filename: str = "<source>", table: GlyphTable = TABLE
) -> tuple[Program, list[Diagnostic]]:
    tokens, diagnostics = tokenize(source, filename)
    program, parse_diagnostics = parse_tokens(tokens, source, filename, table)
    return program, diagnostics + parse_diagnostics
