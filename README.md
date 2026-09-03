# snova-lsp — Snovalang Language Server Protocol

IDE-agnostic Language Server Protocol (LSP) server for Snovalang based on the `snovac` compiler pipeline and Microsoft LSP 3.17 specification.

## Features
- **Real-Time Diagnostics (`textDocument/publishDiagnostics`)**: Compile errors, syntax errors, and type warnings with SNOVA diagnostic codes.
- **Hover Documentation (`textDocument/hover`)**: Rich Markdown tooltips with syntax signatures and parsed `/* -- Doc:{Name} */` documentation.
- **Auto-Completion (`textDocument/completion`)**: Keywords, primitives, prelude types, local variables, and member access.
- **Go to Definition (`textDocument/definition`)**: Jump to declaration site across local scope, members, and project packages.
- **Document Symbols (`textDocument/documentSymbol`)**: Hierarchical outline view of classes, structs, enums, functions, and methods.

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
