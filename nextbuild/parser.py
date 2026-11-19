from __future__ import annotations

from pathlib import Path

from .ast import (
    Assignment,
    BlockExpr,
    BlockItem,
    BoolLit,
    Document,
    Expr,
    Ident,
    NumberLit,
    PathExpr,
    StringLit,
    VariableRef,
)
from .errors import ParseError
from .lexer import Lexer, Token, TokenKind


class Parser:
    def __init__(self, tokens: list[Token], *, path: str | None = None):
        self.tokens = tokens
        self.path = path
        self.index = 0

    @classmethod
    def parse_file(cls, path: str | Path) -> Document:
        file_path = Path(path)
        text = file_path.read_text(encoding="utf-8-sig")
        return cls.parse_source(text, path=str(file_path))

    @classmethod
    def parse_source(cls, source: str, *, path: str | None = None) -> Document:
        # Tolerate BOM from editors on Windows.
        if source.startswith("\ufeff"):
            source = source[1:]
        tokens = Lexer(source, path=path).tokenize()
        return cls(tokens, path=path).parse()

    def parse(self) -> Document:
        statements: list[Assignment] = []
        self._skip_newlines()
        while not self._check(TokenKind.EOF):
            statements.append(self._parse_assignment())
            self._skip_newlines()
        return Document(path=self.path, statements=statements)

    def _at(self) -> Token:
        return self.tokens[self.index]

    def _check(self, kind: TokenKind) -> bool:
        return self._at().kind is kind

    def _advance(self) -> Token:
        token = self._at()
        if token.kind is not TokenKind.EOF:
            self.index += 1
        return token

    def _expect(self, kind: TokenKind, message: str) -> Token:
        if not self._check(kind):
            raise self._error(message)
        return self._advance()

    def _error(self, message: str) -> ParseError:
        token = self._at()
        return ParseError(message, path=self.path, line=token.line, column=token.column)

    def _skip_newlines(self) -> None:
        while self._check(TokenKind.NEWLINE):
            self._advance()

    def _parse_assignment(self) -> Assignment:
        token = self._at()
        is_variable = False
        if self._check(TokenKind.VARIABLE):
            target = self._advance().value
            is_variable = True
        elif self._check(TokenKind.IDENT):
            target = self._advance().value
        else:
            raise self._error("expected directive name or @variable")

        self._expect(TokenKind.COLON, "expected ':' after assignment target")
        value = self._parse_expression()
        return Assignment(
            line=token.line,
            column=token.column,
            target=target,
            is_variable=is_variable,
            value=value,
        )

    def _parse_expression(self) -> Expr:
        if self._check(TokenKind.LBRACE):
            return self._parse_block()
        return self._parse_path()

    def _parse_path(self) -> Expr:
        parts = [self._parse_atom()]
        while self._check(TokenKind.SLASH):
            self._advance()
            parts.append(self._parse_atom())
        if len(parts) == 1:
            return parts[0]
        first = parts[0]
        return PathExpr(line=first.line, column=first.column, parts=parts)

    def _parse_atom(self) -> Expr:
        token = self._at()
        if self._check(TokenKind.STRING):
            self._advance()
            return StringLit(line=token.line, column=token.column, value=token.value)
        if self._check(TokenKind.NUMBER):
            self._advance()
            return NumberLit(line=token.line, column=token.column, raw=token.value)
        if self._check(TokenKind.TRUE):
            self._advance()
            return BoolLit(line=token.line, column=token.column, value=True)
        if self._check(TokenKind.FALSE):
            self._advance()
            return BoolLit(line=token.line, column=token.column, value=False)
        if self._check(TokenKind.VARIABLE):
            self._advance()
            return VariableRef(line=token.line, column=token.column, name=token.value)
        if self._check(TokenKind.IDENT):
            self._advance()
            return Ident(line=token.line, column=token.column, name=token.value)
        raise self._error("expected value expression")

    def _parse_block(self) -> BlockExpr:
        lbrace = self._expect(TokenKind.LBRACE, "expected '{'")
        items: list[BlockItem] = []
        self._skip_newlines()
        while not self._check(TokenKind.RBRACE):
            if self._check(TokenKind.EOF):
                raise self._error("unterminated block, expected '}'")
            items.append(self._parse_block_item())
            self._skip_newlines()
        self._advance()
        return BlockExpr(line=lbrace.line, column=lbrace.column, items=items)

    def _parse_block_item(self) -> BlockItem:
        dash = self._expect(TokenKind.DASH, "expected '-' list item in block")
        key_token = self._expect(TokenKind.IDENT, "expected item name after '-'")
        self._expect(TokenKind.EQUAL, "expected '=' after item name")
        value = self._parse_expression()
        return BlockItem(
            line=dash.line,
            column=dash.column,
            key=key_token.value,
            value=value,
        )
