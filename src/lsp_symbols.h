#ifndef SNOVA_LSP_SYMBOLS_H
#define SNOVA_LSP_SYMBOLS_H

#include "lsp_protocol.h"
#include "lsp_document.h"
#include "lsp_analysis.h"

/* Generates JSON-RPC result for textDocument/documentSymbol. Returns formatted JSON string or NULL. */
char *lsp_document_symbols_query(LspAnalysisEngine *engine, const LspDocument *doc);

#endif /* SNOVA_LSP_SYMBOLS_H */
