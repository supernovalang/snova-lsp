#ifndef SNOVA_LSP_HOVER_H
#define SNOVA_LSP_HOVER_H

#include "lsp_protocol.h"
#include "lsp_document.h"
#include "lsp_analysis.h"
#include "json.h"

/* Generates JSON-RPC result for textDocument/hover. Returns formatted JSON string or NULL. */
char *lsp_hover_query(LspAnalysisEngine *engine, const LspDocument *doc, LspPosition pos);

#endif /* SNOVA_LSP_HOVER_H */
