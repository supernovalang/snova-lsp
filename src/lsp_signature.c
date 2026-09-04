#include "lsp_signature.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static void type_text(const SnType *t, char *out, size_t n) {
    if (!t) { snprintf(out, n, "unit"); return; }
    if (t->kind == SN_TYPE_NAME) snprintf(out, n, "%s%s", t->name ? t->name : "any", t->is_optional ? "?" : "");
    else if (t->kind == SN_TYPE_FUNC) {
        size_t used = (size_t)snprintf(out, n, "(");
        for (size_t i = 0; i < t->params.len && used < n; i++) {
            char param[96];
            type_text(SN_LIST_AT(t->params, const SnType, i), param, sizeof(param));
            if (i) used += (size_t)snprintf(out + used, n - used, ", ");
            used += (size_t)snprintf(out + used, n - used, "%s", param);
        }
        if (used < n) snprintf(out + used, n - used, ")");
    } else {
        snprintf(out, n, "unknown");
    }
}

static SnDecl *find_method(const LspDocAnalysis *a, const char *receiver,
                           const char *name) {
    for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
        if (te->type_decl && te->type_decl->name && strcmp(te->type_decl->name, receiver) == 0) {
            SnSymbol *s = sn_scope_lookup_local(te->member_scope,
                sn_intern_cstr((SnInternTable *)&a->intern, name));
            if (s && s->decl && s->kind == SN_SYM_METHOD) return (SnDecl *)s->decl;
        }
    }
    return NULL;
}

char *lsp_signature_query(LspAnalysisEngine *engine, LspDocStore *store,
                          const LspDocument *doc, LspPosition pos) {
    (void)store;
    if (!doc) return NULL;
    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) a = lsp_engine_analyze_document(engine, store, doc);
    if (!a) return NULL;
    uint32_t off = lsp_pos_to_offset(doc, pos);
    size_t begin = off;
    while (begin > 0 && doc->text[begin - 1] != '\n' && doc->text[begin - 1] != '\r') begin--;
    size_t open = off;
    while (open > begin && doc->text[open - 1] != '(') open--;
    if (open == begin) return NULL;
    size_t p = open - 1;
    while (p > begin && (doc->text[p - 1] == ' ' || doc->text[p - 1] == '\t')) p--;
    size_t name_end = p, name_start = p;
    while (name_start > begin && (isalnum((unsigned char)doc->text[name_start - 1]) || doc->text[name_start - 1] == '_')) name_start--;
    if (name_start == name_end) return NULL;
    char name[128]; size_t nl = name_end - name_start;
    if (nl >= sizeof(name)) return NULL;
    memcpy(name, doc->text + name_start, nl); name[nl] = '\0';
    while (p > begin && (doc->text[p - 1] == ' ' || doc->text[p - 1] == '\t')) p--;
    if (p == begin || doc->text[p - 1] != '.') return NULL;
    size_t re = p - 1, rs = re;
    while (rs > begin && (isalnum((unsigned char)doc->text[rs - 1]) || doc->text[rs - 1] == '_')) rs--;
    char receiver[128]; size_t rl = re - rs;
    if (!rl || rl >= sizeof(receiver)) return NULL;
    memcpy(receiver, doc->text + rs, rl); receiver[rl] = '\0';

    SnDecl *method = find_method(a, receiver, name);
    if (!method) return NULL;
    char label[1024]; size_t used = (size_t)snprintf(label, sizeof(label), "method %s(", name);
    for (size_t i = 0; i < method->params.len && used < sizeof(label); i++) {
        SnParam *param = SN_LIST_AT(method->params, SnParam, i);
        char ty[128]; type_text(param->type, ty, sizeof(ty));
        if (i) used += (size_t)snprintf(label + used, sizeof(label) - used, ", ");
        used += (size_t)snprintf(label + used, sizeof(label) - used, "%s: %s", param->name, ty);
    }
    snprintf(label + used, sizeof(label) - used, "): %s", method->ret ? method->ret->name : "unit");
    JsonBuilder jb; jb_init(&jb); jb_start_obj(&jb);
    jb_key(&jb, "signatures"); jb_start_arr(&jb);
    jb_start_obj(&jb); jb_kv_str(&jb, "label", label); jb_kv_int(&jb, "activeParameter", 0);
    jb_key(&jb, "parameters"); jb_start_arr(&jb);
    for (size_t i = 0; i < method->params.len; i++) {
        SnParam *param = SN_LIST_AT(method->params, SnParam, i);
        char info[160]; char ty[96]; type_text(param->type, ty, sizeof(ty));
        snprintf(info, sizeof(info), "%s: %s", param->name, ty);
        jb_start_obj(&jb); jb_kv_str(&jb, "label", info); jb_end_obj(&jb);
    }
    jb_end_arr(&jb); jb_end_obj(&jb); jb_end_arr(&jb); jb_kv_int(&jb, "activeSignature", 0);
    jb_end_obj(&jb); return jb_take(&jb);
}
