#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_analysis.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>

void lsp_engine_init(LspAnalysisEngine *engine, const char *workspace_root) {
    engine->analyses = NULL;
    engine->workspace_root[0] = '\0';
    engine->builtin_dir[0] = '\0';
    if (workspace_root && workspace_root[0]) {
        lsp_engine_set_workspace_root(engine, workspace_root);
    }
}

void lsp_engine_set_workspace_root(LspAnalysisEngine *engine, const char *workspace_root) {
    if (!workspace_root) return;
    normalize_path_into(workspace_root, engine->workspace_root, sizeof(engine->workspace_root));
    find_builtin_root_for_project(engine->workspace_root, engine->builtin_dir, sizeof(engine->builtin_dir));
}

static void diag_list_init(CapturedDiagList *dl) {
    dl->len = 0;
    dl->cap = 8;
    dl->items = (CapturedDiag *)malloc(sizeof(CapturedDiag) * dl->cap);
}

static void diag_list_free(CapturedDiagList *dl) {
    if (!dl || !dl->items) return;
    for (size_t i = 0; i < dl->len; i++) {
        if (dl->items[i].message) free(dl->items[i].message);
        if (dl->items[i].file_path) free(dl->items[i].file_path);
    }
    free(dl->items);
    dl->items = NULL;
    dl->len = 0;
    dl->cap = 0;
}

static void diag_list_add(CapturedDiagList *dl, int code, SnDiagLevel level, SnSpan span, const char *msg, const char *file_path) {
    if (dl->len >= dl->cap) {
        dl->cap *= 2;
        dl->items = (CapturedDiag *)realloc(dl->items, sizeof(CapturedDiag) * dl->cap);
    }
    CapturedDiag *cd = &dl->items[dl->len++];
    cd->code = code;
    cd->level = level;
    cd->span = span;
    cd->message = strdup(msg ? msg : "");
    cd->file_path = strdup(file_path ? file_path : "");
}

static void parse_diagnostics_from_buffer(CapturedDiagList *dl, const char *buf, size_t len) {
    if (!buf || len == 0) return;
    const char *p = buf;
    const char *end = buf + len;

    while (p < end) {
        // Find line start
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (p >= end) break;

        // Check for "error[SNOVA" or "warning[SNOVA"
        SnDiagLevel level = SN_DIAG_ERROR;
        int code = 0;
        if (strncmp(p, "error[SNOVA", 11) == 0) {
            level = SN_DIAG_ERROR;
            p += 11;
            code = (int)strtoul(p, (char **)&p, 10);
        } else if (strncmp(p, "warning[SNOVA", 13) == 0) {
            level = SN_DIAG_WARNING;
            p += 13;
            code = (int)strtoul(p, (char **)&p, 10);
        } else {
            // Advance to next line
            while (p < end && *p != '\n') p++;
            continue;
        }

        // Skip to "]: "
        while (p < end && *p != ']' && *p != '\n') p++;
        if (p < end && *p == ']') p++;
        if (p < end && *p == ':') p++;
        while (p < end && *p == ' ') p++;

        // Read message up to newline
        const char *msg_start = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        size_t msg_len = (size_t)(p - msg_start);
        char msg_buf[1024];
        if (msg_len >= sizeof(msg_buf)) msg_len = sizeof(msg_buf) - 1;
        memcpy(msg_buf, msg_start, msg_len);
        msg_buf[msg_len] = '\0';

        // Read location line: " --> path:line:col"
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;

        char path_buf[1024] = {0};
        uint32_t line = 1, col = 1;
        if (p + 3 <= end && strncmp(p, "-->", 3) == 0) {
            p += 3;
            while (p < end && *p == ' ') p++;
            const char *loc_start = p;
            while (p < end && *p != '\r' && *p != '\n') p++;
            const char *loc_end = p;

            // Parse path:line:col from back
            const char *c2 = loc_end - 1;
            while (c2 > loc_start && isdigit((unsigned char)*c2)) c2--;
            if (c2 > loc_start && *c2 == ':') {
                col = (uint32_t)strtoul(c2 + 1, NULL, 10);
                const char *c1 = c2 - 1;
                while (c1 > loc_start && isdigit((unsigned char)*c1)) c1--;
                if (c1 > loc_start && *c1 == ':') {
                    line = (uint32_t)strtoul(c1 + 1, NULL, 10);
                    size_t plen = (size_t)(c1 - loc_start);
                    if (plen >= sizeof(path_buf)) plen = sizeof(path_buf) - 1;
                    memcpy(path_buf, loc_start, plen);
                    path_buf[plen] = '\0';
                }
            }
        }

        SnSpan span = {
            .offset = 0,
            .len = 1,
            .line = line,
            .col = col
        };

        diag_list_add(dl, code, level, span, msg_buf, path_buf);

        // Advance past rest of diagnostic snippet
        while (p < end && !(p[0] == '\n' && (strncmp(p + 1, "error[SNOVA", 11) == 0 || strncmp(p + 1, "warning[SNOVA", 13) == 0))) {
            p++;
        }
    }
}

