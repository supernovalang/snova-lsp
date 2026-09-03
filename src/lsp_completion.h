#ifndef SNOVA_LSP_COMPLETION_H
#define SNOVA_LSP_COMPLETION_H

#include "lsp_protocol.h"
#include "lsp_document.h"
#include "lsp_analysis.h"

/* Generates JSON-RPC result for textDocument/completion. Returns formatted JSON string. */
char *lsp_completion_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspPosition pos);

#endif /* SNOVA_LSP_COMPLETION_H */
