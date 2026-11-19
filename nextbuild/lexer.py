from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from typing import Iterator

from .errors import ParseError


class TokenKind(Enum):
    IDENT = auto()
    STRING = auto()
    NUMBER = auto()
    VARIABLE = auto()
    COLON = auto()
    SLASH = auto()
    LBRACE = auto()
    RBRACE = auto()
    DASH = auto()
    EQUAL = auto()
    TRUE = auto()
    FALSE = auto()
    NEWLINE = auto()
    EOF = auto()


@dataclass(frozen=True, slots=True)
class Token:
    kind: TokenKind
    value: str
    line: int
    column: int


_BOOLEAN = {
    "True": TokenKind.TRUE,
    "False": TokenKind.FALSE,
    "true": TokenKind.TRUE,
    "false": TokenKind.FALSE,
}


class Lexer:
    """Tokenize a .nextbuild document.

    Grammar atoms:
      identifier  = letter / digit / '_' / '+' / '-' / '.' / '*'
      variable    = '@' identifier
      string      = "'" ... "'" | '"' ... '"'
      number      = digit+ ('.' digit+)* | '0x' hex+
    """

    def __init__(self, source: str, *, path: str | None = None):
        self.source = source
        self.path = path
        self.length = len(source)
        self.pos = 0
        self.line = 1
        self.column = 1

    def tokenize(self) -> list[Token]:
        return list(self)

    def __iter__(self) -> Iterator[Token]:
        while True:
            token = self._next_token()
            yield token
            if token.kind is TokenKind.EOF:
                break

    def _peek(self, offset: int = 0) -> str:
        index = self.pos + offset
        if index >= self.length:
            return ""
        return self.source[index]

    def _advance(self) -> str:
        if self.pos >= self.length:
            return ""
        ch = self.source[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return ch

    def _error(self, message: str) -> ParseError:
        return ParseError(message, path=self.path, line=self.line, column=self.column)

    def _skip_whitespace_and_comments(self) -> None:
        while self.pos < self.length:
            ch = self._peek()
            if ch in " \t\r":
                self._advance()
                continue
            if ch == "#":
                while self.pos < self.length and self._peek() != "\n":
                    self._advance()
                continue
            if ch == "/" and self._peek(1) == "/":
                while self.pos < self.length and self._peek() != "\n":
                    self._advance()
                continue
            break

    def _next_token(self) -> Token:
        self._skip_whitespace_and_comments()
        line, column = self.line, self.column

        if self.pos >= self.length:
            return Token(TokenKind.EOF, "", line, column)

        ch = self._peek()

        if ch == "\n":
            self._advance()
            return Token(TokenKind.NEWLINE, "\n", line, column)

        singles = {
            ":": TokenKind.COLON,
            "/": TokenKind.SLASH,
            "{": TokenKind.LBRACE,
            "}": TokenKind.RBRACE,
            "=": TokenKind.EQUAL,
        }
        if ch in singles:
            self._advance()
            return Token(singles[ch], ch, line, column)

        if ch == "-":
            nxt = self._peek(1)
            # list bullet when followed by whitespace / ident start
            if nxt == "" or nxt.isspace() or nxt.isalpha() or nxt == "_":
                self._advance()
                return Token(TokenKind.DASH, "-", line, column)
            # otherwise part of an identifier/number-like token (handled below)

        if ch in "'\"":
            return self._read_string(line, column)

        if ch == "@":
            return self._read_variable(line, column)

        if ch.isdigit() or (ch == "0" and self._peek(1) in "xX"):
            return self._read_number(line, column)

        if self._is_ident_start(ch) or ch in "+-.":
            return self._read_ident_or_keyword(line, column)

        raise self._error(f"unexpected character {ch!r}")

    def _is_ident_start(self, ch: str) -> bool:
        return ch.isalpha() or ch == "_"

    def _is_ident_continue(self, ch: str) -> bool:
        return ch.isalnum() or ch in "_+-.*/"

    def _read_string(self, line: int, column: int) -> Token:
        quote = self._advance()
        chars: list[str] = []
        while self.pos < self.length:
            ch = self._peek()
            if ch == "\n":
                raise self._error("unterminated string literal")
            if ch == "\\":
                self._advance()
                escaped = self._advance()
                escapes = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", "'": "'", '"': '"'}
                chars.append(escapes.get(escaped, escaped))
                continue
            if ch == quote:
                self._advance()
                return Token(TokenKind.STRING, "".join(chars), line, column)
            chars.append(self._advance())
        raise self._error("unterminated string literal")

    def _read_variable(self, line: int, column: int) -> Token:
        self._advance()  # '@'
        if not self._is_ident_start(self._peek()):
            raise self._error("expected identifier after '@'")
        name_chars: list[str] = []
        while self._is_ident_continue(self._peek()) and self._peek() not in "/":
            # variables don't include '/', path join is separate
            ch = self._peek()
            if ch in "+-.*":
                break
            if not (ch.isalnum() or ch == "_"):
                break
            name_chars.append(self._advance())
        return Token(TokenKind.VARIABLE, "".join(name_chars), line, column)

    def _read_number(self, line: int, column: int) -> Token:
        start = self.pos
        if self._peek() == "0" and self._peek(1) in "xX":
            self._advance()
            self._advance()
            if not self._peek() or self._peek() not in "0123456789abcdefABCDEF":
                raise self._error("invalid hexadecimal literal")
            while self._peek() and self._peek() in "0123456789abcdefABCDEF":
                self._advance()
            return Token(TokenKind.NUMBER, self.source[start : self.pos], line, column)

        while self._peek().isdigit():
            self._advance()
        # dotted versions like 0.1.0 or 23.1.3 — treat as NUMBER textually
        while self._peek() == "." and self._peek(1).isdigit():
            self._advance()
            while self._peek().isdigit():
                self._advance()
        return Token(TokenKind.NUMBER, self.source[start : self.pos], line, column)

    def _read_ident_or_keyword(self, line: int, column: int) -> Token:
        start = self.pos
        # Allow tokens like c++2d, x64-next-generic, clang++
        while self.pos < self.length:
            ch = self._peek()
            if ch.isalnum() or ch in "_+-*.":
                self._advance()
                continue
            break
        text = self.source[start : self.pos]
        if not text:
            raise self._error("expected identifier")
        kind = _BOOLEAN.get(text, TokenKind.IDENT)
        return Token(kind, text, line, column)