static void free_analysis(LspDocAnalysis *a) {
    if (!a) return;
    if (a->uri) free(a->uri);
    if (a->path) free(a->path);
    diag_list_free(&a->diags);
    sn_arena_free(&a->arena);
    free(a);
}

void lsp_engine_destroy(LspAnalysisEngine *engine) {
    if (!engine) return;
    LspDocAnalysis *cur = engine->analyses;
    while (cur) {
        LspDocAnalysis *next = cur->next;
        free_analysis(cur);
        cur = next;
    }
    engine->analyses = NULL;
}

void lsp_engine_remove_analysis(LspAnalysisEngine *engine, const char *uri) {
    if (!engine || !uri) return;
    LspDocAnalysis **pp = &engine->analyses;
    while (*pp) {
        if (strcmp((*pp)->uri, uri) == 0) {
            LspDocAnalysis *del = *pp;
            *pp = del->next;
            free_analysis(del);
            return;
        }
        pp = &(*pp)->next;
    }
}

LspDocAnalysis *lsp_engine_get_analysis(LspAnalysisEngine *engine, const char *uri) {
    if (!engine || !uri) return NULL;
    for (LspDocAnalysis *a = engine->analyses; a; a = a->next) {
        if (strcmp(a->uri, uri) == 0) return a;
    }
    return NULL;
}

LspDocAnalysis *lsp_engine_analyze_document(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc) {
    (void)store;
    if (!doc) return NULL;

    // Remove existing cached analysis
    lsp_engine_remove_analysis(engine, doc->uri);

    LspDocAnalysis *a = (LspDocAnalysis *)malloc(sizeof(LspDocAnalysis));
    if (!a) return NULL;
    memset(a, 0, sizeof(LspDocAnalysis));
    a->uri = strdup(doc->uri);
    a->path = strdup(doc->path ? doc->path : "");
    a->version = doc->version;

    sn_arena_init(&a->arena, 1024 * 1024);
    sn_intern_init(&a->intern, &a->arena);
    diag_list_init(&a->diags);

    // Setup in-memory stream for diagnostics
    char *diag_buf = NULL;
    size_t diag_size = 0;
    FILE *diag_mem = open_memstream(&diag_buf, &diag_size);

    SnDiagSink diag;
    sn_diag_init(&diag, a->path, doc->text, doc->text_len);
    diag.out = diag_mem;
    diag.use_color = 0;

    // 1. Lex
    memset(&a->tokens, 0, sizeof(a->tokens));
    sn_lex(&a->arena, &diag, doc->text, doc->text_len, &a->tokens);

    // 2. Parse
    memset(&a->unit, 0, sizeof(a->unit));
    sn_parse(&a->arena, &diag, &a->tokens, &a->unit);
    a->has_ast = true;

    // 3. Package Graph & Types & Resolution
    sn_pkggraph_init(&a->graph, &a->arena, &a->intern, &diag);
    sn_types_init(&a->types, &a->arena);
    sn_resolver_init(&a->resolver, &a->arena, &a->intern, &diag, &a->graph, &a->types);

    // Scan project roots / builtins
    if (engine->builtin_dir[0]) {
        sn_pkggraph_scan_root(&a->graph, engine->builtin_dir);
        sn_pkggraph_load_native_manifest(&a->graph, engine->builtin_dir);
    }

    if (a->path[0]) {
        char dir[SNOVAC_PATH_MAX];
        dirname_into(a->path, dir, sizeof(dir));
        sn_pkggraph_scan_single_file(&a->graph, a->path);
        
        char builtin_find[SNOVAC_PATH_MAX];
        if (find_builtin_root_for_project(dir, builtin_find, sizeof(builtin_find))) {
            sn_pkggraph_scan_root(&a->graph, builtin_find);
            sn_pkggraph_load_native_manifest(&a->graph, builtin_find);
        }
    }

    sn_pkggraph_link(&a->graph);

    // 4. Resolve symbols & prelude
    sn_resolver_collect(&a->resolver);
    sn_resolver_build_prelude(&a->resolver);
    a->has_resolved = true;

    // 5. Typecheck bodies
    sn_checker_init(&a->checker, &a->arena, &a->intern, &diag, &a->resolver, &a->types);
    SnBodyCheckScope scope = { .own_prefix = a->path };
    check_all_bodies(&a->checker, &a->resolver, &a->graph, &a->arena, &scope);

    // Close and flush diagnostics
    fclose(diag_mem);
    if (diag_buf && diag_size > 0) {
        parse_diagnostics_from_buffer(&a->diags, diag_buf, diag_size);
        free(diag_buf);
    }

    // Insert into analyses chain
    a->next = engine->analyses;
    engine->analyses = a;
    return a;
}

