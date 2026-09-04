#include "lsp_hover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *get_keyword_doc(SnTokKind kind) {
    switch (kind) {
        case SN_TOK_PACKAGE: return "```snova\npackage <name>\n```\nDeclares the package namespace for this source file.";
        case SN_TOK_IMPORT: return "```snova\nimport <package>\n```\nImports symbols and types from another package.";
        case SN_TOK_CLASS: return "```snova\nclass <Name> { ... }\n```\nDeclares a reference type with fields, methods, and inheritance.";
        case SN_TOK_STRUCT: return "```snova\nstruct <Name> { ... }\n```\nDeclares a value type with fields and methods.";
        case SN_TOK_ENUM: return "```snova\nenum <Name> { Variant1, Variant2(T) }\n```\nDeclares an algebraic data type / tagged union.";
        case SN_TOK_INTERFACE: return "```snova\ninterface <Name> { ... }\n```\nDefines a contract of method signatures.";
        case SN_TOK_FUNC: return "```snova\nfunc <name>(parameters): ReturnType { body }\n```\nDeclares a top-level function.";
        case SN_TOK_METHOD: return "```snova\nmethod <name>(parameters): ReturnType { body }\n```\nDeclares a member method within a class, struct, or interface.";
        case SN_TOK_LET: return "```snova\nlet <name>[: Type] = <expr>\n```\nDeclares an immutable local binding or field.";
        case SN_TOK_VAR: return "```snova\nvar <name>[: Type] = <expr>\n```\nDeclares a mutable variable or field.";
        case SN_TOK_CONST: return "```snova\nconst <name>[: Type] = <expr>\n```\nDeclares a compile-time constant.";
        case SN_TOK_IF: return "```snova\nif <cond> { ... } else { ... }\n```\nConditional execution or ternary expression.";
        case SN_TOK_WHILE: return "```snova\nwhile <cond> { ... }\n```\nRepeats a block of statements while the condition is true.";
        case SN_TOK_FOR: return "```snova\nfor (let item in collection) { ... }\n```\nIterates over elements in an iterable or stream.";
        case SN_TOK_MATCH: return "```snova\nmatch <expr> { pattern -> ... }\n```\nPattern matching on values, enums, and structures.";
        case SN_TOK_RETURN: return "```snova\nreturn [<expr>]\n```\nReturns a value from a function or method.";
        case SN_TOK_ASYNC: return "```snova\nasync func / async method\n```\nDeclares an asynchronous function returning a Task.";
        case SN_TOK_AWAIT: return "```snova\nawait <future-expr>\n```\nSuspends execution until a Task is completed.";
        case SN_TOK_PULSAR: return "```snova\npulsar func / pulsar <expr>\n```\nSpawns an actor / concurrent lightweight task stream.";
        case SN_TOK_THIS: return "Refers to the current instance of the enclosing class or struct.";
        case SN_TOK_NEW: return "Instantiates a type or constructor.";
        case SN_TOK_TRY: return "```snova\ntry { ... } catch (e: Error) { ... }\n```\nCatches runtime errors and exceptions.";
        case SN_TOK_THROW: return "```snova\nthrow <error-expr>\n```\nThrows an error or exception.";
        case SN_TOK_DEFER: return "```snova\ndefer { ... }\n```\nExecutes cleanup code upon scope exit.";
        case SN_TOK_PUBLIC: return "Visibility modifier: accessible from any package.";
        case SN_TOK_PRIVATE: return "Visibility modifier: accessible only within the declaring type/file.";
        case SN_TOK_PROTECTED: return "Visibility modifier: accessible within the type and subclasses.";
        case SN_TOK_OVERRIDE: return "Indicates that a method overrides a supertype method.";
        case SN_TOK_STATIC: return "Declares a member belonging to the type rather than instances.";
        default: return NULL;
    }
}

