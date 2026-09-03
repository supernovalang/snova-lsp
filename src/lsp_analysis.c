#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
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

static void register_unit_decls_in_resolver(LspDocAnalysis *a) {
    if (!a || !a->has_ast) return;

    const char *pkg_name_str = (a->unit.package && a->unit.package[0]) ? a->unit.package : "main";
    const char *pkg_name = sn_intern_cstr(&a->intern, pkg_name_str);

    SnScope *pkg_scope = sn_resolver_package_scope(&a->resolver, pkg_name);
    if (!pkg_scope) {
        pkg_scope = (SnScope *)sn_arena_alloc(&a->arena, sizeof(SnScope));
        sn_scope_init(pkg_scope, &a->arena, NULL);
        SnPackageScopeEntry *pe = (SnPackageScopeEntry *)sn_arena_alloc(&a->arena, sizeof(SnPackageScopeEntry));
        pe->package_name = pkg_name;
        pe->scope = pkg_scope;
        pe->next = a->resolver.packages;
        a->resolver.packages = pe;
    }

    for (size_t i = 0; i < a->unit.decls.len; i++) {
        SnDecl *d = SN_LIST_AT(a->unit.decls, SnDecl, i);
        if (!d || !d->name) continue;

        const char *name = sn_intern_cstr(&a->intern, d->name);
        SnSymbolKind sk = SN_SYM_LOCAL;
        bool has_members = false;

        if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT ||
            d->kind == SN_DECL_ENUM || d->kind == SN_DECL_INTERFACE) {
            sk = SN_SYM_TYPE;
            has_members = true;
        } else if (d->kind == SN_DECL_FUNC) {
            sk = SN_SYM_FUNC;
        } else if (d->kind == SN_DECL_CONST) {
            sk = SN_SYM_CONST;
        }

        SnSymbol *sym = sn_scope_lookup_local(pkg_scope, name);
        if (!sym) {
            sym = sn_scope_define(pkg_scope, name, sk, d, d->span);
        }

        if (has_members) {
            SnScope *ms = NULL;
            for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
                if (te->type_decl && te->type_decl->name && strcmp(te->type_decl->name, d->name) == 0) {
                    ms = te->member_scope;
                    break;
                }
            }
            if (!ms) {
                ms = (SnScope *)sn_arena_alloc(&a->arena, sizeof(SnScope));
                sn_scope_init(ms, &a->arena, NULL);
                SnTypeScopeEntry *te = (SnTypeScopeEntry *)sn_arena_alloc(&a->arena, sizeof(SnTypeScopeEntry));
                te->type_decl = d;
                te->member_scope = ms;
                te->next = a->resolver.type_scopes;
                a->resolver.type_scopes = te;
            }

            for (size_t j = 0; j < d->members.len; j++) {
                SnDecl *m = SN_LIST_AT(d->members, SnDecl, j);
                if (!m || !m->name) continue;
                const char *mname = sn_intern_cstr(&a->intern, m->name);
                SnSymbolKind msk = (m->kind == SN_DECL_METHOD) ? SN_SYM_METHOD : SN_SYM_FIELD;
                if (!sn_scope_lookup_local(ms, mname)) {
                    sn_scope_define(ms, mname, msk, m, m->span);
                }
            }
        }
    }
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

    // Use a temporary stream so the LSP remains portable to C runtimes that
    // do not provide the GNU-only open_memstream extension.
    char *diag_buf = NULL;
    size_t diag_size = 0;
    FILE *diag_mem = tmpfile();

    sn_diag_init(&a->diag, a->path ? a->path : "", doc->text, doc->text_len);
    a->diag.out = diag_mem;
    a->diag.use_color = 0;

    // 1. Lex
    memset(&a->tokens, 0, sizeof(a->tokens));
    sn_lex(&a->arena, &a->diag, doc->text, doc->text_len, &a->tokens);

    // 2. Parse
    memset(&a->unit, 0, sizeof(a->unit));
    sn_parse(&a->arena, &a->diag, &a->tokens, &a->unit);
    a->has_ast = true;

    // 3. Package Graph & Types & Resolution
    sn_pkggraph_init(&a->graph, &a->arena, &a->intern, &a->diag);
    sn_types_init(&a->types, &a->arena);
    sn_resolver_init(&a->resolver, &a->arena, &a->intern, &a->diag, &a->graph, &a->types);

    // Scan project roots, .snovalang/deps, and builtins
    SnProject proj;
    memset(&proj, 0, sizeof(proj));
    const char *scan_start = (a->path && a->path[0] && strcmp(a->path, "/") != 0) ? a->path : engine->workspace_root;
    if (scan_start && scan_start[0]) {
        project_discover(scan_start, &proj);
        if (proj.has_manifest && proj.source_root[0] && strcmp(proj.source_root, "/") != 0) {
            scan_project_roots(&a->graph, &proj);
        } else if (engine->workspace_root[0]) {
            char ws_deps[SNOVAC_PATH_MAX + 32];
            snprintf(ws_deps, sizeof(ws_deps), "%s/.snovalang/deps", engine->workspace_root);
            if (path_is_dir(ws_deps)) {
                sn_pkggraph_scan_root(&a->graph, ws_deps);
            }
            char ws_src[SNOVAC_PATH_MAX + 16];
            snprintf(ws_src, sizeof(ws_src), "%s/src", engine->workspace_root);
            if (path_is_dir(ws_src)) {
                sn_pkggraph_scan_root(&a->graph, ws_src);
            }
        }
    }

    if (a->path && a->path[0] && path_is_file(a->path)) {
        sn_pkggraph_scan_single_file(&a->graph, a->path);
    }

    const char *source_for_builtin = (proj.has_manifest && proj.source_root[0] && strcmp(proj.source_root, "/") != 0) ? proj.source_root : engine->workspace_root;
    char builtin_find[SNOVAC_PATH_MAX];
    if (engine->builtin_dir[0]) {
        sn_pkggraph_scan_root(&a->graph, engine->builtin_dir);
        sn_pkggraph_load_native_manifest(&a->graph, engine->builtin_dir);
    } else if (source_for_builtin && source_for_builtin[0] && find_builtin_root_for_project(source_for_builtin, builtin_find, sizeof(builtin_find))) {
        sn_pkggraph_scan_root(&a->graph, builtin_find);
        sn_pkggraph_load_native_manifest(&a->graph, builtin_find);
    }

    sn_pkggraph_link(&a->graph);

    // 4. Resolve symbols & prelude
    sn_resolver_collect(&a->resolver);
    register_unit_decls_in_resolver(a);
    sn_resolver_build_prelude(&a->resolver);
    a->has_resolved = true;

    // 5. Typecheck bodies
    sn_checker_init(&a->checker, &a->arena, &a->intern, &a->diag, &a->resolver, &a->types);
    SnBodyCheckScope scope = { .own_prefix = a->path };
    check_all_bodies(&a->checker, &a->resolver, &a->graph, &a->arena, &scope);

    // Close and flush diagnostics
    if (diag_mem) {
        long end = ftell(diag_mem);
        if (end > 0) {
            diag_size = (size_t)end;
            diag_buf = (char *)malloc(diag_size + 1);
            if (diag_buf) {
                rewind(diag_mem);
                diag_size = fread(diag_buf, 1, diag_size, diag_mem);
                diag_buf[diag_size] = '\0';
            } else {
                diag_size = 0;
            }
        }
        fclose(diag_mem);
    }
    a->diag.out = NULL;
    a->diag.quiet = 1;
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
        const SnToken *tok = &a->tokens.data[i];
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

        uint32_t start = d->span.offset;
        uint32_t end = UINT32_MAX;
        if (i + 1 < list->len) {
            const SnDecl *next_d = SN_LIST_AT(*list, const SnDecl, i + 1);
            if (next_d && next_d->span.offset > start) {
                end = next_d->span.offset;
            }
        }

        if (offset >= start && offset < end) {
            if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT ||
                d->kind == SN_DECL_INTERFACE || d->kind == SN_DECL_ENUM) {
                const SnDecl *inner = find_decl_in_list(&d->members, offset);
                if (inner) return inner;
                const SnDecl *var_inner = find_decl_in_list(&d->variants, offset);
                if (var_inner) return var_inner;
            }
            return d;
        }
    }
    return NULL;
}

