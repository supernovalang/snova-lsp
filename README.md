# snova-lsp — Snovalang Language Server Protocol

IDE-agnostic Language Server Protocol (LSP) server for Snovalang based on the `snovac` compiler pipeline and Microsoft LSP 3.17 specification.

## Features
- **Real-Time Diagnostics (`textDocument/publishDiagnostics`)**: Compile errors, syntax errors, and type warnings with SNOVA diagnostic codes.
- **Hover Documentation (`textDocument/hover`)**: Rich Markdown tooltips with syntax signatures and parsed `/* -- Doc:{Name} */` documentation.
- **Auto-Completion (`textDocument/completion`)**: Keywords, primitives, prelude types, local variables, and member access.
- **Go to Definition (`textDocument/definition`)**: Jump to declaration site across local scope, members, and project packages.
- **Document Symbols (`textDocument/documentSymbol`)**: Hierarchical outline view of classes, structs, enums, functions, and methods.
- **References and Implementations**: Navigate usages and method implementations across open workspace files.
- **Semantic Highlighting**: Token highlighting, including expressions embedded in interpolated strings.
- **Pulsar completions**: Templates for `pulsar func` and `pulsar method`, with async stream-aware signatures.
- **Package imports**: Resolves package and qualified-symbol imports for completion and navigation.

## Windows

Run `make` from a shell with the Snovalang compiler available at `../snovac`.
The executable is emitted to `tools/bin/snova-lsp.exe`, which is the location
used by the Snovalang editor extension. Add that directory to `PATH` when
launching the server manually.

## Docstring Support
The LSP server automatically parses documentation formatted as:
```snova
/* -- Doc:{FuncName}
 *
 * -- Description: Documentation description text.
 *
 * -- Param{paramName}: Parameter description.
 * -- Returns: Return description.
 */
```
