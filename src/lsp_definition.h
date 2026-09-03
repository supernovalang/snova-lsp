#ifndef SNOVA_LSP_DEFINITION_H
#define SNOVA_LSP_DEFINITION_H

#include "lsp_protocol.h"
#include "lsp_document.h"
#include "lsp_analysis.h"

/* Generates JSON-RPC result for textDocument/definition. Returns formatted JSON string or NULL. */
char *lsp_definition_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspPosition pos);

#endif /* SNOVA_LSP_DEFINITION_H */