static void format_type_repr(const SnType *t, char *buf, size_t buf_sz) {
    if (!t) {
        snprintf(buf, buf_sz, "unit");
        return;
    }
    if (t->kind == SN_TYPE_NAME) {
        snprintf(buf, buf_sz, "%s%s", t->name ? t->name : "unknown", t->is_optional ? "?" : "");
    } else if (t->kind == SN_TYPE_FUNC) {
        size_t used = (size_t)snprintf(buf, buf_sz, "(");
        for (size_t i = 0; i < t->params.len && used < buf_sz; i++) {
            const SnType *param = SN_LIST_AT(t->params, const SnType, i);
            char param_text[96];
            format_type_repr(param, param_text, sizeof(param_text));
            if (i) used += (size_t)snprintf(buf + used, buf_sz - used, ", ");
            used += (size_t)snprintf(buf + used, buf_sz - used, "%s", param_text);
        }
        if (used < buf_sz) {
            const char *ret = t->ret ? (t->ret->name ? t->ret->name : "unit") : "unit";
            snprintf(buf + used, buf_sz - used, ") -> %s", ret);
        }
    } else {
        snprintf(buf, buf_sz, "unknown");
    }
}

static void format_resolved_type(const SnTypeRep *t, char *buf, size_t buf_sz) {
    if (!t) { snprintf(buf, buf_sz, "any"); return; }
    switch (t->tag) {
        case SN_T_INT: snprintf(buf, buf_sz, "int"); break;
        case SN_T_LONG: snprintf(buf, buf_sz, "long"); break;
        case SN_T_DOUBLE: snprintf(buf, buf_sz, "double"); break;
        case SN_T_DECIMAL: snprintf(buf, buf_sz, "decimal"); break;
        case SN_T_FLOAT: snprintf(buf, buf_sz, "float"); break;
        case SN_T_BYTE: snprintf(buf, buf_sz, "byte"); break;
        case SN_T_BOOL: snprintf(buf, buf_sz, "bool"); break;
        case SN_T_STRING: snprintf(buf, buf_sz, "string"); break;
        case SN_T_CHAR: snprintf(buf, buf_sz, "char"); break;
        case SN_T_UNIT: snprintf(buf, buf_sz, "unit"); break;
        case SN_T_ANY: snprintf(buf, buf_sz, "any"); break;
        case SN_T_TYPEVAR: snprintf(buf, buf_sz, "%s", t->decl && t->decl->name ? t->decl->name : "T"); break;
        case SN_T_NAMED: {
            const char *name = t->decl && t->decl->name ? t->decl->name : "Type";
            if (t->nargs == 0) snprintf(buf, buf_sz, "%s", name);
            else {
                size_t used = (size_t)snprintf(buf, buf_sz, "%s<", name);
                for (uint32_t i = 0; i < t->nargs && used < buf_sz; i++) {
                    if (i) used += (size_t)snprintf(buf + used, buf_sz - used, ", ");
                    char arg[96]; format_resolved_type(t->args[i], arg, sizeof(arg));
                    used += (size_t)snprintf(buf + used, buf_sz - used, "%s", arg);
                }
                if (used < buf_sz) snprintf(buf + used, buf_sz - used, ">");
            }
            break;
        }
        case SN_T_ARRAY: {
            char elem[96]; format_resolved_type(t->nargs ? t->args[0] : NULL, elem, sizeof(elem));
            snprintf(buf, buf_sz, "Array<%s>", elem);
            break;
        }
        case SN_T_FUNC: {
            size_t used = (size_t)snprintf(buf, buf_sz, "(");
            for (uint32_t i = 0; i < t->nargs && used < buf_sz; i++) {
                char param[96];
                format_resolved_type(t->args[i], param, sizeof(param));
                if (i) used += (size_t)snprintf(buf + used, buf_sz - used, ", ");
                used += (size_t)snprintf(buf + used, buf_sz - used, "%s", param);
            }
            if (used < buf_sz) {
                char ret[96];
                format_resolved_type(t->ret, ret, sizeof(ret));
                snprintf(buf + used, buf_sz - used, ") -> %s", ret);
            }
            break;
        }
        default: snprintf(buf, buf_sz, "any"); break;
    }
}

static const SnSymbol *find_type_reference(const LspDocAnalysis *a, const SnToken *tok) {
    if (!a || !tok || !tok->text) return NULL;
    const char *name = sn_intern_cstr((SnInternTable *)&a->intern, tok->text);
    SnScope *scope = sn_resolver_package_scope((SnResolver *)&a->resolver,
                                                a->unit.package ? a->unit.package : "main");
    if (scope) {
        SnSymbol *sym = sn_scope_lookup_local(scope, name);
        if (sym && sym->kind == SN_SYM_TYPE) return sym;
    }
    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        SnScope *import_scope = sn_resolver_package_scope((SnResolver *)&a->resolver, imp);
        if (!import_scope) continue;
        SnSymbol *sym = sn_scope_lookup_local(import_scope, name);
        if (sym && sym->kind == SN_SYM_TYPE) return sym;
    }
    if (a->resolver.prelude_scope) {
        SnSymbol *sym = sn_scope_lookup_local(a->resolver.prelude_scope, name);
        if (sym && sym->kind == SN_SYM_TYPE) return sym;
    }
    return NULL;
}

