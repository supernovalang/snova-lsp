#include "lsp_references.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *symbol_name(LspAnalysisEngine *e, LspDocStore *s,
                               const LspDocument *doc, LspPosition pos) {
    LspDocAnalysis *a = lsp_engine_get_analysis(e, doc->uri);
    if (!a) a = lsp_engine_analyze_document(e, s, doc);
    if (!a) return NULL;
    const SnToken *t = lsp_find_token_at(a, lsp_pos_to_offset(doc, pos));
    return t && t->kind == SN_TOK_IDENT ? t->text : NULL;
}

static char *find_locations(LspAnalysisEngine *e, LspDocStore *store,
                            const LspDocument *doc, LspPosition pos, bool implementations) {
    const char *name = symbol_name(e, store, doc, pos);
    if (!name) return NULL;
    JsonBuilder jb; jb_init(&jb); jb_start_arr(&jb);
    for (size_t d = 0; d < store->len; d++) {
        LspDocument *cur = store->docs[d];
        LspDocAnalysis *a = lsp_engine_get_analysis(e, cur->uri);
        if (!a) a = lsp_engine_analyze_document(e, store, cur);
        if (!a || !a->has_ast) continue;
        for (size_t i = 0; i < a->tokens.len; i++) {
            const SnToken *t = &a->tokens.data[i];
            if (t->kind != SN_TOK_IDENT || strcmp(t->text, name) != 0) continue;
            if (implementations) {
                const SnDecl *decl = lsp_find_decl_at(a, t->span.offset);
                if (!decl || decl->kind != SN_DECL_METHOD) continue;
            }
            LspRange r = lsp_span_to_range(cur, t->span.offset, t->span.len, t->span.line, t->span.col);
            jb_start_obj(&jb); jb_kv_str(&jb, "uri", cur->uri);
            jb_key(&jb, "range"); jb_start_obj(&jb);
            jb_key(&jb, "start"); jb_start_obj(&jb);
            jb_kv_int(&jb, "line", r.start.line); jb_kv_int(&jb, "character", r.start.character); jb_end_obj(&jb);
            jb_key(&jb, "end"); jb_start_obj(&jb);
            jb_kv_int(&jb, "line", r.end.line); jb_kv_int(&jb, "character", r.end.character); jb_end_obj(&jb);
            jb_end_obj(&jb); jb_end_obj(&jb);
        }
    }
    jb_end_arr(&jb); return jb_take(&jb);
}

char *lsp_references_query(LspAnalysisEngine *e, LspDocStore *s, const LspDocument *d, LspPosition p) {
    return find_locations(e, s, d, p, false);
}
char *lsp_implementation_query(LspAnalysisEngine *e, LspDocStore *s, const LspDocument *d, LspPosition p) {
    return find_locations(e, s, d, p, true);
}
