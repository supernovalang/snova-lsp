#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_code_action.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

typedef struct {
    char *title;
    char *kind; /* "quickfix", "source.organizeImports", "refactor" */
    bool is_preferred;
    char *doc_uri;
    LspRange edit_range;
    char *new_text;
} ActionItem;

typedef struct {
    ActionItem *items;
    size_t len;
    size_t cap;
} ActionList;

static void actionlist_init(ActionList *list) {
    list->len = 0;
    list->cap = 16;
    list->items = (ActionItem *)malloc(sizeof(ActionItem) * list->cap);
}

static void actionlist_free(ActionList *list) {
    if (!list || !list->items) return;
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].title) free(list->items[i].title);
        if (list->items[i].kind) free(list->items[i].kind);
        if (list->items[i].doc_uri) free(list->items[i].doc_uri);
        if (list->items[i].new_text) free(list->items[i].new_text);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void actionlist_add(ActionList *list, const char *title, const char *kind,
                           bool is_preferred, const char *uri, LspRange range, const char *new_text) {
    if (!list || !title || !new_text) return;

    if (list->len >= list->cap) {
        list->cap *= 2;
        list->items = (ActionItem *)realloc(list->items, sizeof(ActionItem) * list->cap);
    }

    ActionItem *ai = &list->items[list->len++];
    ai->title = strdup(title);
    ai->kind = kind ? strdup(kind) : strdup("quickfix");
    ai->is_preferred = is_preferred;
    ai->doc_uri = uri ? strdup(uri) : NULL;
    ai->edit_range = range;
    ai->new_text = strdup(new_text);
}

static uint32_t find_import_insertion_line(const LspDocument *doc) {
    if (!doc || !doc->text) return 0;
    const char *p = doc->text;
    uint32_t cur_line = 0;
    uint32_t last_import_line = 0;
    bool found_import = false;
    uint32_t package_line = 0;
    bool found_package = false;

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t len = (size_t)(p - line_start);
        if (*p == '\n') p++;

        char line_buf[512];
        if (len >= sizeof(line_buf)) len = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, len);
        line_buf[len] = '\0';

        char *trimmed = line_buf;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "package ", 8) == 0) {
            package_line = cur_line;
            found_package = true;
        } else if (strncmp(trimmed, "import ", 7) == 0) {
            last_import_line = cur_line;
            found_import = true;
        }

        cur_line++;
    }

    if (found_import) {
        return last_import_line + 1;
    }
    if (found_package) {
        return package_line + 1;
    }
    return 0;
}

static void extract_token_text_at_range(const LspDocument *doc, LspRange range, char *out_buf, size_t out_sz) {
    out_buf[0] = '\0';
    if (!doc || !doc->text) return;

    uint32_t start_off = lsp_pos_to_offset(doc, range.start);
    uint32_t end_off = lsp_pos_to_offset(doc, range.end);
    if (end_off <= start_off || end_off > doc->text_len) return;

    size_t len = end_off - start_off;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out_buf, doc->text + start_off, len);
    out_buf[len] = '\0';
}

static void add_auto_import_actions(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc, const char *sym_name) {
    if (!a || !doc || !sym_name || !sym_name[0]) return;

    uint32_t ins_line = find_import_insertion_line(doc);
    LspRange ins_range = {
        .start = { .line = ins_line, .character = 0 },
        .end = { .line = ins_line, .character = 0 }
    };

    // Search across package scopes in the graph
    for (SnPackageScopeEntry *pe = a->resolver.packages; pe; pe = pe->next) {
        if (!pe->package_name || !pe->scope) continue;

        // Skip current package and already imported
        if (a->unit.package && strcmp(pe->package_name, a->unit.package) == 0) continue;
        bool already_imported = false;
        for (size_t i = 0; i < a->unit.imports.len; i++) {
            const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
            if (imp && strcmp(imp, pe->package_name) == 0) {
                already_imported = true;
                break;
            }
        }
        if (already_imported) continue;

        const char *iname = sn_intern_cstr((SnInternTable *)&a->intern, sym_name);
        SnSymbol *sym = sn_scope_lookup_local(pe->scope, iname);
        if (sym && (sym->kind == SN_SYM_TYPE || sym->kind == SN_SYM_FUNC || sym->kind == SN_SYM_CONST || sym->kind == SN_SYM_VARIANT)) {
            char title[256];
            snprintf(title, sizeof(title), "Import '%s' for '%s'", pe->package_name, sym_name);
            char new_text[256];
            snprintf(new_text, sizeof(new_text), "import %s;\n", pe->package_name);
            actionlist_add(list, title, "quickfix", true, doc->uri, ins_range, new_text);
        }
    }
}