const SnDecl *lsp_find_decl_at(const LspDocAnalysis *a, uint32_t offset) {
    if (!a || !a->has_ast) return NULL;
    return find_decl_in_list(&a->unit.decls, offset);
}

static const SnDecl *find_enclosing_routine_helper(const SnList *decls, uint32_t offset, const SnDecl **out_type) {
    if (!decls) return NULL;
    for (size_t i = 0; i < decls->len; i++) {
        const SnDecl *d = SN_LIST_AT(*decls, const SnDecl, i);
        if (!d) continue;

        uint32_t start = d->span.offset;
        uint32_t end = UINT32_MAX;
        if (i + 1 < decls->len) {
            const SnDecl *next_d = SN_LIST_AT(*decls, const SnDecl, i + 1);
            if (next_d && next_d->span.offset > start) {
                end = next_d->span.offset;
            }
        }

        if (offset >= start && offset < end) {
            if (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT ||
                d->kind == SN_DECL_INTERFACE || d->kind == SN_DECL_ENUM) {
                const SnDecl *inner = find_enclosing_routine_helper(&d->members, offset, out_type);
                if (inner) {
                    if (out_type) *out_type = d;
                    return inner;
                }
                if (out_type) *out_type = d;
            }
            if (d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) {
                return d;
            }
        }
    }
    return NULL;
}

