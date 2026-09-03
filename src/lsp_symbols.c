#include "lsp_symbols.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static void emit_decl_symbol(JsonBuilder *jb, const LspDocument *doc, const SnDecl *d) {
    if (!d || !d->name) return;

    LspSymbolKind sym_kind = LSP_SYMBOL_VARIABLE;
    const char *detail = "";

    switch (d->kind) {
        case SN_DECL_CLASS: sym_kind = LSP_SYMBOL_CLASS; detail = "class"; break;
        case SN_DECL_STRUCT: sym_kind = LSP_SYMBOL_STRUCT; detail = "struct"; break;
        case SN_DECL_INTERFACE: sym_kind = LSP_SYMBOL_INTERFACE; detail = "interface"; break;
        case SN_DECL_ENUM: sym_kind = LSP_SYMBOL_ENUM; detail = "enum"; break;
        case SN_DECL_FUNC: sym_kind = LSP_SYMBOL_FUNCTION; detail = "func"; break;
        case SN_DECL_METHOD: sym_kind = LSP_SYMBOL_METHOD; detail = "method"; break;
        case SN_DECL_FIELD: sym_kind = LSP_SYMBOL_FIELD; detail = "field"; break;
        case SN_DECL_CONST: sym_kind = LSP_SYMBOL_CONSTANT; detail = "const"; break;
        case SN_DECL_VARIANT: sym_kind = LSP_SYMBOL_ENUM_MEMBER; detail = "variant"; break;
        case SN_DECL_TYPEALIAS: sym_kind = LSP_SYMBOL_TYPE_PARAM; detail = "typealias"; break;
        case SN_DECL_EXTENSION: sym_kind = LSP_SYMBOL_MODULE; detail = "extension"; break;
        default: break;
    }

    LspRange r = lsp_span_to_range(doc, d->span.offset, d->span.len, d->span.line, d->span.col);

    jb_start_obj(jb);
    jb_kv_str(jb, "name", d->name);
    jb_kv_int(jb, "kind", (int)sym_kind);
    if (detail[0]) jb_kv_str(jb, "detail", detail);

    jb_key(jb, "range");
    jb_start_obj(jb);
    jb_key(jb, "start");
    jb_start_obj(jb);
    jb_kv_int(jb, "line", r.start.line);
    jb_kv_int(jb, "character", r.start.character);
    jb_end_obj(jb);
    jb_key(jb, "end");
    jb_start_obj(jb);
    jb_kv_int(jb, "line", r.end.line);
    jb_kv_int(jb, "character", r.end.character);
    jb_end_obj(jb);
    jb_end_obj(jb);

    jb_key(jb, "selectionRange");
    jb_start_obj(jb);
    jb_key(jb, "start");
    jb_start_obj(jb);
    jb_kv_int(jb, "line", r.start.line);
    jb_kv_int(jb, "character", r.start.character);
    jb_end_obj(jb);
    jb_key(jb, "end");
    jb_start_obj(jb);
    jb_kv_int(jb, "line", r.start.line);
    jb_kv_int(jb, "character", r.start.character + (uint32_t)strlen(d->name));
    jb_end_obj(jb);
    jb_end_obj(jb);

    // Children members
    bool has_children = (d->members.len > 0 || d->variants.len > 0);
    if (has_children) {
        jb_key(jb, "children");
        jb_start_arr(jb);
        for (size_t i = 0; i < d->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(d->members, const SnDecl, i);
            emit_decl_symbol(jb, doc, m);
        }
        for (size_t i = 0; i < d->variants.len; i++) {
            const SnDecl *v = SN_LIST_AT(d->variants, const SnDecl, i);
            emit_decl_symbol(jb, doc, v);
        }
        jb_end_arr(jb);
    }

    jb_end_obj(jb);
}

char *lsp_document_symbols_query(LspAnalysisEngine *engine, const LspDocument *doc) {
    if (!doc) return NULL;
    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, NULL, doc);
    }
    if (!a || !a->has_ast) return NULL;

    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);

    // Package namespace symbol if any
    if (a->unit.package) {
        LspRange pr = lsp_span_to_range(doc, a->unit.package_span.offset, a->unit.package_span.len, a->unit.package_span.line, a->unit.package_span.col);
        jb_start_obj(&jb);
        jb_kv_str(&jb, "name", a->unit.package);
        jb_kv_int(&jb, "kind", (int)LSP_SYMBOL_PACKAGE);
        jb_kv_str(&jb, "detail", "package");

        jb_key(&jb, "range");
        jb_start_obj(&jb);
        jb_key(&jb, "start");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", pr.start.line);
        jb_kv_int(&jb, "character", pr.start.character);
        jb_end_obj(&jb);
        jb_key(&jb, "end");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", pr.end.line);
        jb_kv_int(&jb, "character", pr.end.character);
        jb_end_obj(&jb);
        jb_end_obj(&jb);

        jb_key(&jb, "selectionRange");
        jb_start_obj(&jb);
        jb_key(&jb, "start");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", pr.start.line);
        jb_kv_int(&jb, "character", pr.start.character);
        jb_end_obj(&jb);
        jb_key(&jb, "end");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", pr.end.line);
        jb_kv_int(&jb, "character", pr.end.character);
        jb_end_obj(&jb);
        jb_end_obj(&jb);

        jb_end_obj(&jb);
    }

    for (size_t i = 0; i < a->unit.decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(a->unit.decls, const SnDecl, i);
        emit_decl_symbol(&jb, doc, d);
    }

    jb_end_arr(&jb);
    return jb_take(&jb);
}