static void add_organize_imports_action(ActionList *list, const LspDocument *doc) {
    if (!doc || !doc->text) return;

    // Collect all import lines and their spans
    uint32_t first_import_line = 0;
    uint32_t last_import_line = 0;
    bool found_any = false;

    char **imports = NULL;
    size_t imp_count = 0;
    size_t imp_cap = 8;
    imports = (char **)malloc(sizeof(char *) * imp_cap);

    const char *p = doc->text;
    uint32_t cur_line = 0;

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t len = (size_t)(p - line_start);
        if (*p == '\n') p++;

        char line_buf[512];
        if (len >= sizeof(line_buf)) len = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, len);
        line_buf[len] = '\0';

        char *trimmed = line_buf;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "import ", 7) == 0) {
            if (!found_any) {
                first_import_line = cur_line;
                found_any = true;
            }
            last_import_line = cur_line;

            char *pkg = trimmed + 7;
            while (*pkg == ' ') pkg++;
            char *semi = strchr(pkg, ';');
            if (semi) *semi = '\0';
            size_t plen = strlen(pkg);
            while (plen > 0 && (pkg[plen - 1] == ' ' || pkg[plen - 1] == '\r')) pkg[--plen] = '\0';

            // Add if not duplicate
            bool dup = false;
            for (size_t i = 0; i < imp_count; i++) {
                if (strcmp(imports[i], pkg) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup && pkg[0]) {
                if (imp_count >= imp_cap) {
                    imp_cap *= 2;
                    imports = (char **)realloc(imports, sizeof(char *) * imp_cap);
                }
                imports[imp_count++] = strdup(pkg);
            }
        }
        cur_line++;
    }

    if (found_any && imp_count > 0) {
        // Sort imports alphabetically
        for (size_t i = 0; i < imp_count; i++) {
            for (size_t j = i + 1; j < imp_count; j++) {
                if (strcmp(imports[i], imports[j]) > 0) {
                    char *tmp = imports[i];
                    imports[i] = imports[j];
                    imports[j] = tmp;
                }
            }
        }

        // Build replacement text
        size_t total_sz = 1;
        for (size_t i = 0; i < imp_count; i++) {
            total_sz += strlen(imports[i]) + 16;
        }
        char *cleaned = (char *)malloc(total_sz);
        cleaned[0] = '\0';
        for (size_t i = 0; i < imp_count; i++) {
            strcat(cleaned, "import ");
            strcat(cleaned, imports[i]);
            strcat(cleaned, ";\n");
        }

        LspRange replace_range = {
            .start = { .line = first_import_line, .character = 0 },
            .end = { .line = last_import_line + 1, .character = 0 }
        };

        actionlist_add(list, "Organize Imports", "source.organizeImports", true, doc->uri, replace_range, cleaned);
        free(cleaned);
    }

    for (size_t i = 0; i < imp_count; i++) {
        free(imports[i]);
    }
    free(imports);
}