static void format_decl_signature(const SnDecl *d, char *out, size_t out_sz) {
    if (!d) return;
    char vis[32] = "";
    if (d->vis == SN_VIS_PUBLIC) strcpy(vis, "public ");
    else if (d->vis == SN_VIS_PRIVATE) strcpy(vis, "private ");
    else if (d->vis == SN_VIS_PROTECTED) strcpy(vis, "protected ");

    char st[32] = "";
    if (d->is_static) strcpy(st, "static ");
    if (d->is_async) strcat(st, "async ");
    if (d->is_pulsar) strcat(st, "pulsar ");
    if (d->is_override) strcat(st, "override ");

    switch (d->kind) {
        case SN_DECL_CLASS:
            snprintf(out, out_sz, "```snova\n%s%sclass %s\n```", vis, st, d->name ? d->name : "");
            break;
        case SN_DECL_STRUCT:
            snprintf(out, out_sz, "```snova\n%s%sstruct %s\n```", vis, st, d->name ? d->name : "");
            break;
        case SN_DECL_INTERFACE:
            snprintf(out, out_sz, "```snova\n%s%sinterface %s\n```", vis, st, d->name ? d->name : "");
            break;
        case SN_DECL_ENUM:
            snprintf(out, out_sz, "```snova\n%senum %s\n```", vis, d->name ? d->name : "");
            break;
        case SN_DECL_FUNC: {
            char ret[64] = "unit";
            if (d->ret) format_type_repr(d->ret, ret, sizeof(ret));
            char params[512] = "";
            for (size_t i = 0; i < d->params.len; i++) {
                const SnParam *p = SN_LIST_AT(d->params, const SnParam, i);
                char pt[96] = "any";
                if (p && p->type) format_type_repr(p->type, pt, sizeof(pt));
                if (i) strncat(params, ", ", sizeof(params) - strlen(params) - 1);
                char part[128];
                snprintf(part, sizeof(part), "%s: %s", p && p->name ? p->name : "_", pt);
                strncat(params, part, sizeof(params) - strlen(params) - 1);
            }
            snprintf(out, out_sz, "```snova\n%s%sfunc %s(%s): %s\n```", vis, st, d->name ? d->name : "", params, ret);
            break;
        }
        case SN_DECL_METHOD: {
            char ret[64] = "unit";
            if (d->ret) format_type_repr(d->ret, ret, sizeof(ret));
            char params[512] = "";
            for (size_t i = 0; i < d->params.len; i++) {
                const SnParam *p = SN_LIST_AT(d->params, const SnParam, i);
                char pt[96] = "any";
                if (p && p->type) format_type_repr(p->type, pt, sizeof(pt));
                if (i) strncat(params, ", ", sizeof(params) - strlen(params) - 1);
                char part[128];
                snprintf(part, sizeof(part), "%s: %s", p && p->name ? p->name : "_", pt);
                strncat(params, part, sizeof(params) - strlen(params) - 1);
            }
            snprintf(out, out_sz, "```snova\n%s%smethod %s(%s): %s\n```", vis, st, d->name ? d->name : "", params, ret);
            break;
        }
        case SN_DECL_FIELD: {
            char typ[64] = "unknown";
            if (d->type) format_type_repr(d->type, typ, sizeof(typ));
            const char *mut = d->is_mutable ? "var" : "let";
            snprintf(out, out_sz, "```snova\n%s%s%s %s: %s\n```", vis, st, mut, d->name ? d->name : "", typ);
            break;
        }
        case SN_DECL_CONST: {
            char typ[64] = "unknown";
            if (d->type) format_type_repr(d->type, typ, sizeof(typ));
            snprintf(out, out_sz, "```snova\n%sconst %s: %s\n```", vis, d->name ? d->name : "", typ);
            break;
        }
        case SN_DECL_VARIANT:
            snprintf(out, out_sz, "```snova\n(variant) %s\n```", d->name ? d->name : "");
            break;
        default:
            snprintf(out, out_sz, "```snova\n%s\n```", d->name ? d->name : "");
            break;
    }
}