const SnToken *lsp_find_token_at(const LspDocAnalysis *a, uint32_t offset) {
    if (!a || a->tokens.len == 0) return NULL;
    for (size_t i = 0; i < a->tokens.len; i++) {
        const SnToken *tok = &a->tokens.items[i];
        if (offset >= tok->span.offset && offset < tok->span.offset + tok->span.len) {
            return tok;
        }
    }
    return NULL;
}

static const SnDecl *find_decl_in_list(const SnList *list, uint32_t offset) {
    if (!list) return NULL;
    for (size_t i = 0; i < list->len; i++) {
        const SnDecl *d = SN_LIST_AT(*list, const SnDecl, i);
        if (!d) continue;
        if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT ||
            d->kind == SN_DECL_INTERFACE || d->kind == SN_DECL_ENUM) {
            const SnDecl *inner = find_decl_in_list(&d->members, offset);
            if (inner) return inner;
            const SnDecl *var_inner = find_decl_in_list(&d->variants, offset);
            if (var_inner) return var_inner;
        }
        if (offset >= d->span.offset && offset <= d->span.offset + (d->span.len ? d->span.len : 1000000)) {
            return d;
        }
    }
    return NULL;
}

const SnDecl *lsp_find_decl_at(const LspDocAnalysis *a, uint32_t offset) {
    if (!a || !a->has_ast) return NULL;
    return find_decl_in_list(&a->unit.decls, offset);
}

const SnSymbol *lsp_find_symbol_at(const LspDocAnalysis *a, const LspDocument *doc, uint32_t offset, const char **out_name) {
    (void)doc;
    if (!a || !a->has_resolved) return NULL;
    const SnToken *tok = lsp_find_token_at(a, offset);
    if (!tok || (tok->kind != SN_TOK_IDENT && !sn_tok_is_keyword(tok->kind))) {
        return NULL;
    }
    if (out_name) *out_name = tok->text;

    const char *name = tok->text;
    const char *pkg_name = a->unit.package ? a->unit.package : "";

    // 1. Check current package scope
    SnScope *pkg_scope = sn_resolver_package_scope(&a->resolver, pkg_name);
    if (pkg_scope) {
        SnSymbol *s = sn_scope_lookup(pkg_scope, name);
        if (s) return s;
    }

    // 2. Check prelude scope
    if (a->resolver.prelude_scope) {
        SnSymbol *s = sn_scope_lookup(a->resolver.prelude_scope, name);
        if (s) return s;
    }

    // 3. Check imports
    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        SnScope *imp_scope = sn_resolver_package_scope(&a->resolver, imp);
        if (imp_scope) {
            SnSymbol *s = sn_scope_lookup(imp_scope, name);
            if (s) return s;
        }
    }

    // 4. Check all type member scopes
    for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
        SnSymbol *s = sn_scope_lookup_local(te->member_scope, name);
        if (s) return s;
    }

    return NULL;
}