static void define_type_params_in_scope(SnChecker *c, SnScope *scope, const SnDecl *decl) {
    if (!decl || decl->generics.len == 0) return;
    for (size_t i = 0; i < decl->generics.len; i++) {
        const char *name = SN_LIST_AT(decl->generics, const char, i);
        const char *iname = sn_intern_cstr(c->intern, name);
        SnSymbol *sym = sn_scope_define(scope, iname, SN_SYM_TYPE, NULL, decl->span);
        if (sym) {
            sym->value_type = sn_type_typevar(c->types, sym);
        }
    }
}

static void collect_statements_before_offset(SnChecker *checker, SnScope *scope, const SnStmt *s, uint32_t offset) {
    if (!s) return;
    if (s->span.offset >= offset) return;

    if (s->kind == SN_STMT_LET || s->kind == SN_STMT_VAR) {
        sn_check_stmt(checker, scope, (SnStmt *)s);
    } else if (s->kind == SN_STMT_FOR) {
        if (s->name) {
            SnTypeRep *elem_ty = sn_type_any(checker->types);
            if (s->expr) {
                SnTypeRep *iter_ty = sn_check_expr(checker, scope, s->expr);
                if (iter_ty && iter_ty->tag == SN_T_ARRAY && iter_ty->nargs >= 1 && iter_ty->args && iter_ty->args[0]) {
                    elem_ty = iter_ty->args[0];
                }
            }
            const char *iname = sn_intern_cstr(checker->intern, s->name);
            SnSymbol *sym = sn_scope_define(scope, iname, SN_SYM_LOCAL, NULL, s->span);
            if (sym) {
                sym->value_type = elem_ty;
                sym->is_mutable = 0;
            }
        }
        if (s->then_br) collect_statements_before_offset(checker, scope, s->then_br, offset);
    } else if (s->kind == SN_STMT_BLOCK) {
        for (size_t i = 0; i < s->stmts.len; i++) {
            const SnStmt *child = SN_LIST_AT(s->stmts, const SnStmt, i);
            collect_statements_before_offset(checker, scope, child, offset);
        }
    } else if (s->kind == SN_STMT_IF) {
        if (s->then_br) collect_statements_before_offset(checker, scope, s->then_br, offset);
        if (s->else_br) collect_statements_before_offset(checker, scope, s->else_br, offset);
    } else if (s->kind == SN_STMT_WHILE) {
        if (s->then_br) collect_statements_before_offset(checker, scope, s->then_br, offset);
    } else if (s->kind == SN_STMT_TRY) {
        if (s->then_br) collect_statements_before_offset(checker, scope, s->then_br, offset);
        for (size_t i = 0; i < s->catches.len; i++) {
            const SnStmt *cat = SN_LIST_AT(s->catches, const SnStmt, i);
            if (cat) {
                if (cat->name) {
                    const char *iname = sn_intern_cstr(checker->intern, cat->name);
                    SnSymbol *sym = sn_scope_define(scope, iname, SN_SYM_LOCAL, NULL, cat->span);
                    if (sym) {
                        sym->value_type = sn_type_any(checker->types);
                        sym->is_mutable = 0;
                    }
                }
                collect_statements_before_offset(checker, scope, cat, offset);
            }
        }
        if (s->finally_br) collect_statements_before_offset(checker, scope, s->finally_br, offset);
    }
}

