from __future__ import annotations

import argparse
import sys
import traceback
from pathlib import Path
from typing import Any

from .analyze import DocAnalysis, analyze_document, find_main_root, uri_to_path
from .complete import completions, definition, hover
from .rpc import JsonRpcStdio, Request


class NextBuildLanguageServer:
    def __init__(self) -> None:
        self.rpc = JsonRpcStdio()
        self.documents: dict[str, str] = {}
        self.main_root = find_main_root()
        self._shutdown = False

    def run(self) -> int:
        while True:
            message = self.rpc.read_message()
            if message is None:
                return 0
            if "method" not in message:
                continue
            request = Request(
                id=message.get("id"),
                method=message["method"],
                params=message.get("params"),
            )
            try:
                self._dispatch(request)
            except Exception as exc:  # noqa: BLE001 — never crash the LSP loop
                if request.id is not None:
                    self.rpc.respond_error(request.id, -32603, f"{type(exc).__name__}: {exc}")
                traceback.print_exc(file=sys.stderr)
            if self._shutdown and request.method == "exit":
                return 0

    def _dispatch(self, request: Request) -> None:
        method = request.method
        params = request.params or {}

        if method == "initialize":
            root = params.get("rootUri") or params.get("workspaceFolders", [{}])[0].get("uri")
            if root:
                self.main_root = find_main_root(uri_to_path(root))
            self.rpc.respond(
                request.id,
                {
                    "capabilities": {
                        "textDocumentSync": 1,  # Full
                        "completionProvider": {
                            "triggerCharacters": ["@", "/", "'", '"', ":", "-", " "],
                            "resolveProvider": False,
                        },
                        "hoverProvider": True,
                        "definitionProvider": True,
                        "documentSymbolProvider": True,
                    },
                    "serverInfo": {"name": "nextbuild-ls", "version": "0.1.0"},
                },
            )
            return

        if method == "initialized":
            return

        if method == "shutdown":
            self._shutdown = True
            self.rpc.respond(request.id, None)
            return

        if method == "exit":
            return

        if method == "workspace/didChangeConfiguration":
            return

        if method == "textDocument/didOpen":
            doc = params["textDocument"]
            self.documents[doc["uri"]] = doc["text"]
            self._publish_diagnostics(doc["uri"])
            return

        if method == "textDocument/didChange":
            uri = params["textDocument"]["uri"]
            changes = params.get("contentChanges") or []
            if changes:
                # Full sync
                self.documents[uri] = changes[-1]["text"]
                self._publish_diagnostics(uri)
            return

        if method == "textDocument/didClose":
            uri = params["textDocument"]["uri"]
            self.documents.pop(uri, None)
            self.rpc.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": []})
            return

        if method == "textDocument/completion":
            uri = params["textDocument"]["uri"]
            pos = params["position"]
            analysis = self._analysis(uri)
            items = completions(
                analysis,
                line=pos["line"],
                character=pos["character"],
                main_root=self.main_root,
            )
            self.rpc.respond(request.id, items)
            return

        if method == "textDocument/hover":
            uri = params["textDocument"]["uri"]
            pos = params["position"]
            analysis = self._analysis(uri)
            self.rpc.respond(
                request.id,
                hover(analysis, line=pos["line"], character=pos["character"]),
            )
            return

        if method == "textDocument/definition":
            uri = params["textDocument"]["uri"]
            pos = params["position"]
            analysis = self._analysis(uri)
            self.rpc.respond(
                request.id,
                definition(
                    analysis,
                    line=pos["line"],
                    character=pos["character"],
                    uri=uri,
                ),
            )
            return

        if method == "textDocument/documentSymbol":
            uri = params["textDocument"]["uri"]
            analysis = self._analysis(uri)
            symbols = []
            for name, var in analysis.variables.items():
                symbols.append(
                    {
                        "name": f"@{name}",
                        "kind": 13,  # Variable
                        "range": {
                            "start": {"line": var.line, "character": var.character},
                            "end": {
                                "line": var.line,
                                "character": var.character + len(name) + 1,
                            },
                        },
                        "selectionRange": {
                            "start": {"line": var.line, "character": var.character},
                            "end": {
                                "line": var.line,
                                "character": var.character + len(name) + 1,
                            },
                        },
                    }
                )
            self.rpc.respond(request.id, symbols)
            return

        if request.id is not None:
            self.rpc.respond_error(request.id, -32601, f"method not found: {method}")

    def _analysis(self, uri: str) -> DocAnalysis:
        text = self.documents.get(uri, "")
        path = str(uri_to_path(uri))
        return analyze_document(text, path=path)

    def _publish_diagnostics(self, uri: str) -> None:
        analysis = self._analysis(uri)
        self.rpc.notify(
            "textDocument/publishDiagnostics",
            {"uri": uri, "diagnostics": analysis.diagnostics},
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="nextbuild language server")
    parser.add_argument("--stdio", action="store_true", default=True, help="use stdio (default)")
    parser.parse_args(argv)
    # Ensure tools/ is importable when launched as python -m nextbuild.lsp
    tools = Path(__file__).resolve().parents[2]
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    return NextBuildLanguageServer().run()


if __name__ == "__main__":
    raise SystemExit(main())