/* Parses doc comment format:
 *   -- Doc:{Name}
 *   -- Description: ...
 *   -- Param{name}: ...
 *   -- Returns: ...
 */
static void extract_doc_comment(const char *src, size_t src_len, uint32_t decl_offset, char *out, size_t out_sz) {
    if (!src || decl_offset == 0 || decl_offset > src_len) return;

    // Scan backwards from decl_offset to find comment
    size_t p = decl_offset;
    while (p > 0 && (src[p - 1] == ' ' || src[p - 1] == '\t' || src[p - 1] == '\n' || src[p - 1] == '\r')) {
        p--;
    }

    if (p < 2 || src[p - 2] != '*' || src[p - 1] != '/') {
        return;
    }

    size_t comment_end = p;
    size_t comment_start = comment_end - 2;
    while (comment_start > 0 && !(src[comment_start] == '/' && src[comment_start + 1] == '*')) {
        comment_start--;
    }

    if (src[comment_start] != '/' || src[comment_start + 1] != '*') {
        return;
    }

    // Parse comment lines
    const char *c = src + comment_start;
    size_t clen = comment_end - comment_start;
    
    char desc[1024] = {0};
    char params[1024] = {0};
    char returns[256] = {0};

    char line[512];
    size_t i = 0;
    while (i < clen) {
        size_t lstart = i;
        while (i < clen && c[i] != '\n') i++;
        size_t llen = i - lstart;
        if (i < clen && c[i] == '\n') i++;

        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, c + lstart, llen);
        line[llen] = '\0';

        // Strip leading whitespace and '*'
        char *lp = line;
        while (*lp == ' ' || *lp == '\t' || *lp == '/' || *lp == '*') lp++;

        if (strncmp(lp, "-- Description:", 15) == 0) {
            lp += 15;
            while (*lp == ' ') lp++;
            snprintf(desc + strlen(desc), sizeof(desc) - strlen(desc), "%s ", lp);
        } else if (strncmp(lp, "-- Param{", 9) == 0) {
            char *pname = lp + 9;
            char *pend = strchr(pname, '}');
            if (pend) {
                *pend = '\0';
                char *pval = pend + 1;
                if (*pval == ':') pval++;
                while (*pval == ' ') pval++;
                snprintf(params + strlen(params), sizeof(params) - strlen(params), "\n- `%s`: %s", pname, pval);
            }
        } else if (strncmp(lp, "-- Returns:", 11) == 0) {
            lp += 11;
            while (*lp == ' ') lp++;
            snprintf(returns, sizeof(returns), "%s", lp);
        } else if (strncmp(lp, "-- ", 3) == 0 && desc[0] != '\0' && params[0] == '\0' && returns[0] == '\0') {
            lp += 3;
            snprintf(desc + strlen(desc), sizeof(desc) - strlen(desc), "%s ", lp);
        }
    }

    if (desc[0] != '\0' || params[0] != '\0' || returns[0] != '\0') {
        size_t cur = strlen(out);
        if (desc[0] != '\0') {
            snprintf(out + cur, out_sz - cur, "\n\n%s", desc);
            cur = strlen(out);
        }
        if (params[0] != '\0') {
            snprintf(out + cur, out_sz - cur, "\n\n**Parameters:**%s", params);
            cur = strlen(out);
        }
        if (returns[0] != '\0') {
            snprintf(out + cur, out_sz - cur, "\n\n**Returns:** %s", returns);
        }
    }
}