SnScope *lsp_build_scope_at(const LspDocAnalysis *a, SnChecker *checker, uint32_t offset, const SnDecl **out_enclosing_decl, const SnDecl **out_enclosing_type) {
    if (out_enclosing_decl) *out_enclosing_decl = NULL;
    if (out_enclosing_type) *out_enclosing_type = NULL;
    if (!a || !checker) return NULL;

    const SnDecl *enclosing_type = NULL;
    const SnDecl *routine = find_enclosing_routine_helper(&a->unit.decls, offset, &enclosing_type);
    if (out_enclosing_decl) *out_enclosing_decl = routine;
    if (out_enclosing_type) *out_enclosing_type = enclosing_type;

    checker->current_package = sn_intern_cstr(checker->intern, (a->unit.package && a->unit.package[0]) ? a->unit.package : "main");
    checker->current_imports = &a->unit.imports;
    checker->enclosing_type = enclosing_type;

    SnScope *scope = (SnScope *)sn_arena_alloc(checker->arena, sizeof(SnScope));
    sn_scope_init(scope, checker->arena, NULL);

    if (!routine) {
        return scope;
    }

    SnScope *type_params_scope = (SnScope *)sn_arena_alloc(checker->arena, sizeof(SnScope));
    sn_scope_init(type_params_scope, checker->arena, NULL);
    define_type_params_in_scope(checker, type_params_scope, routine);
    if (enclosing_type) {
        define_type_params_in_scope(checker, type_params_scope, enclosing_type);
    }
    checker->type_params = type_params_scope;

    // Define parameters
    for (size_t i = 0; i < routine->params.len; i++) {
        SnParam *p = SN_LIST_AT(routine->params, SnParam, i);
        SnTypeRep *pty = sn_check_resolve_type(checker, p->type);
        SnSymbol *sym = sn_scope_define(scope, sn_intern_cstr(checker->intern, p->name),
                                        SN_SYM_PARAM, NULL, p->span);
        if (sym) {
            sym->value_type = pty;
            sym->is_mutable = 0;
        }
    }

    // Define `this`
    if (enclosing_type) {
        const char *tname = enclosing_type->name ? enclosing_type->name : "";
        const char *iname = sn_intern_cstr(checker->intern, tname);
        SnScope *pkg_scope = sn_resolver_package_scope(checker->resolver, checker->current_package);
        SnSymbol *self_sym = pkg_scope ? sn_scope_lookup(pkg_scope, iname) : NULL;
        if (!self_sym && checker->resolver->prelude_scope) {
            self_sym = sn_scope_lookup(checker->resolver->prelude_scope, iname);
        }
        SnTypeRep *self_ty = self_sym ? sn_type_named(checker->types, self_sym, NULL, 0)
                                      : sn_type_error(checker->types);
        SnSymbol *this_sym = sn_scope_define(scope, sn_intern_cstr(checker->intern, "this"),
                                             SN_SYM_PARAM, NULL, routine->span);
        if (this_sym) {
            this_sym->value_type = self_ty;
            this_sym->is_mutable = 0;
        }
        if (routine->name && strcmp(routine->name, "new") == 0) {
            checker->current_return_type = self_ty;
        } else {
            checker->current_return_type = sn_check_resolve_type(checker, routine->ret);
        }
    } else {
        checker->current_return_type = sn_check_resolve_type(checker, routine->ret);
    }

    // Collect statements prior to cursor offset
    if (routine->body && routine->body->stmts.len > 0) {
        for (size_t i = 0; i < routine->body->stmts.len; i++) {
            const SnStmt *s = SN_LIST_AT(routine->body->stmts, const SnStmt, i);
            collect_statements_before_offset(checker, scope, s, offset);
        }
    }

    return scope;
}

