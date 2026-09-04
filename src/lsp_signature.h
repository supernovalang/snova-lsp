#ifndef SNOVA_LSP_SIGNATURE_H
#define SNOVA_LSP_SIGNATURE_H

#include "lsp_analysis.h"

char *lsp_signature_query(LspAnalysisEngine *engine, LspDocStore *store,
                          const LspDocument *doc, LspPosition pos);

#endif
