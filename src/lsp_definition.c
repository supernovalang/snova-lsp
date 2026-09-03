#include "lsp_definition.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

char *lsp_definition_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspPosition pos) {
    if (!doc) return NULL;
    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, store, doc);
    }
    if (!a) return NULL;

    uint32_t offset = lsp_pos_to_offset(doc, pos);
    const SnToken *tok = lsp_find_token_at(a, offset);
    if (!tok || (tok->kind != SN_TOK_IDENT && !sn_tok_is_keyword(tok->kind))) {
        return NULL;
    }

    const char *name = NULL;
    const SnSymbol *sym = lsp_find_symbol_at(a, doc, offset, &name);

    const char *target_path = NULL;
    SnSpan target_span = {0};

    if (sym && sym->decl) {
        target_span = sym->decl->span;
        target_path = sym->origin ? sym->origin->path : doc->path;
    } else if (sym) {
        target_span = sym->span;
        target_path = sym->origin ? sym->origin->path : doc->path;
    } else {
        // Check if token matches declaration in current unit
        const SnDecl *d = lsp_find_decl_at(a, offset);
        if (d && d->name && tok->text && strcmp(d->name, tok->text) == 0) {
            target_span = d->span;
            target_path = doc->path;
        }
    }

    if (!target_path || target_span.line == 0) {
        return NULL;
    }

    char *target_uri = lsp_path_to_uri(target_path);
    if (!target_uri) return NULL;

    LspDocument *target_doc = lsp_docstore_get_by_path(store, target_path);
    LspRange r = lsp_span_to_range(target_doc, target_span.offset, target_span.len, target_span.line, target_span.col);

    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    jb_kv_str(&jb, "uri", target_uri);
    
    jb_key(&jb, "range");
    jb_start_obj(&jb);
    jb_key(&jb, "start");
    jb_start_obj(&jb);
    jb_kv_int(&jb, "line", r.start.line);
    jb_kv_int(&jb, "character", r.start.character);
    jb_end_obj(&jb);
    jb_key(&jb, "end");
    jb_start_obj(&jb);
    jb_kv_int(&jb, "line", r.end.line);
    jb_kv_int(&jb, "character", r.end.character);
    jb_end_obj(&jb);
    jb_end_obj(&jb);

    jb_end_obj(&jb);

    free(target_uri);
    return jb_take(&jb);
}
