#include "lsp_hover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *get_keyword_doc(SnTokKind kind) {
    switch (kind) {
        case SN_TOK_PACKAGE: return "```snova\npackage <name>\n```\nDeclares the package namespace for this source file.";
        case SN_TOK_IMPORT: return "```snova\nimport <package>\n```\nImports symbols and types from another package.";
        case SN_TOK_CLASS: return "```snova\nclass <Name> { ... }\n```\nDeclares a reference type with fields, methods, and inheritance.";
        case SN_TOK_STRUCT: return "```snova\nstruct <Name> { ... }\n```\nDeclares a value type with fields and methods.";
        case SN_TOK_ENUM: return "```snova\nenum <Name> { Variant1, Variant2(T) }\n```\nDeclares an algebraic data type / tagged union.";
        case SN_TOK_INTERFACE: return "```snova\ninterface <Name> { ... }\n```\nDefines a contract of method signatures.";
        case SN_TOK_FUNC: return "```snova\nfunc <name>(params): ReturnType { ... }\n```\nDeclares a top-level function.";
        case SN_TOK_METHOD: return "```snova\nmethod <name>(params): ReturnType { ... }\n```\nDeclares a member method within a class, struct, or interface.";
        case SN_TOK_LET: return "```snova\nlet <name>[: Type] = <expr>\n```\nDeclares an immutable local binding or field.";
        case SN_TOK_VAR: return "```snova\nvar <name>[: Type] = <expr>\n```\nDeclares a mutable variable or field.";
        case SN_TOK_CONST: return "```snova\nconst <name>[: Type] = <expr>\n```\nDeclares a compile-time constant.";
        case SN_TOK_IF: return "```snova\nif <cond> { ... } else { ... }\n```\nConditional execution or ternary expression.";
        case SN_TOK_WHILE: return "```snova\nwhile <cond> { ... }\n```\nRepeats a block of statements while the condition is true.";
        case SN_TOK_FOR: return "```snova\nfor (let item in collection) { ... }\n```\nIterates over elements in an iterable or stream.";
        case SN_TOK_MATCH: return "```snova\nmatch <expr> { pattern => ... }\n```\nPattern matching on values, enums, and structures.";
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
        snprintf(buf, buf_sz, "(...) -> %s", t->ret ? (t->ret->name ? t->ret->name : "unit") : "unit");
    } else {
        snprintf(buf, buf_sz, "unknown");
    }
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
            snprintf(out, out_sz, "```snova\n%s%sfunc %s(...): %s\n```", vis, st, d->name ? d->name : "", ret);
            break;
        }
        case SN_DECL_METHOD: {
            char ret[64] = "unit";
            if (d->ret) format_type_repr(d->ret, ret, sizeof(ret));
            snprintf(out, out_sz, "```snova\n%s%smethod %s(...): %s\n```", vis, st, d->name ? d->name : "", ret);
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

    char hover_text[2048] = {0};

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

    // 3. Symbol lookup
    if (hover_text[0] == '\0') {
        const char *name = NULL;
        const SnSymbol *sym = lsp_find_symbol_at(a, doc, offset, &name);
        if (sym && sym->decl) {
            format_decl_signature(sym->decl, hover_text, sizeof(hover_text));
        } else if (sym) {
            const char *kind_str = (sym->kind == SN_SYM_PARAM) ? "parameter" : "variable";
            snprintf(hover_text, sizeof(hover_text), "```snova\n(%s) %s\n```", kind_str, sym->name ? sym->name : "");
        }
    }

    // 4. Fallback to declaration under cursor
    if (hover_text[0] == '\0') {
        const SnDecl *decl = lsp_find_decl_at(a, offset);
        if (decl) {
            format_decl_signature(decl, hover_text, sizeof(hover_text));
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
