#ifndef SNOVA_LSP_REFERENCES_H
#define SNOVA_LSP_REFERENCES_H
#include "lsp_analysis.h"
char *lsp_references_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition);
char *lsp_implementation_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition);
#endif
