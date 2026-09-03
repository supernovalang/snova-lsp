#ifndef SNOVA_LSP_CODE_ACTION_H
#define SNOVA_LSP_CODE_ACTION_H

#include "lsp_protocol.h"
#include "lsp_document.h"
#include "lsp_analysis.h"
#include "json.h"

/* Generates JSON-RPC result for textDocument/codeAction. Returns formatted JSON string. */
char *lsp_code_action_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspRange range, const JsonVal *context_diags);

#endif /* SNOVA_LSP_CODE_ACTION_H */