SnTypeRep *lsp_infer_expr_type_at(const LspDocAnalysis *a, SnChecker *checker, SnScope *local, const char *expr_str) {
    if (!a || !checker || !expr_str || !expr_str[0]) return NULL;

    size_t len = strlen(expr_str);
    char *buf = sn_arena_strndup(checker->arena, expr_str, len);

    SnDiagSink sink;
    sn_diag_init(&sink, a->path ? a->path : "", buf, len);
    sink.out = NULL;
    sink.quiet = 1;

    SnTokenVec toks;
    memset(&toks, 0, sizeof(toks));
    if (sn_lex(checker->arena, &sink, buf, len, &toks) != 0) {
        return NULL;
    }

    SnExpr *e = sn_parse_expr_only(checker->arena, &sink, &toks);
    if (!e) return NULL;

    SnTypeRep *ty = sn_check_expr(checker, local, e);
    return ty;
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
    const char *iname = sn_intern_cstr((SnInternTable *)&a->intern, name);

    // 0. Check local scope in enclosing routine
    SnDiagSink null_diag;
    sn_diag_init(&null_diag, a->path ? a->path : "", "", 0);
    null_diag.out = NULL;
    null_diag.quiet = 1;

    SnChecker checker;
    sn_checker_init(&checker, (SnArena *)&a->arena, (SnInternTable *)&a->intern,
                    &null_diag, (SnResolver *)&a->resolver, (SnTypeTable *)&a->types);
    const SnDecl *enclosing_decl = NULL;
    const SnDecl *enclosing_type = NULL;
    SnScope *local_scope = lsp_build_scope_at(a, &checker, offset, &enclosing_decl, &enclosing_type);
    if (local_scope) {
        SnSymbol *s = sn_scope_lookup(local_scope, iname);
        if (s) return s;
    }

    const char *pkg_name = a->unit.package ? a->unit.package : "";

    // 1. Check current package scope
    SnScope *pkg_scope = sn_resolver_package_scope(&a->resolver, pkg_name);
    if (pkg_scope) {
        SnSymbol *s = sn_scope_lookup(pkg_scope, iname);
        if (s) return s;
    }

    // 2. Check prelude scope
    if (a->resolver.prelude_scope) {
        SnSymbol *s = sn_scope_lookup(a->resolver.prelude_scope, iname);
        if (s) return s;
    }

    // 3. Check imports
    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        SnScope *imp_scope = sn_resolver_package_scope(&a->resolver, imp);
        char package_name[SNOVAC_PATH_MAX];
        if (!imp_scope && imp) {
            snprintf(package_name, sizeof(package_name), "%s", imp);
            for (char *dot = strrchr(package_name, '.'); dot && !imp_scope; dot = strrchr(package_name, '.')) {
                *dot = '\0';
                imp_scope = sn_resolver_package_scope(&a->resolver, package_name);
            }
        }
        if (imp_scope) {
            SnSymbol *s = sn_scope_lookup(imp_scope, iname);
            if (s) return s;
        }
    }

    // 4. Check all type member scopes
    for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
        SnSymbol *s = sn_scope_lookup_local(te->member_scope, iname);
        if (s) return s;
    }

    return NULL;
}

/* -----------------------------------------------------------------------
 * Snova Manifest Analysis  (mod.sno / snova.mod)
 * Parses the manifest text and produces LSP diagnostics for:
 *   - missing/malformed "module" declaration
 *   - missing/malformed "snova" version declaration
 *   - malformed "dependencies(...)" block
 * ----------------------------------------------------------------------- */

/* Returns a heap-allocated LspDocAnalysis with diagnostics only.
   The analysis is inserted into the engine cache under the document URI. */
