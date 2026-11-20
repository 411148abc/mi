"""The MI lexer.

The lexer is driven by ``规格/glyphs.json``: every keyword, operator, literal
and symbol-only builtin spelling is matched by longest-match, word spellings
case-insensitively and only at word boundaries.

It never raises: errors are collected as diagnostics and lexing continues, so a
single scan can report several problems.
"""
from __future__ import annotations

from .errors import Diagnostic, DiagnosticBag, suggest
from .glyphs import TABLE, GlyphTable
from .tokens import EOF, PUNCTUATION, SEP, Position, Span, Token

WORD_ESCAPES = {
    "n": "\n",
    "t": "\t",
    "r": "\r",
    "\\": "\\",
    '"': '"',
    "'": "'",
    "0": "\0",
    "«": "«",
    "»": "»",
}

QUOTE_CLOSERS = {'"': '"', "'": "'", "«": "»"}


class Lexer:
    def __init__(self, source: str, filename: str = "<source>", table: GlyphTable = TABLE):
        self.source = source
        self.filename = filename
        self.table = table
        self.pos = 0
        self.line = 1
        self.col = 1
        self.tokens: list[Token] = []
        self.bag = DiagnosticBag(filename=filename)

    # ------------------------------------------------------------------ pos
    def _here(self) -> Position:
        return Position(self.line, self.col, self.pos)

    def _peek(self, k: int = 0) -> str:
        idx = self.pos + k
        return self.source[idx] if idx < len(self.source) else ""

    def _advance(self) -> str:
        ch = self.source[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def _word_char(self, ch: str) -> bool:
        return bool(ch) and (ch.isalnum() or ch == "_")

    # -------------------------------------------------------------- helpers
    def _emit(self, kind: str, value: object, start: Position) -> None:
        text = self.source[start.offset : self.pos]
        span = Span(start, self._here())
        token = Token(kind, value, text, span)
        if kind == SEP and self.tokens and self.tokens[-1].kind == SEP:
            return
        self.tokens.append(token)

    def _error(self, code: str, message: str, start: Position, hint: str | None = None) -> None:
        span = Span(start, self._here() if self.pos > start.offset else Position(start.line, start.col, start.offset + 1))
        self.bag.error(code, message, span, hint)

    # ----------------------------------------------------------------- lex
    def lex(self) -> tuple[list[Token], list[Diagnostic]]:
        while self.pos < len(self.source):
            ch = self._peek()
            if ch in " \t\r":
                self._advance()
                continue
            if ch == "\n":
                start = self._here()
                self._advance()
                self._emit(SEP, "\n", start)
                continue
            if self._peek() == ";" and self._peek(1) == ";":
                self._skip_line_comment()
                continue
            if ch == ";":
                start = self._here()
                self._advance()
                self._emit(SEP, ";", start)
                continue
            if ch == "#":
                self._skip_line_comment()
                continue
            if self._peek() == "-" and self._peek(1) == "-":
                self._skip_line_comment()
                continue
            if self._peek() == "/" and self._peek(1) == "*":
                self._block_comment("/*", "*/")
                continue
            if self._peek() == "(" and self._peek(1) == "*":
                self._block_comment("(*", "*)")
                continue
            if ch in QUOTE_CLOSERS:
                self._lex_text()
                continue
            if ch.isdigit() or (ch == "." and self._peek(1).isdigit()):
                self._lex_number()
                continue
            if self._match_spelling():
                continue
            if ch in PUNCTUATION:
                start = self._here()
                self._advance()
                self._emit("PUNCT", ch, start)
                continue
            if self._word_char(ch):
                self._lex_identifier()
                continue
            start = self._here()
            self._advance()
            close = suggest(ch, self.table.suggestion_words, n=1)
            hint = f"maybe you meant `{close[0]}`?" if close else None
            self._error("E0001", f"unexpected character {ch!r}", start, hint)

        if not self.tokens or self.tokens[-1].kind != SEP:
            here = self._here()
            self.tokens.append(Token(SEP, "\n", "", Span(here, here)))
        here = self._here()
        self.tokens.append(Token(EOF, None, "", Span(here, here)))
        return self.tokens, self.bag.diagnostics

    # ----------------------------------------------------------- comments
    def _skip_line_comment(self) -> None:
        while self.pos < len(self.source) and self._peek() != "\n":
            self._advance()

    def _block_comment(self, opener: str, closer: str) -> None:
        start = self._here()
        self._advance()
        self._advance()
        while self.pos < len(self.source):
            if self._peek() == closer[0] and self._peek(1) == closer[1]:
                self._advance()
                self._advance()
                return
            self._advance()
        self._error("E0002", f"unterminated block comment, expected {closer!r}", start)

    # ------------------------------------------------------------- texts
    def _lex_text(self) -> None:
        start = self._here()
        opener = self._advance()
        closer = QUOTE_CLOSERS[opener]
        value: list[str] = []
        while True:
            if self.pos >= len(self.source):
                self._error("E0002", "unterminated text literal", start)
                break
            ch = self._advance()
            if ch == closer:
                break
            if ch == "\\":
                if self.pos >= len(self.source):
                    self._error("E0003", "text literal ends with a dangling escape", start)
                    break
                esc = self._advance()
                if esc in WORD_ESCAPES:
                    value.append(WORD_ESCAPES[esc])
                elif esc == "u":
                    if self._peek() != "{":
                        self._error("E0003", "expected `{` after `\\u`", start)
                        continue
                    self._advance()
                    digits = ""
                    while self.pos < len(self.source) and self._peek() != "}":
                        digits += self._advance()
                    if self._peek() == "}":
                        self._advance()
                    try:
                        value.append(chr(int(digits, 16)))
                    except ValueError:
                        self._error("E0003", f"bad unicode escape `\\u{{{digits}}}`", start)
                elif esc == "\n":
                    continue
                else:
                    self._error("E0003", f"unknown escape `\\{esc}`", start, "use \\\\ for a backslash")
                    value.append(esc)
            else:
                value.append(ch)
        self._emit("TEXT", "".join(value), start)

    # ------------------------------------------------------------ numbers
    def _lex_number(self) -> None:
        start = self._here()
        if self._peek() == "0" and self._peek(1) in ("x", "X"):
            self._advance()
            self._advance()
            digits = ""
            while self._peek().isalnum() or self._peek() == "_":
                digits += self._advance()
            clean = digits.replace("_", "")
            try:
                value: object = int(clean, 16)
            except ValueError:
                self._error("E0004", f"bad hexadecimal number `0x{digits}`", start)
                value = 0
            self._emit("NUMBER", value, start)
            return
        if self._peek() == "0" and self._peek(1) in ("b", "B"):
            self._advance()
            self._advance()
            digits = ""
            while self._peek().isalnum() or self._peek() == "_":
                digits += self._advance()
            clean = digits.replace("_", "")
            try:
                value = int(clean, 2)
            except ValueError:
                self._error("E0004", f"bad binary number `0b{digits}`", start)
                value = 0
            self._emit("NUMBER", value, start)
            return

        text = ""
        seen_dot = False
        while True:
            ch = self._peek()
            if ch.isdigit() or (ch == "_" and text and self._peek(1).isdigit()):
                text += self._advance()
            elif ch == "." and not seen_dot and self._peek(1).isdigit():
                seen_dot = True
                text += self._advance()
            else:
                break
        if self._peek() in ("e", "E"):
            save = (self.pos, self.line, self.col)
            exp = self._advance()
            if self._peek() in ("+", "-"):
                exp += self._advance()
            digits = ""
            while self._peek().isdigit() or self._peek() == "_":
                digits += self._advance()
            if digits:
                text += exp + digits
            else:
                self.pos, self.line, self.col = save
                self._error("E0004", "bad number: exponent has no digits", start)
        clean = text.replace("_", "")
        try:
            value = float(clean) if ("." in clean or "e" in clean or "E" in clean) else int(clean, 10)
        except ValueError:
            self._error("E0004", f"bad number `{text}`", start)
            value = 0
        self._emit("NUMBER", value, start)

    # -------------------------------------------------------- identifiers
    def _lex_identifier(self) -> None:
        start = self._here()
        text = ""
        while self._word_char(self._peek()):
            text += self._advance()
        self._emit("IDENT", text, start)

    # ------------------------------------------------------- spelling match
    def _match_spelling(self) -> bool:
        source = self.source
        start_pos = self.pos
        for spelling, is_word, kind, value in self.table.match_entries:
            length = len(spelling)
            if start_pos + length > len(source):
                continue
            window = source[start_pos : start_pos + length]
            if is_word:
                if window.lower() != spelling:
                    continue
                after = source[start_pos + length] if start_pos + length < len(source) else ""
                if self._word_char(after):
                    continue
                if start_pos > 0 and self._word_char(source[start_pos - 1]):
                    continue
            else:
                if window != spelling:
                    continue
            start = self._here()
            for _ in range(length):
                self._advance()
            if kind == "LITERAL":
                literal = self.table.literal(str(value))
                if literal is None:  # pragma: no cover - table is consistent
                    continue
                if literal.kind == "flag":
                    self._emit("FLAG", literal.value, start)
                elif literal.kind == "nil":
                    self._emit("NIL", None, start)
                else:  # pragma: no cover
                    self._emit("TEXT", literal.value, start)
            else:
                self._emit(kind, value, start)
            return True
        return False


def tokenize(source: str, filename: str = "<source>") -> tuple[list[Token], list[Diagnostic]]:
    return Lexer(source, filename).lex()