static void add_generate_constructor_action(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc, LspPosition pos) {
    if (!a || !doc || !a->has_ast) return;
    uint32_t offset = lsp_pos_to_offset(doc, pos);
    const SnDecl *decl = lsp_find_decl_at(a, offset);
    if (decl && decl->kind == SN_DECL_FIELD) {
        for (size_t i = 0; i < a->unit.decls.len; i++) {
            const SnDecl *top = SN_LIST_AT(a->unit.decls, const SnDecl, i);
            if (top && (top->kind == SN_DECL_CLASS || top->kind == SN_DECL_STRUCT)) {
                for (size_t j = 0; j < top->members.len; j++) {
                    if (SN_LIST_AT(top->members, const SnDecl, j) == decl) {
                        decl = top;
                        break;
                    }
                }
            }
        }
    }
    if (!decl || (decl->kind != SN_DECL_CLASS && decl->kind != SN_DECL_STRUCT)) return;

    // Check if class already has a constructor
    bool has_ctor = false;
    for (size_t i = 0; i < decl->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(decl->members, const SnDecl, i);
        if (m && m->name && (strcmp(m->name, "constructor") == 0 || strcmp(m->name, "new") == 0)) {
            has_ctor = true;
            break;
        }
    }
    if (has_ctor) return;

    // Collect fields
    char params_buf[512] = "";
    char body_buf[1024] = "";
    bool first = true;

    for (size_t i = 0; i < decl->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(decl->members, const SnDecl, i);
        if (m && m->kind == SN_DECL_FIELD && m->name) {
            char ftype[128] = "any";
            if (m->type && m->type->kind == SN_TYPE_NAME && m->type->name) {
                snprintf(ftype, sizeof(ftype), "%s", m->type->name);
            }
            if (!first) {
                strcat(params_buf, ", ");
            }
            char param_part[128];
            snprintf(param_part, sizeof(param_part), "%s: %s", m->name, ftype);
            strcat(params_buf, param_part);

            char assign_part[256];
            snprintf(assign_part, sizeof(assign_part), "        this.%s = %s;\n", m->name, m->name);
            strcat(body_buf, assign_part);
            first = false;
        }
    }

    if (!first) {
        char ctor_code[2048];
        snprintf(ctor_code, sizeof(ctor_code),
                 "\n    constructor(%s) {\n%s    }\n",
                 params_buf, body_buf);

        // Insertion position: after the class declaration header
        uint32_t ins_line = (decl->span.line > 0) ? decl->span.line : pos.line + 1;
        LspRange ins_range = {
            .start = { .line = ins_line, .character = 0 },
            .end = { .line = ins_line, .character = 0 }
        };

        char title[128];
        snprintf(title, sizeof(title), "Generate constructor for '%s'", decl->name ? decl->name : "Type");
        actionlist_add(list, title, "refactor.generate.constructor", false, doc->uri, ins_range, ctor_code);
    }
}