char *lsp_hover_query(LspAnalysisEngine *engine, const LspDocument *doc, LspPosition pos) {
    if (!doc) return NULL;
    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, NULL, doc);
    }
    if (!a) return NULL;

    uint32_t offset = lsp_pos_to_offset(doc, pos);
    const SnToken *tok = lsp_find_token_at(a, offset);
    if (!tok) return NULL;

    char hover_text[4096] = {0};

    // 1. Keyword doc
    if (sn_tok_is_keyword(tok->kind)) {
        const char *kw_doc = get_keyword_doc(tok->kind);
        if (kw_doc) {
            snprintf(hover_text, sizeof(hover_text), "%s", kw_doc);
        }
    }

    // 2. Builtin types
    if (hover_text[0] == '\0' && tok->text) {
        if (strcmp(tok->text, "int") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype int\n```\n32-bit signed integer.");
        } else if (strcmp(tok->text, "long") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype long\n```\n64-bit signed integer.");
        } else if (strcmp(tok->text, "double") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype double\n```\n64-bit IEEE 754 floating point number.");
        } else if (strcmp(tok->text, "decimal") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype decimal\n```\n128-bit high-precision fixed/decimal number.");
        } else if (strcmp(tok->text, "string") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype string\n```\nImmutable UTF-8 string with string interpolation support.");
        } else if (strcmp(tok->text, "bool") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype bool\n```\nBoolean value (`true` or `false`).");
        } else if (strcmp(tok->text, "unit") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\ntype unit\n```\nUnit type indicating no returned value (void).");
        } else if (strcmp(tok->text, "Option") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\nenum Option<T> {\n    Some(T),\n    None\n}\n```\nOptional value container.");
        } else if (strcmp(tok->text, "Result") == 0) {
            snprintf(hover_text, sizeof(hover_text), "```snova\nenum Result<T, E> {\n    Ok(T),\n    Err(E)\n}\n```\nError handling container.");
        }
    }

    // 3. Type references must prefer a type symbol over a same-named value/member.
    const SnSymbol *type_sym = find_type_reference(a, tok);
    if (type_sym && type_sym->decl) {
        format_decl_signature(type_sym->decl, hover_text, sizeof(hover_text));
    }

    // 3. Symbol lookup with doc comment extraction
    if (hover_text[0] == '\0') {
        const char *name = NULL;
        const SnSymbol *sym = lsp_find_symbol_at(a, doc, offset, &name);
        if (sym && sym->decl) {
            format_decl_signature(sym->decl, hover_text, sizeof(hover_text));
            const char *src = sym->origin ? sym->origin->src : doc->text;
            size_t src_len = sym->origin ? sym->origin->src_len : doc->text_len;
            extract_doc_comment(src, src_len, sym->decl->span.offset, hover_text, sizeof(hover_text));
        } else if (sym) {
            const char *kind_str = (sym->kind == SN_SYM_PARAM) ? "parameter" : "variable";
            const SnTypeRep *value_type = sym->value_type;
            if (!value_type && (sym->kind == SN_SYM_LOCAL || sym->kind == SN_SYM_PARAM)) {
                SnDiagSink null_diag;
                sn_diag_init(&null_diag, a->path ? a->path : "", "", 0);
                null_diag.out = NULL;
                null_diag.quiet = 1;

                SnChecker checker;
                sn_checker_init(&checker, (SnArena *)&a->arena, (SnInternTable *)&a->intern,
                                &null_diag, (SnResolver *)&a->resolver, (SnTypeTable *)&a->types);
                SnScope *local_scope = lsp_build_scope_at(
                    a, &checker, offset, NULL, NULL);
                value_type = lsp_infer_expr_type_at(a, &checker, local_scope,
                                                    sym->name);
            }
            if (value_type) {
                char ty[128];
                format_resolved_type(value_type, ty, sizeof(ty));
                snprintf(hover_text, sizeof(hover_text), "```snova\n(%s) %s: %s\n```",
                         kind_str, sym->name ? sym->name : "", ty);
            } else {
                snprintf(hover_text, sizeof(hover_text), "```snova\n(%s) %s\n```", kind_str, sym->name ? sym->name : "");
            }
        }
    }

    // 4. Fallback to declaration under cursor
    if (hover_text[0] == '\0') {
        const SnDecl *decl = lsp_find_decl_at(a, offset);
        if (decl) {
            format_decl_signature(decl, hover_text, sizeof(hover_text));
            extract_doc_comment(doc->text, doc->text_len, decl->span.offset, hover_text, sizeof(hover_text));
        }
    }

    if (hover_text[0] == '\0') {
        return NULL;
    }

    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    
    jb_key(&jb, "contents");
    jb_start_obj(&jb);
    jb_kv_str(&jb, "kind", "markdown");
    jb_kv_str(&jb, "value", hover_text);
    jb_end_obj(&jb);

    LspRange r = lsp_span_to_range(doc, tok->span.offset, tok->span.len, tok->span.line, tok->span.col);
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
    return jb_take(&jb);
}