LspDocAnalysis *lsp_engine_analyze_manifest(LspAnalysisEngine *engine, const LspDocument *doc) {
    if (!engine || !doc || !doc->text) return NULL;

    /* Remove any previously cached analysis for this URI */
    lsp_engine_remove_analysis(engine, doc->uri);

    LspDocAnalysis *a = (LspDocAnalysis *)malloc(sizeof(LspDocAnalysis));
    if (!a) return NULL;
    memset(a, 0, sizeof(LspDocAnalysis));
    a->uri     = strdup(doc->uri);
    a->path    = strdup(doc->path ? doc->path : "");
    a->version = doc->version;
    sn_arena_init(&a->arena, 64 * 1024);
    diag_list_init(&a->diags);

    const char *src   = doc->text;
    size_t      src_len = doc->text_len;
    const char *end   = src + src_len;

    /* ── helpers ────────────────────────────────────────────────────── */
#define MANIFEST_ERR(LINE, COL, MSG) \
    do { \
        SnSpan _sp = { .offset = 0, .len = 1, .line = (LINE), .col = (COL) }; \
        diag_list_add(&a->diags, 0, SN_DIAG_ERROR, _sp, (MSG), a->path); \
    } while (0)

#define MANIFEST_WARN(LINE, COL, MSG) \
    do { \
        SnSpan _sp = { .offset = 0, .len = 1, .line = (LINE), .col = (COL) }; \
        diag_list_add(&a->diags, 0, SN_DIAG_WARNING, _sp, (MSG), a->path); \
    } while (0)

    bool found_module      = false;
    bool found_snova_ver   = false;
    bool found_deps        = false;
    bool deps_paren_open   = false;
    bool deps_paren_close  = false;

    /* ── line-by-line scan ──────────────────────────────────────────── */
    const char *p    = src;
    uint32_t    lnum = 1;

    while (p < end) {
        /* skip leading whitespace (but not newlines) */
        const char *line_start = p;
        while (p < end && (*p == ' ' || *p == '\t')) p++;

        /* detect comment: # or // */
        if (p < end && (*p == '#' || (p + 1 < end && p[0] == '/' && p[1] == '/'))) {
            /* skip until end of line */
            while (p < end && *p != '\n') p++;
            if (p < end && *p == '\n') p++;
            lnum++;
            continue;
        }

        /* find end of line */
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;

        size_t line_len = (size_t)(line_end - line_start);
        char line_buf[512];
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';

        /* ── "module <name> [<url>]" ─────────────────────────────── */
        if (strncmp(p, "module", 6) == 0 && (p + 6 >= end || p[6] == ' ' || p[6] == '\t' || p[6] == '\n' || p[6] == '\r')) {
            found_module = true;
            const char *after = p + 6;
            while (after < line_end && (*after == ' ' || *after == '\t')) after++;
            if (after >= line_end || *after == '\r' || *after == '\n') {
                MANIFEST_ERR(lnum, 1, "manifest: 'module' declaration is missing a module path");
            }
        }
        /* ── "snova \"<semver>\"" ─────────────────────────────────── */
        else if (strncmp(p, "snova", 5) == 0 && (p + 5 >= end || p[5] == ' ' || p[5] == '\t' || p[5] == '"')) {
            /* distinguish from "snova-lsp" or other identifiers */
            if (p + 5 < line_end && (p[5] == ' ' || p[5] == '\t' || p[5] == '"')) {
                found_snova_ver = true;
                const char *after = p + 5;
                while (after < line_end && (*after == ' ' || *after == '\t')) after++;
                if (after >= line_end || *after != '"') {
                    MANIFEST_ERR(lnum, (uint32_t)(after - line_start) + 1,
                                 "manifest: 'snova' version must be a quoted semantic version, e.g. snova \"1.0.0\"");
                }
            }
        }
        /* ── "dependencies(...)" block ───────────────────────────── */
        else if (strncmp(p, "dependencies", 12) == 0) {
            found_deps = true;
            /* Scan for opening parenthesis on this or following lines */
            const char *q = p + 12;
            while (q < line_end && (*q == ' ' || *q == '\t')) q++;
            if (q < line_end && *q == '(') {
                deps_paren_open = true;
            } else if (q >= line_end) {
                /* opening paren may be on the next line — tolerate */
                deps_paren_open = true;
            } else {
                MANIFEST_ERR(lnum, (uint32_t)(q - line_start) + 1,
                             "manifest: expected '(' after 'dependencies'");
            }
        }
        /* ── closing ')' for dependencies block ─────────────────── */
        else if (deps_paren_open && !deps_paren_close && p < line_end && *p == ')') {
            deps_paren_close = true;
        }

        /* advance to next line */
        p = line_end;
        if (p < end && *p == '\n') p++;
        lnum++;
    }

    /* ── post-scan validations ───────────────────────────────────── */
    if (!found_module) {
        MANIFEST_ERR(1, 1, "manifest: missing 'module' declaration (e.g. module github.com/org/repo)");
    }
    if (!found_snova_ver) {
        MANIFEST_WARN(1, 1, "manifest: missing 'snova' version declaration (e.g. snova \"1.0.0\")");
    }
    if (found_deps && deps_paren_open && !deps_paren_close) {
        MANIFEST_ERR(lnum > 1 ? lnum - 1 : 1, 1,
                     "manifest: 'dependencies(...)' block is missing closing ')'");
    }

#undef MANIFEST_ERR
#undef MANIFEST_WARN

    /* mark as manifest — no AST available */
    a->has_ast      = false;
    a->has_resolved = false;

    /* insert into engine cache */
    a->next          = engine->analyses;
    engine->analyses = a;
    return a;
}