char *lsp_code_action_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspRange range, const JsonVal *context_diags) {
    if (!doc) return NULL;

    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, store, doc);
    }

    ActionList list;
    actionlist_init(&list);

    // 1. Process diagnostics in the request context or from active analysis
    if (context_diags && context_diags->kind == JSON_ARRAY) {
        size_t diag_cnt = json_arr_len(context_diags);
        for (size_t i = 0; i < diag_cnt; i++) {
            const JsonVal *d = json_arr_at(context_diags, i);
            const char *msg = json_get_str(d, "message", "");
            const char *code_str = json_get_str(d, "code", "");
            const JsonVal *dr_obj = json_get_obj(d, "range");
            LspRange diag_range = range;
            if (dr_obj) {
                const JsonVal *s = json_get_obj(dr_obj, "start");
                const JsonVal *e = json_get_obj(dr_obj, "end");
                if (s && e) {
                    diag_range.start.line = (uint32_t)json_get_int(s, "line", 0);
                    diag_range.start.character = (uint32_t)json_get_int(s, "character", 0);
                    diag_range.end.line = (uint32_t)json_get_int(e, "line", 0);
                    diag_range.end.character = (uint32_t)json_get_int(e, "character", 0);
                }
            }

            char tok_text[128] = {0};
            extract_token_text_at_range(doc, diag_range, tok_text, sizeof(tok_text));

            // A. Type not imported / Undeclared name (SNOVA0121 / SNOVA0023)
            if (strstr(code_str, "0121") || strstr(code_str, "0023") || strstr(msg, "not imported") || strstr(msg, "undeclared")) {
                if (tok_text[0]) {
                    add_auto_import_actions(&list, a, doc, tok_text);
                }
            }

            // B. Legacy type spelling (SNOVA0011 / SNOVA0027 with capitalized types)
            if (strstr(code_str, "0011") || strstr(code_str, "0027") || strstr(msg, "legacy") || strstr(msg, "spelling")) {
                if (strcasecmp(tok_text, "Int") == 0) {
                    actionlist_add(&list, "Change to 'int'", "quickfix", true, doc->uri, diag_range, "int");
                } else if (strcasecmp(tok_text, "String") == 0) {
                    actionlist_add(&list, "Change to 'string'", "quickfix", true, doc->uri, diag_range, "string");
                } else if (strcasecmp(tok_text, "Bool") == 0) {
                    actionlist_add(&list, "Change to 'bool'", "quickfix", true, doc->uri, diag_range, "bool");
                } else if (strcasecmp(tok_text, "Double") == 0) {
                    actionlist_add(&list, "Change to 'double'", "quickfix", true, doc->uri, diag_range, "double");
                } else if (strcasecmp(tok_text, "Long") == 0) {
                    actionlist_add(&list, "Change to 'long'", "quickfix", true, doc->uri, diag_range, "long");
                } else if (strcasecmp(tok_text, "Decimal") == 0) {
                    actionlist_add(&list, "Change to 'decimal'", "quickfix", true, doc->uri, diag_range, "decimal");
                }
            }

            // C. Immutable reassign (SNOVA0047)
            if (strstr(code_str, "0047") || strstr(msg, "immutable") || strstr(msg, "reassign")) {
                if (tok_text[0]) {
                    // Search definition of variable to replace 'let' with 'var'
                    uint32_t off = lsp_pos_to_offset(doc, diag_range.start);
                    const SnToken *tok = lsp_find_token_at(a, off);
                    if (tok) {
                        const char *name = tok->text;
                        const SnSymbol *sym = lsp_find_symbol_at(a, doc, off, NULL);
                        if (sym && sym->decl && sym->decl->span.offset > 0) {
                            LspRange r = lsp_span_to_range(doc, sym->decl->span.offset, 3, sym->decl->span.line, sym->decl->span.col);
                            char title[128];
                            snprintf(title, sizeof(title), "Change 'let %s' to 'var %s'", name, name);
                            actionlist_add(&list, title, "quickfix", true, doc->uri, r, "var");
                        }
                    }
                }
            }

            // D. Missing return (SNOVA0142)
            if (strstr(code_str, "0142") || strstr(msg, "return")) {
                uint32_t off = lsp_pos_to_offset(doc, diag_range.start);
                const SnDecl *decl = lsp_find_decl_at(a, off);
                if (decl && decl->body && decl->body->span.offset > 0) {
                    uint32_t body_end = decl->body->span.offset + decl->body->span.len;
                    if (body_end > 1) body_end--; // Before closing brace
                    LspPosition p = lsp_offset_to_pos(doc, body_end);
                    LspRange r = { .start = p, .end = p };
                    actionlist_add(&list, "Add 'return null;' statement", "quickfix", false, doc->uri, r, "        return null;\n");
                }
            }

            // E. Optional chaining non-optional (SNOVA0203)
            if (strstr(code_str, "0203") || strstr(msg, "redundant optional chaining")) {
                actionlist_add(&list, "Replace '?.' with '.'", "quickfix", true, doc->uri, diag_range, ".");
            }

            // F. Func in class (SNOVA0030) or Method at top level (SNOVA0031)
            if (strstr(code_str, "0030") || strstr(msg, "cannot declare `func` inside")) {
                actionlist_add(&list, "Change 'func' to 'method'", "quickfix", true, doc->uri, diag_range, "method");
            }
            if (strstr(code_str, "0031") || strstr(msg, "cannot declare `method` at top level")) {
                actionlist_add(&list, "Change 'method' to 'func'", "quickfix", true, doc->uri, diag_range, "func");
            }
        }
    }

    // Direct range & token inspection (quickfixes available at cursor even without explicit diagnostic context)
    char range_tok[128] = {0};
    extract_token_text_at_range(doc, range, range_tok, sizeof(range_tok));
    if (range_tok[0]) {
        // Casing fix
        if (strcmp(range_tok, "Int") == 0) {
            actionlist_add(&list, "Use primitive 'int' (preferred spelling)", "quickfix", true, doc->uri, range, "int");
        } else if (strcmp(range_tok, "String") == 0) {
            actionlist_add(&list, "Use primitive 'string' (preferred spelling)", "quickfix", true, doc->uri, range, "string");
        } else if (strcmp(range_tok, "Bool") == 0) {
            actionlist_add(&list, "Use primitive 'bool' (preferred spelling)", "quickfix", true, doc->uri, range, "bool");
        } else if (strcmp(range_tok, "Double") == 0) {
            actionlist_add(&list, "Use primitive 'double' (preferred spelling)", "quickfix", true, doc->uri, range, "double");
        } else if (strcmp(range_tok, "Long") == 0) {
            actionlist_add(&list, "Use primitive 'long' (preferred spelling)", "quickfix", true, doc->uri, range, "long");
        } else if (strcmp(range_tok, "Decimal") == 0) {
            actionlist_add(&list, "Use primitive 'decimal' (preferred spelling)", "quickfix", true, doc->uri, range, "decimal");
        }

        // Mutability fix on immutable variable reference / assignment
        uint32_t off = lsp_pos_to_offset(doc, range.start);
        const SnSymbol *sym = lsp_find_symbol_at(a, doc, off, NULL);
        if (sym && sym->kind == SN_SYM_LOCAL && !sym->is_mutable) {
            uint32_t decl_off = (sym->decl && sym->decl->span.offset > 0) ? sym->decl->span.offset : sym->span.offset;
            uint32_t decl_line = (sym->decl && sym->decl->span.line > 0) ? sym->decl->span.line : sym->span.line;
            uint32_t decl_col = (sym->decl && sym->decl->span.col > 0) ? sym->decl->span.col : sym->span.col;
            if (decl_off > 0 || decl_line > 0) {
                LspRange r = lsp_span_to_range(doc, decl_off, 3, decl_line, decl_col);
                char title[128];
                snprintf(title, sizeof(title), "Change 'let %s' to 'var %s'", sym->name ? sym->name : range_tok, sym->name ? sym->name : range_tok);
                actionlist_add(&list, title, "quickfix", true, doc->uri, r, "var");
            }
        }
    }

    // 2. Source Actions (Organize Imports)
    add_organize_imports_action(&list, doc);

    // 3. Refactorings (Generate Constructor)
    add_generate_constructor_action(&list, a, doc, range.start);

    // 4. Build JSON Array Response
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);

    for (size_t i = 0; i < list.len; i++) {
        const ActionItem *ai = &list.items[i];
        jb_start_obj(&jb);
        jb_kv_str(&jb, "title", ai->title);
        jb_kv_str(&jb, "kind", ai->kind);
        if (ai->is_preferred) {
            jb_kv_bool(&jb, "isPreferred", true);
        }

        // WorkspaceEdit
        jb_key(&jb, "edit");
        jb_start_obj(&jb);
        jb_key(&jb, "changes");
        jb_start_obj(&jb);

        const char *target_uri = ai->doc_uri ? ai->doc_uri : doc->uri;
        jb_key(&jb, target_uri);
        jb_start_arr(&jb);

        jb_start_obj(&jb);
        jb_key(&jb, "range");
        jb_start_obj(&jb);
        jb_key(&jb, "start");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", ai->edit_range.start.line);
        jb_kv_int(&jb, "character", ai->edit_range.start.character);
        jb_end_obj(&jb);
        jb_key(&jb, "end");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", ai->edit_range.end.line);
        jb_kv_int(&jb, "character", ai->edit_range.end.character);
        jb_end_obj(&jb);
        jb_end_obj(&jb);

        jb_kv_str(&jb, "newText", ai->new_text);
        jb_end_obj(&jb);

        jb_end_arr(&jb);
        jb_end_obj(&jb);
        jb_end_obj(&jb);

        jb_end_obj(&jb);
    }

    jb_end_arr(&jb);

    actionlist_free(&list);
    return jb_take(&jb);
}
