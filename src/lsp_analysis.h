#ifndef SNOVA_LSP_ANALYSIS_H
#define SNOVA_LSP_ANALYSIS_H

#include "lsp_protocol.h"
#include "lsp_document.h"

// snovac compiler headers
#include "arena.h"
#include "intern.h"
#include "token.h"
#include "lex.h"
#include "ast.h"
#include "parse.h"
#include "symbol.h"
#include "types.h"
#include "builtins.h"
#include "package.h"
#include "resolve.h"
#include "check.h"
#include "cmd_check.h"
#include "project.h"
#include "driver_utils.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int code;
    SnDiagLevel level;
    SnSpan span;
    char *message;
    char *file_path;
} CapturedDiag;

typedef struct {
    CapturedDiag *items;
    size_t len;
    size_t cap;
} CapturedDiagList;

typedef struct LspDocAnalysis LspDocAnalysis;

struct LspDocAnalysis {
    char *uri;
    char *path;
    int version;

    SnArena arena;
    SnInternTable intern;
    SnDiagSink diag;
    SnTokenVec tokens;
    SnUnit unit;
    
    SnPackageGraph graph;
    SnTypeTable types;
    SnResolver resolver;
    SnChecker checker;

    CapturedDiagList diags;
    bool has_ast;
    bool has_resolved;

    LspDocAnalysis *next;
};

typedef struct {
    LspDocAnalysis *analyses;
    char workspace_root[SNOVAC_PATH_MAX];
    char builtin_dir[SNOVAC_PATH_MAX];
} LspAnalysisEngine;

void lsp_engine_init(LspAnalysisEngine *engine, const char *workspace_root);
void lsp_engine_destroy(LspAnalysisEngine *engine);
void lsp_engine_set_workspace_root(LspAnalysisEngine *engine, const char *workspace_root);

/* Analyzes document, updates cache, and returns analysis snapshot. */
LspDocAnalysis *lsp_engine_analyze_document(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc);

/* Analyzes entire workspace and returns comprehensive project analysis snapshot. */
LspDocAnalysis *lsp_engine_analyze_workspace(LspAnalysisEngine *engine, LspDocStore *store);

/* Retrieves cached analysis */
LspDocAnalysis *lsp_engine_get_analysis(LspAnalysisEngine *engine, const char *uri);

/* Invalidate / free analysis for closed doc */
void lsp_engine_remove_analysis(LspAnalysisEngine *engine, const char *uri);

/* Analyzes a snova-manifest file (mod.sno) and returns diagnostics-only analysis.
   The returned object is owned by the engine and freed on the next call or engine destroy. */
LspDocAnalysis *lsp_engine_analyze_manifest(LspAnalysisEngine *engine, const LspDocument *doc);

/* Search / AST helpers */
const SnToken *lsp_find_token_at(const LspDocAnalysis *a, uint32_t offset);
const SnDecl *lsp_find_decl_at(const LspDocAnalysis *a, uint32_t offset);
const SnExpr *lsp_find_expr_at(const LspDocAnalysis *a, uint32_t offset);
const SnStmt *lsp_find_stmt_at(const LspDocAnalysis *a, uint32_t offset);
const SnSymbol *lsp_find_symbol_at(const LspDocAnalysis *a, const LspDocument *doc, uint32_t offset, const char **out_name);

/* Scope & Semantic Type Inference helpers */
SnScope *lsp_build_scope_at(const LspDocAnalysis *a, SnChecker *checker, uint32_t offset, const SnDecl **out_enclosing_decl, const SnDecl **out_enclosing_type);
SnTypeRep *lsp_infer_expr_type_at(const LspDocAnalysis *a, SnChecker *checker, SnScope *local, const char *expr_str);

#endif /* SNOVA_LSP_ANALYSIS_H */
