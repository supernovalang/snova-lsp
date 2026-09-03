#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_completion.h"
#include "lsp_analysis.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ── Completion Candidate Struct & List ───────────────────────────────────── */

typedef struct {
    char *label;
    LspCompletionKind kind;
    char *detail;
    char *doc;
    char *insert_text;
    int insert_text_format; /* 1 = PlainText, 2 = Snippet */
    char *filter_text;
    char *additional_import;
    int base_score;
    int type_bonus;
    int match_bonus;
    int final_score;
} CompItem;

typedef struct {
    CompItem *items;
    size_t len;
    size_t cap;
} CompList;

static void complist_init(CompList *list) {
    list->len = 0;
    list->cap = 64;
    list->items = (CompItem *)malloc(sizeof(CompItem) * list->cap);
}

static void complist_free(CompList *list) {
    if (!list || !list->items) return;
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].label) free(list->items[i].label);
        if (list->items[i].detail) free(list->items[i].detail);
        if (list->items[i].doc) free(list->items[i].doc);
        if (list->items[i].insert_text) free(list->items[i].insert_text);
        if (list->items[i].filter_text) free(list->items[i].filter_text);
        if (list->items[i].additional_import) free(list->items[i].additional_import);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static bool complist_has_label(const CompList *list, const char *label) {
    if (!list || !label) return false;
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].label && strcmp(list->items[i].label, label) == 0) {
            return true;
        }
    }
    return false;
}

static void complist_add(CompList *list, const char *label, LspCompletionKind kind,
                         const char *detail, const char *doc, const char *insert_text,
                         int insert_format, const char *additional_import,
                         int base_score, int type_bonus) {
    if (!list || !label || !label[0]) return;
    if (complist_has_label(list, label)) return;

    if (list->len >= list->cap) {
        list->cap *= 2;
        list->items = (CompItem *)realloc(list->items, sizeof(CompItem) * list->cap);
    }

    CompItem *ci = &list->items[list->len++];
    ci->label = strdup(label);
    ci->kind = kind;
    ci->detail = detail ? strdup(detail) : NULL;
    ci->doc = doc ? strdup(doc) : NULL;
    ci->insert_text = insert_text ? strdup(insert_text) : strdup(label);
    ci->insert_text_format = insert_format;
    ci->filter_text = strdup(label);
    ci->additional_import = additional_import ? strdup(additional_import) : NULL;
    ci->base_score = base_score;
    ci->type_bonus = type_bonus;
    ci->match_bonus = 0;
    ci->final_score = 0;
}

/* ── Ranking & Fuzzy Matching (Gopls & Rust-Analyzer tier ranking) ────────── */

static bool acronym_matches(const char *label, const char *prefix) {
    if (!label || !prefix || !prefix[0]) return false;
    size_t pi = 0;
    size_t plen = strlen(prefix);

    for (size_t i = 0; label[i] && pi < plen; i++) {
        bool is_hump = (i == 0) || (isupper((unsigned char)label[i]) && !isupper((unsigned char)label[i - 1])) ||
                       (label[i - 1] == '_' && label[i] != '_');
        if (is_hump) {
            if (tolower((unsigned char)label[i]) == tolower((unsigned char)prefix[pi])) {
                pi++;
            }
        }
    }
    return (pi == plen);
}

static bool subsequence_matches(const char *label, const char *prefix) {
    if (!label || !prefix || !prefix[0]) return false;
    size_t pi = 0;
    size_t plen = strlen(prefix);
    for (size_t i = 0; label[i] && pi < plen; i++) {
        if (tolower((unsigned char)label[i]) == tolower((unsigned char)prefix[pi])) {
            pi++;
        }
    }
    return (pi == plen);
}

static bool case_insensitive_contains(const char *text, const char *needle) {
    if (!text || !needle || !needle[0]) return false;
    size_t n = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static int compute_match_bonus(const char *label, const char *prefix) {
    if (!prefix || !prefix[0]) return 0;
    if (!label) return -1;

    size_t plen = strlen(prefix);

    // Tier 1: Exact case-sensitive match
    if (strcmp(label, prefix) == 0) return 120;

    // Tier 2: Exact case-insensitive match
    if (strcasecmp(label, prefix) == 0) return 100;

    // Tier 3: Prefix case-sensitive match
    if (strncmp(label, prefix, plen) == 0) return 80;

    // Tier 4: Prefix case-insensitive match
    if (strncasecmp(label, prefix, plen) == 0) return 60;

    // Tier 5: Acronym match (camelCase humps)
    if (acronym_matches(label, prefix)) return 35;

    // Tier 6: Subsequence fuzzy match
    if (subsequence_matches(label, prefix)) return 20;

    // Tier 7: Substring match
    if (case_insensitive_contains(label, prefix)) return 15;

    return -1;
}

static void rank_and_sort_candidates(CompList *list, const char *prefix) {
    if (!list || list->len == 0) return;

    // 1. Calculate scores and filter out non-matching candidates
    for (size_t i = 0; i < list->len; i++) {
        CompItem *ci = &list->items[i];
        int mb = compute_match_bonus(ci->label, prefix);
        if (prefix && prefix[0] && mb < 0) {
            ci->final_score = -1;
            continue;
        }
        ci->match_bonus = (mb >= 0) ? mb : 0;
        ci->final_score = ci->base_score + (ci->type_bonus * 3) + ci->match_bonus;
    }

    // 2. Compact valid items
    size_t dst = 0;
    for (size_t src = 0; src < list->len; src++) {
        if (list->items[src].final_score >= 0) {
            if (dst != src) {
                list->items[dst] = list->items[src];
            }
            dst++;
        } else {
            if (list->items[src].label) free(list->items[src].label);
            if (list->items[src].detail) free(list->items[src].detail);
            if (list->items[src].doc) free(list->items[src].doc);
            if (list->items[src].insert_text) free(list->items[src].insert_text);
            if (list->items[src].filter_text) free(list->items[src].filter_text);
            if (list->items[src].additional_import) free(list->items[src].additional_import);
        }
    }
    list->len = dst;

    // 3. Insertion sort
    for (size_t i = 1; i < list->len; i++) {
        CompItem key = list->items[i];
        size_t j = i;
        while (j > 0) {
            CompItem *prev = &list->items[j - 1];
            bool swap = false;
            if (key.final_score > prev->final_score) {
                swap = true;
            } else if (key.final_score == prev->final_score) {
                size_t klen = strlen(key.label);
                size_t plen = strlen(prev->label);
                if (klen < plen) {
                    swap = true;
                } else if (klen == plen && strcasecmp(key.label, prev->label) < 0) {
                    swap = true;
                }
            }

            if (swap) {
                list->items[j] = list->items[j - 1];
                j--;
            } else {
                break;
            }
        }
        list->items[j] = key;
    }
}

/* ── Context Analysis ─────────────────────────────────────────────────────── */

typedef enum {
    CTX_GENERAL = 0,
    CTX_MEMBER,
    CTX_TYPE_POS,
    CTX_DECORATOR,
    CTX_IMPORT_LINE
} ComplContext;

static void extract_line_info(const char *doc_text, uint32_t target_line, uint32_t target_col,
                              char *line_buf, size_t line_buf_sz,
                              char *prefix_buf, size_t prefix_buf_sz,
                              char *receiver_buf, size_t receiver_buf_sz,
                              ComplContext *out_ctx, bool *out_has_following_paren) {
    line_buf[0] = '\0';
    prefix_buf[0] = '\0';
    receiver_buf[0] = '\0';
    *out_ctx = CTX_GENERAL;
    *out_has_following_paren = false;

    if (!doc_text) return;

    const char *p = doc_text;
    uint32_t cur_line = 0;
    while (*p && cur_line < target_line) {
        if (*p == '\n') cur_line++;
        p++;
    }

    size_t li = 0;
    while (*p && *p != '\r' && *p != '\n' && li + 1 < line_buf_sz) {
        line_buf[li++] = *p++;
    }
    line_buf[li] = '\0';

    size_t col = target_col;
    if (col > li) col = li;

    size_t fp = col;
    while (fp < li && (line_buf[fp] == ' ' || line_buf[fp] == '\t')) fp++;
    if (fp < li && line_buf[fp] == '(') {
        *out_has_following_paren = true;
    }

    size_t start = col;
    while (start > 0) {
        char c = line_buf[start - 1];
        if (isalnum((unsigned char)c) || c == '_') {
            start--;
        } else {
            break;
        }
    }
    size_t plen = col - start;
    if (plen >= prefix_buf_sz) plen = prefix_buf_sz - 1;
    memcpy(prefix_buf, line_buf + start, plen);
    prefix_buf[plen] = '\0';

    size_t prec = start;
    while (prec > 0 && (line_buf[prec - 1] == ' ' || line_buf[prec - 1] == '\t')) prec--;

    if (prec > 0 && line_buf[prec - 1] == '.') {
        *out_ctx = CTX_MEMBER;
        size_t r_end = prec - 1;
        while (r_end > 0 && (line_buf[r_end - 1] == ' ' || line_buf[r_end - 1] == '\t')) r_end--;
        if (r_end > 0 && line_buf[r_end - 1] == '?') {
            r_end--;
            while (r_end > 0 && (line_buf[r_end - 1] == ' ' || line_buf[r_end - 1] == '\t')) r_end--;
        }

        int paren_depth = 0;
        int bracket_depth = 0;
        size_t r_start = r_end;
        while (r_start > 0) {
            char c = line_buf[r_start - 1];
            if (c == ')') paren_depth++;
            else if (c == '(') {
                if (paren_depth > 0) paren_depth--;
                else break;
            } else if (c == ']') bracket_depth++;
            else if (c == '[') {
                if (bracket_depth > 0) bracket_depth--;
                else break;
            } else if (paren_depth == 0 && bracket_depth == 0) {
                if (!isalnum((unsigned char)c) && c != '_' && c != '.' && c != '?' && c != '$' && c != '"' && c != '\'') {
                    break;
                }
            }
            r_start--;
        }
        while (r_start < r_end && (line_buf[r_start] == ' ' || line_buf[r_start] == '\t')) r_start++;
        size_t rlen = r_end - r_start;
        if (rlen >= receiver_buf_sz) rlen = receiver_buf_sz - 1;
        memcpy(receiver_buf, line_buf + r_start, rlen);
        receiver_buf[rlen] = '\0';
        return;
    }

    if (prec > 0 && line_buf[prec - 1] == '@') {
        *out_ctx = CTX_DECORATOR;
        return;
    }

    const char *trimmed = line_buf;
    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
    if (strncmp(trimmed, "import ", 7) == 0 || strncmp(trimmed, "using ", 6) == 0) {
        *out_ctx = CTX_IMPORT_LINE;
        return;
    }

    if (prec > 0 && (line_buf[prec - 1] == ':' || line_buf[prec - 1] == '<')) {
        *out_ctx = CTX_TYPE_POS;
        return;
    }
}

/* ── Type Formatting Helpers ──────────────────────────────────────────────── */

static void format_typerep_string(const SnTypeRep *ty, char *out, size_t out_sz) {
    if (!ty) {
        snprintf(out, out_sz, "any");
        return;
    }
    switch (ty->tag) {
        case SN_T_UNIT: snprintf(out, out_sz, "unit"); break;
        case SN_T_BOOL: snprintf(out, out_sz, "bool"); break;
        case SN_T_INT: snprintf(out, out_sz, "int"); break;
        case SN_T_LONG: snprintf(out, out_sz, "long"); break;
        case SN_T_DOUBLE: snprintf(out, out_sz, "double"); break;
        case SN_T_DECIMAL: snprintf(out, out_sz, "decimal"); break;
        case SN_T_FLOAT: snprintf(out, out_sz, "float"); break;
        case SN_T_BYTE: snprintf(out, out_sz, "byte"); break;
        case SN_T_CHAR: snprintf(out, out_sz, "char"); break;
        case SN_T_STRING: snprintf(out, out_sz, "string"); break;
        case SN_T_ANY: snprintf(out, out_sz, "any"); break;
        case SN_T_ERROR: snprintf(out, out_sz, "any"); break;
        case SN_T_ARRAY: {
            char elem_str[128] = "any";
            if (ty->nargs > 0 && ty->args && ty->args[0]) {
                format_typerep_string(ty->args[0], elem_str, sizeof(elem_str));
            }
            snprintf(out, out_sz, "Array<%s>", elem_str);
            break;
        }
        case SN_T_NAMED: {
            const char *name = (ty->decl && ty->decl->name) ? ty->decl->name : "Type";
            if (ty->nargs > 0 && ty->args) {
                char args_buf[256] = "";
                for (uint32_t i = 0; i < ty->nargs; i++) {
                    char a_str[64];
                    format_typerep_string(ty->args[i], a_str, sizeof(a_str));
                    if (i > 0) strcat(args_buf, ", ");
                    strcat(args_buf, a_str);
                }
                snprintf(out, out_sz, "%s<%s>", name, args_buf);
            } else {
                snprintf(out, out_sz, "%s", name);
            }
            break;
        }
        case SN_T_TYPEVAR:
            snprintf(out, out_sz, "%s", (ty->decl && ty->decl->name) ? ty->decl->name : "T");
            break;
        case SN_T_FUNC: {
            char ret_str[64] = "unit";
            if (ty->ret) format_typerep_string(ty->ret, ret_str, sizeof(ret_str));
            snprintf(out, out_sz, "(...) -> %s", ret_str);
            break;
        }
        default:
            snprintf(out, out_sz, "any");
            break;
    }
}

/* ── Standard Keywords, Snippets, Builtin Types, and Decorators ──────────── */

static void add_keywords_and_snippets(CompList *list, ComplContext ctx, bool has_following_paren) {
    if (ctx == CTX_MEMBER || ctx == CTX_IMPORT_LINE) return;

    if (ctx == CTX_DECORATOR) {
        complist_add(list, "native", LSP_COMPLETION_KEYWORD, "decorator", "Marks a function or method as implemented natively", "native", 1, NULL, 90, 50);
        complist_add(list, "route", LSP_COMPLETION_KEYWORD, "decorator", "Declares an HTTP route endpoint", "route(\"${1:path}\")", 2, NULL, 90, 50);
        complist_add(list, "get", LSP_COMPLETION_KEYWORD, "decorator", "Declares an HTTP GET handler", "get(\"${1:path}\")", 2, NULL, 90, 50);
        complist_add(list, "post", LSP_COMPLETION_KEYWORD, "decorator", "Declares an HTTP POST handler", "post(\"${1:path}\")", 2, NULL, 90, 50);
        complist_add(list, "put", LSP_COMPLETION_KEYWORD, "decorator", "Declares an HTTP PUT handler", "put(\"${1:path}\")", 2, NULL, 90, 50);
        complist_add(list, "delete", LSP_COMPLETION_KEYWORD, "decorator", "Declares an HTTP DELETE handler", "delete(\"${1:path}\")", 2, NULL, 90, 50);
        complist_add(list, "table", LSP_COMPLETION_KEYWORD, "decorator", "Maps a struct/class to a database table", "table(\"${1:tableName}\")", 2, NULL, 90, 50);
        complist_add(list, "column", LSP_COMPLETION_KEYWORD, "decorator", "Maps a field to a database column", "column(\"${1:colName}\")", 2, NULL, 90, 50);
        complist_add(list, "serializable", LSP_COMPLETION_KEYWORD, "decorator", "Enables JSON/binary serialization", "serializable", 1, NULL, 90, 50);
        complist_add(list, "deprecated", LSP_COMPLETION_KEYWORD, "decorator", "Marks symbol as deprecated", "deprecated(\"${1:reason}\")", 2, NULL, 90, 50);
        complist_add(list, "test", LSP_COMPLETION_KEYWORD, "decorator", "Declares a unit test function", "test", 1, NULL, 90, 50);
        return;
    }

    int type_score = (ctx == CTX_TYPE_POS) ? 95 : 65;
    int type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;

    // Builtin Primitive Types
    complist_add(list, "int", LSP_COMPLETION_KEYWORD, "type", "64-bit signed integer", "int", 1, NULL, type_score, type_bonus);
    complist_add(list, "long", LSP_COMPLETION_KEYWORD, "type", "64-bit signed integer", "long", 1, NULL, type_score, type_bonus);
    complist_add(list, "double", LSP_COMPLETION_KEYWORD, "type", "64-bit IEEE 754 floating point", "double", 1, NULL, type_score, type_bonus);
    complist_add(list, "decimal", LSP_COMPLETION_KEYWORD, "type", "High-precision decimal number", "decimal", 1, NULL, type_score, type_bonus);
    complist_add(list, "string", LSP_COMPLETION_KEYWORD, "type", "UTF-8 immutable string", "string", 1, NULL, type_score, type_bonus);
    complist_add(list, "bool", LSP_COMPLETION_KEYWORD, "type", "Boolean true or false", "bool", 1, NULL, type_score, type_bonus);
    complist_add(list, "char", LSP_COMPLETION_KEYWORD, "type", "Unicode character", "char", 1, NULL, type_score, type_bonus);
    complist_add(list, "byte", LSP_COMPLETION_KEYWORD, "type", "8-bit unsigned byte", "byte", 1, NULL, type_score, type_bonus);
    complist_add(list, "unit", LSP_COMPLETION_KEYWORD, "type", "Unit / void return type", "unit", 1, NULL, type_score, type_bonus);
    complist_add(list, "List", LSP_COMPLETION_CLASS, "type List<T>", "Dynamic array collection", "List<${1:T}>", 2, NULL, type_score, type_bonus);
    complist_add(list, "Map", LSP_COMPLETION_CLASS, "type Map<K, V>", "Key-value hash map", "Map<${1:K}, ${2:V}>", 2, NULL, type_score, type_bonus);
    complist_add(list, "Option", LSP_COMPLETION_ENUM, "enum Option<T>", "Optional value: Some(T) or None", "Option<${1:T}>", 2, NULL, type_score, type_bonus);
    complist_add(list, "Result", LSP_COMPLETION_ENUM, "enum Result<T, E>", "Result value: Ok(T) or Err(E)", "Result<${1:T}, ${2:Error}>", 2, NULL, type_score, type_bonus);
    complist_add(list, "Task", LSP_COMPLETION_CLASS, "class Task<T>", "Asynchronous task handle", "Task<${1:T}>", 2, NULL, type_score, type_bonus);
    complist_add(list, "Array", LSP_COMPLETION_CLASS, "class Array<T>", "Fixed-size or slice array", "Array<${1:T}>", 2, NULL, type_score, type_bonus);

    if (ctx == CTX_TYPE_POS) {
        return;
    }

    // Builtin Functions & Constructors
    const char *call_suf = has_following_paren ? "" : "($1)";
    int call_fmt = has_following_paren ? 1 : 2;

    char insert_println[32], insert_print[32], insert_some[32], insert_ok[32], insert_err[32], insert_assert[32];
    snprintf(insert_println, sizeof(insert_println), "println%s", call_suf);
    snprintf(insert_print, sizeof(insert_print), "print%s", call_suf);
    snprintf(insert_some, sizeof(insert_some), "Some%s", call_suf);
    snprintf(insert_ok, sizeof(insert_ok), "Ok%s", call_suf);
    snprintf(insert_err, sizeof(insert_err), "Err%s", call_suf);
    snprintf(insert_assert, sizeof(insert_assert), "assert%s", call_suf);

    complist_add(list, "println", LSP_COMPLETION_FUNCTION, "func println(msg: string): unit", "Prints message with newline to stdout", insert_println, call_fmt, NULL, 70, 0);
    complist_add(list, "print", LSP_COMPLETION_FUNCTION, "func print(msg: string): unit", "Prints message to stdout", insert_print, call_fmt, NULL, 70, 0);
    complist_add(list, "Some", LSP_COMPLETION_CONSTRUCTOR, "Option.Some(value)", "Creates an Option with a value", insert_some, call_fmt, NULL, 70, 0);
    complist_add(list, "None", LSP_COMPLETION_ENUM_MEMBER, "Option.None", "Empty Option value", "None", 1, NULL, 70, 0);
    complist_add(list, "Ok", LSP_COMPLETION_CONSTRUCTOR, "Result.Ok(value)", "Creates a successful Result", insert_ok, call_fmt, NULL, 70, 0);
    complist_add(list, "Err", LSP_COMPLETION_CONSTRUCTOR, "Result.Err(error)", "Creates a failed Result", insert_err, call_fmt, NULL, 70, 0);
    complist_add(list, "assert", LSP_COMPLETION_FUNCTION, "func assert(cond: bool, msg: string): unit", "Asserts condition is true", insert_assert, call_fmt, NULL, 70, 0);
    complist_add(list, "true", LSP_COMPLETION_VALUE, "bool", "Boolean true", "true", 1, NULL, 70, 0);
    complist_add(list, "false", LSP_COMPLETION_VALUE, "bool", "Boolean false", "false", 1, NULL, 70, 0);
    complist_add(list, "null", LSP_COMPLETION_VALUE, "null", "Null reference", "null", 1, NULL, 65, 0);
    complist_add(list, "this", LSP_COMPLETION_VALUE, "this", "Current instance reference", "this", 1, NULL, 85, 0);

    // Keywords & Snippets
    complist_add(list, "let", LSP_COMPLETION_KEYWORD, "keyword", "Immutable local binding", "let ${1:name} = ${2:value};", 2, NULL, 40, 0);
    complist_add(list, "var", LSP_COMPLETION_KEYWORD, "keyword", "Mutable variable binding", "var ${1:name} = ${2:value};", 2, NULL, 40, 0);
    complist_add(list, "const", LSP_COMPLETION_KEYWORD, "keyword", "Compile-time constant", "const ${1:NAME}: ${2:Type} = ${3:value};", 2, NULL, 40, 0);
    complist_add(list, "func", LSP_COMPLETION_SNIPPET, "snippet", "Function declaration", "func ${1:name}(${2:params}): ${3:unit} {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "method", LSP_COMPLETION_SNIPPET, "snippet", "Method declaration", "method ${1:name}(${2:params}): ${3:unit} {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "class", LSP_COMPLETION_SNIPPET, "snippet", "Class declaration", "class ${1:Name} {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "struct", LSP_COMPLETION_SNIPPET, "snippet", "Struct declaration", "struct ${1:Name} {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "enum", LSP_COMPLETION_SNIPPET, "snippet", "Enum declaration", "enum ${1:Name} {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "interface", LSP_COMPLETION_SNIPPET, "snippet", "Interface declaration", "interface ${1:Name} {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "if", LSP_COMPLETION_SNIPPET, "snippet", "If statement", "if (${1:condition}) {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "else", LSP_COMPLETION_KEYWORD, "keyword", "Else branch", "else {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "while", LSP_COMPLETION_SNIPPET, "snippet", "While loop", "while (${1:condition}) {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "for", LSP_COMPLETION_SNIPPET, "snippet", "For-in loop", "for (let ${1:item} in ${2:collection}) {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "match", LSP_COMPLETION_SNIPPET, "snippet", "Match pattern expression", "match ${1:expr} {\n\t${2:pattern} -> $0\n}", 2, NULL, 40, 0);
    complist_add(list, "return", LSP_COMPLETION_KEYWORD, "keyword", "Return statement", "return ${1:value};", 2, NULL, 40, 0);
    complist_add(list, "async", LSP_COMPLETION_KEYWORD, "keyword", "Async function modifier", "async func ", 1, NULL, 40, 0);
    complist_add(list, "await", LSP_COMPLETION_KEYWORD, "keyword", "Await async future expression", "await ", 1, NULL, 40, 0);
    complist_add(list, "pulsar", LSP_COMPLETION_KEYWORD, "keyword", "Pulsar actor / task modifier", "pulsar ", 1, NULL, 40, 0);
    complist_add(list, "pulsar func", LSP_COMPLETION_SNIPPET, "pulsar function", "Declares a Pulsar function that can participate in asynchronous streams", "pulsar func ${1:name}(${2:params}): ${3:unit} {\n\t$0\n}", 2, NULL, 52, 0);
    complist_add(list, "pulsar method", LSP_COMPLETION_SNIPPET, "pulsar method", "Declares a Pulsar method on the current type", "pulsar method ${1:name}(${2:params}): ${3:unit} {\n\t$0\n}", 2, NULL, 52, 0);
    complist_add(list, "try", LSP_COMPLETION_SNIPPET, "snippet", "Try-catch block", "try {\n\t${1:/* work */}\n} catch (e: Error) {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "throw", LSP_COMPLETION_KEYWORD, "keyword", "Throw error statement", "throw ${1:error};", 2, NULL, 40, 0);
    complist_add(list, "defer", LSP_COMPLETION_SNIPPET, "snippet", "Defer cleanup block", "defer {\n\t$0\n}", 2, NULL, 40, 0);
    complist_add(list, "public", LSP_COMPLETION_KEYWORD, "keyword", "Public visibility", "public ", 1, NULL, 35, 0);
    complist_add(list, "private", LSP_COMPLETION_KEYWORD, "keyword", "Private visibility", "private ", 1, NULL, 35, 0);
    complist_add(list, "protected", LSP_COMPLETION_KEYWORD, "keyword", "Protected visibility", "protected ", 1, NULL, 35, 0);
    complist_add(list, "override", LSP_COMPLETION_KEYWORD, "keyword", "Override method modifier", "override ", 1, NULL, 35, 0);
    complist_add(list, "static", LSP_COMPLETION_KEYWORD, "keyword", "Static member modifier", "static ", 1, NULL, 35, 0);
    complist_add(list, "import", LSP_COMPLETION_KEYWORD, "keyword", "Import package", "import ${1:package};", 2, NULL, 35, 0);
    complist_add(list, "package", LSP_COMPLETION_KEYWORD, "keyword", "Package declaration", "package ${1:name};", 2, NULL, 35, 0);
}

/* ── Builtin Type Members ─────────────────────────────────────────────────── */

static void add_array_builtin_members(CompList *list, bool has_following_paren) {
    int cf = has_following_paren ? 1 : 2;

    complist_add(list, "length", LSP_COMPLETION_FIELD, "let length: int", "Number of elements in array", "length", 1, NULL, 90, 0);
    complist_add(list, "len", LSP_COMPLETION_METHOD, "method len(): int", "Number of elements in array", has_following_paren ? "len" : "len()", 1, NULL, 90, 0);
    complist_add(list, "push", LSP_COMPLETION_METHOD, "method push(item: T): unit", "Appends an element to the end", has_following_paren ? "push" : "push($1)", cf, NULL, 90, 0);
    complist_add(list, "get", LSP_COMPLETION_METHOD, "method get(index: int): T", "Gets element at index", has_following_paren ? "get" : "get(${1:index})", cf, NULL, 90, 0);
    complist_add(list, "set", LSP_COMPLETION_METHOD, "method set(index: int, item: T): unit", "Sets element at index", has_following_paren ? "set" : "set(${1:index}, ${2:item})", cf, NULL, 90, 0);
    complist_add(list, "map", LSP_COMPLETION_METHOD, "method map(fn: (T) -> U): Array<U>", "Transforms each element", has_following_paren ? "map" : "map(${1:item} -> $0)", cf, NULL, 90, 0);
    complist_add(list, "flatMap", LSP_COMPLETION_METHOD, "method flatMap(fn: (T) -> Array<U>): Array<U>", "Maps and flattens result array", has_following_paren ? "flatMap" : "flatMap(${1:item} -> $0)", cf, NULL, 90, 0);
    complist_add(list, "filter", LSP_COMPLETION_METHOD, "method filter(fn: (T) -> bool): Array<T>", "Filters elements matching predicate", has_following_paren ? "filter" : "filter(${1:item} -> $0)", cf, NULL, 90, 0);
    complist_add(list, "forEach", LSP_COMPLETION_METHOD, "method forEach(fn: (T) -> unit): unit", "Iterates over each element", has_following_paren ? "forEach" : "forEach(${1:item} -> $0)", cf, NULL, 90, 0);
    complist_add(list, "reduce", LSP_COMPLETION_METHOD, "method reduce(init: U, fn: (U, T) -> U): U", "Reduces array to a single value", has_following_paren ? "reduce" : "reduce(${1:init}, (${2:acc}, ${3:item}) -> $0)", cf, NULL, 90, 0);
    complist_add(list, "take", LSP_COMPLETION_METHOD, "method take(count: int): Array<T>", "Takes first N elements", has_following_paren ? "take" : "take(${1:count})", cf, NULL, 90, 0);
    complist_add(list, "drop", LSP_COMPLETION_METHOD, "method drop(count: int): Array<T>", "Drops first N elements", has_following_paren ? "drop" : "drop(${1:count})", cf, NULL, 90, 0);
    complist_add(list, "first", LSP_COMPLETION_METHOD, "method first(): T", "Returns the first element", has_following_paren ? "first" : "first()", 1, NULL, 90, 0);
    complist_add(list, "last", LSP_COMPLETION_METHOD, "method last(): T", "Returns the last element", has_following_paren ? "last" : "last()", 1, NULL, 90, 0);
    complist_add(list, "join", LSP_COMPLETION_METHOD, "method join(separator: string): string", "Joins elements with separator", has_following_paren ? "join" : "join(\"${1:,}\")", cf, NULL, 90, 0);
    complist_add(list, "slice", LSP_COMPLETION_METHOD, "method slice(start: int, end: int): Array<T>", "Returns sub-array slice", has_following_paren ? "slice" : "slice(${1:start}, ${2:end})", cf, NULL, 90, 0);
    complist_add(list, "clear", LSP_COMPLETION_METHOD, "method clear(): unit", "Removes all elements", has_following_paren ? "clear" : "clear()", 1, NULL, 90, 0);
    complist_add(list, "concurrentMapping", LSP_COMPLETION_METHOD, "method concurrentMapping(fn: (T) -> U): Array<U>", "Parallel map across tasks", has_following_paren ? "concurrentMapping" : "concurrentMapping(${1:item} -> $0)", cf, NULL, 85, 0);
    complist_add(list, "concurrentMappingWith", LSP_COMPLETION_METHOD, "method concurrentMappingWith(workers: int, fn: (T) -> U): Array<U>", "Parallel map with fixed workers", has_following_paren ? "concurrentMappingWith" : "concurrentMappingWith(${1:workers}, ${2:item} -> $0)", cf, NULL, 85, 0);
    complist_add(list, "concurrentForEach", LSP_COMPLETION_METHOD, "method concurrentForEach(fn: (T) -> unit): unit", "Parallel for-each", has_following_paren ? "concurrentForEach" : "concurrentForEach(${1:item} -> $0)", cf, NULL, 85, 0);
}

static void add_string_builtin_members(CompList *list, bool has_following_paren) {
    int cf = has_following_paren ? 1 : 2;

    complist_add(list, "length", LSP_COMPLETION_FIELD, "let length: int", "Number of characters in string", "length", 1, NULL, 90, 0);
    complist_add(list, "len", LSP_COMPLETION_METHOD, "method len(): int", "Number of characters in string", has_following_paren ? "len" : "len()", 1, NULL, 90, 0);
    complist_add(list, "toString", LSP_COMPLETION_METHOD, "method toString(): string", "String representation", has_following_paren ? "toString" : "toString()", 1, NULL, 90, 0);
    complist_add(list, "asString", LSP_COMPLETION_METHOD, "method asString(): string", "String representation", has_following_paren ? "asString" : "asString()", 1, NULL, 90, 0);
    complist_add(list, "charAt", LSP_COMPLETION_METHOD, "method charAt(index: int): string", "Character at index", has_following_paren ? "charAt" : "charAt(${1:index})", cf, NULL, 90, 0);
    complist_add(list, "substring", LSP_COMPLETION_METHOD, "method substring(start: int, end: int): string", "Sub-string extraction", has_following_paren ? "substring" : "substring(${1:start}, ${2:end})", cf, NULL, 90, 0);
    complist_add(list, "trim", LSP_COMPLETION_METHOD, "method trim(): string", "Removes leading and trailing whitespace", has_following_paren ? "trim" : "trim()", 1, NULL, 90, 0);
    complist_add(list, "trimStart", LSP_COMPLETION_METHOD, "method trimStart(): string", "Removes leading whitespace", has_following_paren ? "trimStart" : "trimStart()", 1, NULL, 90, 0);
    complist_add(list, "trimEnd", LSP_COMPLETION_METHOD, "method trimEnd(): string", "Removes trailing whitespace", has_following_paren ? "trimEnd" : "trimEnd()", 1, NULL, 90, 0);
    complist_add(list, "startsWith", LSP_COMPLETION_METHOD, "method startsWith(prefix: string): bool", "Checks if string starts with prefix", has_following_paren ? "startsWith" : "startsWith(\"${1:prefix}\")", cf, NULL, 90, 0);
    complist_add(list, "endsWith", LSP_COMPLETION_METHOD, "method endsWith(suffix: string): bool", "Checks if string ends with suffix", has_following_paren ? "endsWith" : "endsWith(\"${1:suffix}\")", cf, NULL, 90, 0);
    complist_add(list, "contains", LSP_COMPLETION_METHOD, "method contains(sub: string): bool", "Checks if string contains substring", has_following_paren ? "contains" : "contains(\"${1:sub}\")", cf, NULL, 90, 0);
    complist_add(list, "indexOf", LSP_COMPLETION_METHOD, "method indexOf(sub: string): int", "Finds index of substring", has_following_paren ? "indexOf" : "indexOf(\"${1:sub}\")", cf, NULL, 90, 0);
    complist_add(list, "lastIndexOf", LSP_COMPLETION_METHOD, "method lastIndexOf(sub: string): int", "Finds last index of substring", has_following_paren ? "lastIndexOf" : "lastIndexOf(\"${1:sub}\")", cf, NULL, 90, 0);
    complist_add(list, "split", LSP_COMPLETION_METHOD, "method split(separator: string): Array<string>", "Splits string into array", has_following_paren ? "split" : "split(\"${1:,}\")", cf, NULL, 90, 0);
    complist_add(list, "replace", LSP_COMPLETION_METHOD, "method replace(target: string, replacement: string): string", "Replaces occurrences", has_following_paren ? "replace" : "replace(\"${1:target}\", \"${2:replacement}\")", cf, NULL, 90, 0);
    complist_add(list, "isEmpty", LSP_COMPLETION_METHOD, "method isEmpty(): bool", "Checks if string is empty", has_following_paren ? "isEmpty" : "isEmpty()", 1, NULL, 90, 0);
}

static void add_option_builtin_members(CompList *list, bool has_following_paren) {
    int cf = has_following_paren ? 1 : 2;
    complist_add(list, "isSome", LSP_COMPLETION_METHOD, "method isSome(): bool", "Returns true if Option contains a value", has_following_paren ? "isSome" : "isSome()", 1, NULL, 90, 0);
    complist_add(list, "isNone", LSP_COMPLETION_METHOD, "method isNone(): bool", "Returns true if Option is None", has_following_paren ? "isNone" : "isNone()", 1, NULL, 90, 0);
    complist_add(list, "unwrap", LSP_COMPLETION_METHOD, "method unwrap(): T", "Unwraps the contained value or throws", has_following_paren ? "unwrap" : "unwrap()", 1, NULL, 90, 0);
    complist_add(list, "unwrapOr", LSP_COMPLETION_METHOD, "method unwrapOr(default: T): T", "Unwraps value or returns default value", has_following_paren ? "unwrapOr" : "unwrapOr(${1:default})", cf, NULL, 90, 0);
    complist_add(list, "map", LSP_COMPLETION_METHOD, "method map(fn: (T) -> U): Option<U>", "Transforms contained value with function", has_following_paren ? "map" : "map(${1:item} -> $0)", cf, NULL, 90, 0);
    complist_add(list, "andThen", LSP_COMPLETION_METHOD, "method andThen(fn: (T) -> Option<U>): Option<U>", "Chains Option-returning operations", has_following_paren ? "andThen" : "andThen(${1:item} -> $0)", cf, NULL, 90, 0);
}

static void add_result_builtin_members(CompList *list, bool has_following_paren) {
    int cf = has_following_paren ? 1 : 2;
    complist_add(list, "isOk", LSP_COMPLETION_METHOD, "method isOk(): bool", "Returns true if Result is Ok", has_following_paren ? "isOk" : "isOk()", 1, NULL, 90, 0);
    complist_add(list, "isErr", LSP_COMPLETION_METHOD, "method isErr(): bool", "Returns true if Result is Err", has_following_paren ? "isErr" : "isErr()", 1, NULL, 90, 0);
    complist_add(list, "unwrap", LSP_COMPLETION_METHOD, "method unwrap(): T", "Unwraps Ok value or throws", has_following_paren ? "unwrap" : "unwrap()", 1, NULL, 90, 0);
    complist_add(list, "unwrapErr", LSP_COMPLETION_METHOD, "method unwrapErr(): E", "Unwraps Err error value or throws", has_following_paren ? "unwrapErr" : "unwrapErr()", 1, NULL, 90, 0);
    complist_add(list, "map", LSP_COMPLETION_METHOD, "method map(fn: (T) -> U): Result<U, E>", "Transforms Ok value with function", has_following_paren ? "map" : "map(${1:item} -> $0)", cf, NULL, 90, 0);
}

static void add_map_builtin_members(CompList *list, bool has_following_paren) {
    int cf = has_following_paren ? 1 : 2;
    complist_add(list, "get", LSP_COMPLETION_METHOD, "method get(key: K): Option<V>", "Gets value by key", has_following_paren ? "get" : "get(${1:key})", cf, NULL, 90, 0);
    complist_add(list, "set", LSP_COMPLETION_METHOD, "method set(key: K, val: V): unit", "Sets value for key", has_following_paren ? "set" : "set(${1:key}, ${2:val})", cf, NULL, 90, 0);
    complist_add(list, "has", LSP_COMPLETION_METHOD, "method has(key: K): bool", "Checks if key exists in map", has_following_paren ? "has" : "has(${1:key})", cf, NULL, 90, 0);
    complist_add(list, "remove", LSP_COMPLETION_METHOD, "method remove(key: K): Option<V>", "Removes key from map", has_following_paren ? "remove" : "remove(${1:key})", cf, NULL, 90, 0);
    complist_add(list, "len", LSP_COMPLETION_METHOD, "method len(): int", "Number of entries in map", has_following_paren ? "len" : "len()", 1, NULL, 90, 0);
    complist_add(list, "clear", LSP_COMPLETION_METHOD, "method clear(): unit", "Removes all entries", has_following_paren ? "clear" : "clear()", 1, NULL, 90, 0);
    complist_add(list, "keys", LSP_COMPLETION_METHOD, "method keys(): Array<K>", "Returns array of all keys", has_following_paren ? "keys" : "keys()", 1, NULL, 90, 0);
    complist_add(list, "values", LSP_COMPLETION_METHOD, "method values(): Array<V>", "Returns array of all values", has_following_paren ? "values" : "values()", 1, NULL, 90, 0);
}

static void add_list_builtin_members(CompList *list, bool has_following_paren) {
    int cf = has_following_paren ? 1 : 2;
    complist_add(list, "add", LSP_COMPLETION_METHOD, "method add(item: T): unit", "Appends item to list", has_following_paren ? "add" : "add(${1:item})", cf, NULL, 90, 0);
    complist_add(list, "get", LSP_COMPLETION_METHOD, "method get(index: int): T", "Gets item at index", has_following_paren ? "get" : "get(${1:index})", cf, NULL, 90, 0);
    complist_add(list, "set", LSP_COMPLETION_METHOD, "method set(index: int, item: T): unit", "Sets item at index", has_following_paren ? "set" : "set(${1:index}, ${2:item})", cf, NULL, 90, 0);
    complist_add(list, "len", LSP_COMPLETION_METHOD, "method len(): int", "Number of items in list", has_following_paren ? "len" : "len()", 1, NULL, 90, 0);
    complist_add(list, "length", LSP_COMPLETION_FIELD, "let length: int", "Number of items in list", "length", 1, NULL, 90, 0);
    complist_add(list, "clear", LSP_COMPLETION_METHOD, "method clear(): unit", "Removes all items", has_following_paren ? "clear" : "clear()", 1, NULL, 90, 0);
    complist_add(list, "contains", LSP_COMPLETION_METHOD, "method contains(item: T): bool", "Checks if item exists in list", has_following_paren ? "contains" : "contains(${1:item})", cf, NULL, 90, 0);
    complist_add(list, "map", LSP_COMPLETION_METHOD, "method map(fn: (T) -> U): List<U>", "Transforms list items", has_following_paren ? "map" : "map(${1:item} -> $0)", cf, NULL, 90, 0);
    complist_add(list, "filter", LSP_COMPLETION_METHOD, "method filter(fn: (T) -> bool): List<T>", "Filters list items", has_following_paren ? "filter" : "filter(${1:item} -> $0)", cf, NULL, 90, 0);
}

static void add_scalar_builtin_members(CompList *list, bool has_following_paren) {
    complist_add(list, "toString", LSP_COMPLETION_METHOD, "method toString(): string", "String representation", has_following_paren ? "toString" : "toString()", 1, NULL, 90, 0);
    complist_add(list, "asString", LSP_COMPLETION_METHOD, "method asString(): string", "String representation", has_following_paren ? "asString" : "asString()", 1, NULL, 90, 0);
}

/* ── Decl & Scope Symbol Addition ─────────────────────────────────────────── */

static void add_decl_member_symbols(CompList *list, const SnDecl *type_decl, bool has_following_paren) {
    if (!type_decl) return;

    for (size_t i = 0; i < type_decl->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(type_decl->members, const SnDecl, i);
        if (!m || !m->name) continue;

        LspCompletionKind kind = LSP_COMPLETION_FIELD;
        char detail[256] = "";
        char insert_text[256];
        int insert_fmt = 1;

        if (m->kind == SN_DECL_METHOD) {
            kind = LSP_COMPLETION_METHOD;
            snprintf(detail, sizeof(detail), "method %s", m->name);
            if (!has_following_paren) {
                if (m->params.len > 0) {
                    snprintf(insert_text, sizeof(insert_text), "%s($1)", m->name);
                    insert_fmt = 2;
                } else {
                    snprintf(insert_text, sizeof(insert_text), "%s()", m->name);
                }
            } else {
                snprintf(insert_text, sizeof(insert_text), "%s", m->name);
            }
        } else if (m->kind == SN_DECL_FIELD) {
            kind = LSP_COMPLETION_FIELD;
            snprintf(detail, sizeof(detail), "%s %s", m->is_mutable ? "var" : "let", m->name);
            snprintf(insert_text, sizeof(insert_text), "%s", m->name);
        } else if (m->kind == SN_DECL_CONST) {
            kind = LSP_COMPLETION_CONSTANT;
            snprintf(detail, sizeof(detail), "const %s", m->name);
            snprintf(insert_text, sizeof(insert_text), "%s", m->name);
        } else {
            snprintf(detail, sizeof(detail), "member %s", m->name);
            snprintf(insert_text, sizeof(insert_text), "%s", m->name);
        }

        complist_add(list, m->name, kind, detail, NULL, insert_text, insert_fmt, NULL, 90, 0);
    }
}

static void add_scope_symbols(CompList *list, SnScope *scope, ComplContext ctx, int base_score, bool has_following_paren) {
    if (!scope || !scope->buckets) return;

    for (size_t b = 0; b < scope->nbuckets; b++) {
        for (SnSymbol *sym = scope->buckets[b]; sym; sym = sym->next) {
            if (!sym->name || !sym->name[0]) continue;

            LspCompletionKind kind = LSP_COMPLETION_VARIABLE;
            char detail[256] = "";
            int type_bonus = 0;
            int score = base_score;

            switch (sym->kind) {
                case SN_SYM_TYPE:
                    kind = LSP_COMPLETION_CLASS;
                    snprintf(detail, sizeof(detail), "type %s", sym->name);
                    type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
                    if (ctx == CTX_TYPE_POS) score = 95;
                    break;
                case SN_SYM_FUNC:
                    kind = LSP_COMPLETION_FUNCTION;
                    snprintf(detail, sizeof(detail), "func %s", sym->name);
                    break;
                case SN_SYM_METHOD:
                    kind = LSP_COMPLETION_METHOD;
                    snprintf(detail, sizeof(detail), "method %s", sym->name);
                    break;
                case SN_SYM_FIELD:
                    kind = LSP_COMPLETION_FIELD;
                    snprintf(detail, sizeof(detail), "field %s", sym->name);
                    break;
                case SN_SYM_CONST:
                    kind = LSP_COMPLETION_CONSTANT;
                    snprintf(detail, sizeof(detail), "const %s", sym->name);
                    break;
                case SN_SYM_VARIANT:
                    kind = LSP_COMPLETION_ENUM_MEMBER;
                    snprintf(detail, sizeof(detail), "variant %s", sym->name);
                    break;
                case SN_SYM_PARAM:
                    kind = LSP_COMPLETION_VARIABLE;
                    if (sym->value_type) {
                        char ty_s[128];
                        format_typerep_string(sym->value_type, ty_s, sizeof(ty_s));
                        snprintf(detail, sizeof(detail), "param %s: %s", sym->name, ty_s);
                    } else {
                        snprintf(detail, sizeof(detail), "param %s", sym->name);
                    }
                    score = 95;
                    break;
                case SN_SYM_LOCAL:
                    kind = LSP_COMPLETION_VARIABLE;
                    if (sym->value_type) {
                        char ty_s[128];
                        format_typerep_string(sym->value_type, ty_s, sizeof(ty_s));
                        snprintf(detail, sizeof(detail), "%s %s: %s", sym->is_mutable ? "var" : "let", sym->name, ty_s);
                    } else {
                        snprintf(detail, sizeof(detail), "%s %s", sym->is_mutable ? "var" : "let", sym->name);
                    }
                    score = 90;
                    break;
                case SN_SYM_PACKAGE:
                    kind = LSP_COMPLETION_MODULE;
                    snprintf(detail, sizeof(detail), "package %s", sym->name);
                    break;
                default:
                    snprintf(detail, sizeof(detail), "symbol %s", sym->name);
                    break;
            }

            if (ctx == CTX_TYPE_POS && sym->kind != SN_SYM_TYPE) {
                continue;
            }

            char insert_text[256];
            int insert_fmt = 1;
            if ((sym->kind == SN_SYM_FUNC || sym->kind == SN_SYM_METHOD) && !has_following_paren) {
                snprintf(insert_text, sizeof(insert_text), "%s($1)", sym->name);
                insert_fmt = 2;
            } else {
                snprintf(insert_text, sizeof(insert_text), "%s", sym->name);
            }

            complist_add(list, sym->name, kind, detail, NULL, insert_text, insert_fmt, NULL, score, type_bonus);
        }
    }
}

static SnScope *completion_import_scope(const LspDocAnalysis *a, const char *imp) {
    if (!a || !imp) return NULL;
    SnScope *scope = sn_resolver_package_scope(&a->resolver, imp);
    if (scope) return scope;
    char prefix[SNOVAC_PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s", imp);
    for (char *p = strrchr(prefix, '.'); p; p = strrchr(prefix, '.')) {
        *p = '\0';
        scope = sn_resolver_package_scope(&a->resolver, prefix);
        if (scope) return scope;
    }
    return NULL;
}

static bool import_covers_package(const LspDocAnalysis *a, const char *package_name) {
    if (!a || !package_name || !package_name[0]) return false;
    size_t package_len = strlen(package_name);
    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        if (!imp) continue;
        if (strcmp(imp, package_name) == 0 ||
            (strncmp(imp, package_name, package_len) == 0 &&
             imp[package_len] == '.')) {
            return true;
        }
    }
    return false;
}

/* Return the end of the package declaration line. Imports are inserted at
 * this point so the existing blank line and import formatting remain intact. */
static uint32_t package_line_end(const LspDocument *doc, const char *package_name,
                                 const char **newline) {
    if (!doc || !package_name) return UINT32_MAX;
    const char *text = doc->text;
    size_t offset = 0;
    while (offset <= doc->text_len) {
        size_t line_start = offset;
        while (offset < doc->text_len && text[offset] != '\n' && text[offset] != '\r') offset++;
        size_t line_end = offset;
        while (line_start < line_end && (text[line_start] == ' ' || text[line_start] == '\t')) line_start++;
        size_t package_len = strlen(package_name);
        if (line_end - line_start >= 8 + package_len &&
            strncmp(text + line_start, "package", 7) == 0 &&
            (text[line_start + 7] == ' ' || text[line_start + 7] == '\t') &&
            strncmp(text + line_start + 8, package_name, package_len) == 0) {
            size_t after_name = line_start + 8 + package_len;
            if (after_name == line_end || text[after_name] == ' ' || text[after_name] == '\t' ||
                text[after_name] == ';') {
                if (newline) *newline = "\n";
                if (offset < doc->text_len && text[offset] == '\r') {
                    if (newline) *newline = "\r\n";
                }
                return (uint32_t)offset;
            }
        }
        if (offset >= doc->text_len) break;
        if (text[offset] == '\r' && offset + 1 < doc->text_len && text[offset + 1] == '\n') offset++;
        offset++;
    }
    return UINT32_MAX;
}

/* ── Member Completion on Inferred Type ───────────────────────────────────── */

static void add_members_for_typerep(CompList *list, const LspDocAnalysis *a, const SnTypeRep *ty, bool has_following_paren) {
    if (!ty) return;

    if (ty->tag == SN_T_ARRAY) {
        add_array_builtin_members(list, has_following_paren);
        return;
    }
    if (ty->tag == SN_T_STRING) {
        add_string_builtin_members(list, has_following_paren);
        return;
    }
    if (ty->tag == SN_T_INT || ty->tag == SN_T_LONG || ty->tag == SN_T_DOUBLE ||
        ty->tag == SN_T_DECIMAL || ty->tag == SN_T_FLOAT || ty->tag == SN_T_BYTE || ty->tag == SN_T_BOOL) {
        add_scalar_builtin_members(list, has_following_paren);
        return;
    }

    if (ty->tag == SN_T_NAMED && ty->decl) {
        const char *dname = ty->decl->name ? ty->decl->name : "";
        if (strcmp(dname, "Option") == 0) {
            add_option_builtin_members(list, has_following_paren);
        } else if (strcmp(dname, "Result") == 0) {
            add_result_builtin_members(list, has_following_paren);
        } else if (strcmp(dname, "Map") == 0) {
            add_map_builtin_members(list, has_following_paren);
        } else if (strcmp(dname, "List") == 0) {
            add_list_builtin_members(list, has_following_paren);
        }

        const SnDecl *decl = ty->decl->decl;
        if (decl) {
            add_decl_member_symbols(list, decl, has_following_paren);
        }

        // Look up member scope in resolver
        for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
            if (te->type_decl && te->type_decl->name && ty->decl->name &&
                strcmp(te->type_decl->name, ty->decl->name) == 0) {
                add_scope_symbols(list, te->member_scope, CTX_MEMBER, 90, has_following_paren);
            }
        }
    }
}

/* ── Main Completion Handler ─────────────────────────────────────────────── */

char *lsp_completion_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspPosition pos) {
    if (!doc) return NULL;

    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, store, doc);
    }

    char line_buf[4096] = {0};
    char prefix_buf[256] = {0};
    char receiver_buf[256] = {0};
    ComplContext ctx = CTX_GENERAL;
    bool has_following_paren = false;

    extract_line_info(doc->text, pos.line, pos.character,
                      line_buf, sizeof(line_buf),
                      prefix_buf, sizeof(prefix_buf),
                      receiver_buf, sizeof(receiver_buf),
                      &ctx, &has_following_paren);

    CompList list;
    complist_init(&list);

    uint32_t cursor_offset = lsp_pos_to_offset(doc, pos);

    SnDiagSink null_diag;
    SnChecker checker;
    if (a) {
        sn_diag_init(&null_diag, a->path ? a->path : "", "", 0);
        null_diag.out = NULL;
        null_diag.quiet = 1;
        sn_checker_init(&checker, (SnArena *)&a->arena, (SnInternTable *)&a->intern,
                        &null_diag, (SnResolver *)&a->resolver, (SnTypeTable *)&a->types);
    }
    const SnDecl *enclosing_decl = NULL;
    const SnDecl *enclosing_type = NULL;
    SnScope *local_scope = a ? lsp_build_scope_at(a, &checker, cursor_offset, &enclosing_decl, &enclosing_type) : NULL;
    if (a) {
        checker.current_package = (a->unit.package && a->unit.package[0])
            ? sn_intern_cstr((SnInternTable *)&a->intern, a->unit.package)
            : sn_intern_cstr((SnInternTable *)&a->intern, "main");
        checker.current_imports = &a->unit.imports;
        checker.enclosing_type = enclosing_type;
    }

    if (ctx == CTX_MEMBER) {
        // Semantic Member Completion
        bool found_receiver = false;

        if (a && receiver_buf[0]) {
            // 1. Check if receiver is `this`
            if (strcmp(receiver_buf, "this") == 0 && enclosing_type) {
                add_decl_member_symbols(&list, enclosing_type, has_following_paren);
                found_receiver = true;
            } else {
                // 2. Infer receiver expression type semantically
                SnTypeRep *recv_ty = lsp_infer_expr_type_at(a, &checker, local_scope, receiver_buf);
                if (recv_ty && recv_ty->tag != SN_T_ERROR) {
                    // Check if receiver is Option<T> and unwrapped access
                    if (recv_ty->tag == SN_T_NAMED && recv_ty->decl &&
                        strcmp(recv_ty->decl->name, "Option") == 0 &&
                        recv_ty->nargs >= 1 && recv_ty->args && recv_ty->args[0]) {
                        add_members_for_typerep(&list, a, recv_ty->args[0], has_following_paren);
                    }
                    add_members_for_typerep(&list, a, recv_ty, has_following_paren);
                    found_receiver = true;
                }
            }

            // 3. If receiver is a static type name (e.g. `User.` or `Option.`)
            if (!found_receiver) {
                for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
                    if (te->type_decl && te->type_decl->name && strcmp(te->type_decl->name, receiver_buf) == 0) {
                        add_scope_symbols(&list, te->member_scope, CTX_MEMBER, 90, has_following_paren);
                        found_receiver = true;
                    }
                }
            }

            // 4. If receiver is a package name (e.g. `math.` or `io.`)
            if (!found_receiver) {
                SnScope *pkg_scope = sn_resolver_package_scope(&a->resolver, receiver_buf);
                if (pkg_scope) {
                    add_scope_symbols(&list, pkg_scope, CTX_MEMBER, 85, has_following_paren);
                    found_receiver = true;
                }
            }
        }
    } else {
        // Non-member (General, Type, Decorator, Import) Completion

        // 1. Semantic Local Variables & Parameters (highest priority 95 / 90)
        if (local_scope && ctx != CTX_DECORATOR && ctx != CTX_IMPORT_LINE) {
            add_scope_symbols(&list, local_scope, ctx, 90, has_following_paren);
        }

        // 2. Enclosing type members (if inside a class method)
        if (enclosing_type && ctx == CTX_GENERAL) {
            add_decl_member_symbols(&list, enclosing_type, has_following_paren);
        }

        // 3. Current AST declarations in active unit
        if (a && a->has_ast) {
            for (size_t i = 0; i < a->unit.decls.len; i++) {
                const SnDecl *d = SN_LIST_AT(a->unit.decls, const SnDecl, i);
                if (!d || !d->name) continue;
                LspCompletionKind kind = (d->kind == SN_DECL_CLASS) ? LSP_COMPLETION_CLASS :
                                         (d->kind == SN_DECL_STRUCT) ? LSP_COMPLETION_STRUCT :
                                         (d->kind == SN_DECL_ENUM) ? LSP_COMPLETION_ENUM :
                                         (d->kind == SN_DECL_INTERFACE) ? LSP_COMPLETION_INTERFACE :
                                         (d->kind == SN_DECL_FUNC) ? LSP_COMPLETION_FUNCTION :
                                         (d->kind == SN_DECL_CONST) ? LSP_COMPLETION_CONSTANT : LSP_COMPLETION_VARIABLE;
                if (ctx == CTX_TYPE_POS && kind != LSP_COMPLETION_CLASS && kind != LSP_COMPLETION_STRUCT &&
                    kind != LSP_COMPLETION_ENUM && kind != LSP_COMPLETION_INTERFACE) {
                    continue;
                }

                char detail[256];
                snprintf(detail, sizeof(detail), "decl %s", d->name);
                char insert_text[256];
                int insert_fmt = 1;
                if (kind == LSP_COMPLETION_FUNCTION && !has_following_paren) {
                    snprintf(insert_text, sizeof(insert_text), "%s($1)", d->name);
                    insert_fmt = 2;
                } else {
                    snprintf(insert_text, sizeof(insert_text), "%s", d->name);
                }
                int score = (ctx == CTX_TYPE_POS) ? 95 : 80;
                int type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
                complist_add(&list, d->name, kind, detail, NULL, insert_text, insert_fmt, NULL, score, type_bonus);
            }
        }

        // 4. Current package & imported symbols from resolver
        if (a && a->has_resolved) {
            const char *pkg_name = a->unit.package ? a->unit.package : "";
            SnScope *pkg_scope = sn_resolver_package_scope(&a->resolver, pkg_name);
            if (pkg_scope) {
                add_scope_symbols(&list, pkg_scope, ctx, 80, has_following_paren);
            }

            for (size_t i = 0; i < a->unit.imports.len; i++) {
                const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
                SnScope *imp_scope = completion_import_scope(a, imp);
                if (imp_scope) {
                    add_scope_symbols(&list, imp_scope, ctx, 75, has_following_paren);
                }
            }

            if (a->resolver.prelude_scope) {
                add_scope_symbols(&list, a->resolver.prelude_scope, ctx, 70, has_following_paren);
            }

            // Cross-package symbols from all dependency / workspace packages
            for (SnPackageScopeEntry *pe = a->resolver.packages; pe; pe = pe->next) {
                if (!pe->package_name || !pe->scope) continue;
                if (pkg_name && strcmp(pe->package_name, pkg_name) == 0) continue;

                bool in_imports = import_covers_package(a, pe->package_name);
                if (in_imports) continue;

                for (size_t b = 0; b < pe->scope->nbuckets; b++) {
                    for (SnSymbol *sym = pe->scope->buckets[b]; sym; sym = sym->next) {
                        if (!sym->name || !sym->name[0]) continue;
                        if (sym->decl && sym->decl->vis != SN_VIS_PUBLIC && sym->decl->vis != SN_VIS_DEFAULT) continue;

                        LspCompletionKind kind = LSP_COMPLETION_VARIABLE;
                        const char *kind_str = "symbol";
                        int type_bonus = 0;

                        switch (sym->kind) {
                            case SN_SYM_TYPE:
                                kind = LSP_COMPLETION_CLASS;
                                kind_str = "type";
                                type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
                                break;
                            case SN_SYM_FUNC:
                                kind = LSP_COMPLETION_FUNCTION;
                                kind_str = "func";
                                break;
                            case SN_SYM_METHOD:
                                kind = LSP_COMPLETION_METHOD;
                                kind_str = "method";
                                break;
                            case SN_SYM_CONST:
                                kind = LSP_COMPLETION_CONSTANT;
                                kind_str = "const";
                                break;
                            case SN_SYM_VARIANT:
                                kind = LSP_COMPLETION_ENUM_MEMBER;
                                kind_str = "variant";
                                break;
                            default:
                                continue;
                        }

                        if (ctx == CTX_TYPE_POS && sym->kind != SN_SYM_TYPE) continue;

                        char detail[512];
                        snprintf(detail, sizeof(detail), "%s %s (from %s)", kind_str, sym->name, pe->package_name);

                        char insert_text[256];
                        int insert_fmt = 1;
                        if ((sym->kind == SN_SYM_FUNC || sym->kind == SN_SYM_METHOD) && !has_following_paren) {
                            snprintf(insert_text, sizeof(insert_text), "%s($1)", sym->name);
                            insert_fmt = 2;
                        } else {
                            snprintf(insert_text, sizeof(insert_text), "%s", sym->name);
                        }

                        int score = (ctx == CTX_TYPE_POS) ? 90 : 65;
                        const char *needed_import = (ctx == CTX_IMPORT_LINE ||
                                                     import_covers_package(a, pe->package_name))
                            ? NULL : pe->package_name;
                        complist_add(&list, sym->name, kind, detail, pe->package_name,
                                     insert_text, insert_fmt, needed_import, score, type_bonus);
                    }
                }
            }

            // Package modules for imports
            for (SnPackageNode *pn = a->graph.nodes; pn; pn = pn->next) {
                if (pn->name && pn->name[0]) {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "package %s", pn->name);
                    int base_pkg_score = (ctx == CTX_IMPORT_LINE) ? 90 : 60;
                    complist_add(&list, pn->name, LSP_COMPLETION_MODULE, detail, "Snovalang package dependency", pn->name, 1, NULL, base_pkg_score, 0);
                }
            }
        }

        // Keywords, snippets, types, decorators
        add_keywords_and_snippets(&list, ctx, has_following_paren);
    }

    // Rank and Sort candidates
    rank_and_sort_candidates(&list, prefix_buf);

    // Build JSON-RPC response
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    jb_kv_bool(&jb, "isIncomplete", false);
    jb_key(&jb, "items");
    jb_start_arr(&jb);

    uint32_t start_col = pos.character >= (uint32_t)strlen(prefix_buf) ? pos.character - (uint32_t)strlen(prefix_buf) : 0;
    uint32_t end_col = pos.character;
    const char *package_name = (a && a->unit.package && a->unit.package[0]) ? a->unit.package : NULL;
    const char *import_newline = "\n";
    uint32_t package_end = package_line_end(doc, package_name, &import_newline);
    LspPosition import_pos = {0, 0};
    if (package_end != UINT32_MAX) import_pos = lsp_offset_to_pos(doc, package_end);

    for (size_t i = 0; i < list.len; i++) {
        const CompItem *ci = &list.items[i];
        jb_start_obj(&jb);
        jb_kv_str(&jb, "label", ci->label);
        jb_kv_int(&jb, "kind", (int)ci->kind);
        if (ci->detail) jb_kv_str(&jb, "detail", ci->detail);
        if (ci->doc) {
            jb_key(&jb, "documentation");
            jb_start_obj(&jb);
            jb_kv_str(&jb, "kind", "markdown");
            jb_kv_str(&jb, "value", ci->doc);
            jb_end_obj(&jb);
        }

        char sort_text[32];
        snprintf(sort_text, sizeof(sort_text), "%05zu", i);
        jb_kv_str(&jb, "sortText", sort_text);
        jb_kv_str(&jb, "filterText", ci->filter_text ? ci->filter_text : ci->label);
        if (i == 0) {
            jb_kv_bool(&jb, "preselect", true);
        }

        jb_kv_str(&jb, "insertText", ci->insert_text ? ci->insert_text : ci->label);
        jb_kv_int(&jb, "insertTextFormat", ci->insert_text_format);

        // TextEdit
        jb_key(&jb, "textEdit");
        jb_start_obj(&jb);
        jb_key(&jb, "range");
        jb_start_obj(&jb);
        jb_key(&jb, "start");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", pos.line);
        jb_kv_int(&jb, "character", start_col);
        jb_end_obj(&jb);
        jb_key(&jb, "end");
        jb_start_obj(&jb);
        jb_kv_int(&jb, "line", pos.line);
        jb_kv_int(&jb, "character", end_col);
        jb_end_obj(&jb);
        jb_end_obj(&jb);
        jb_kv_str(&jb, "newText", ci->insert_text ? ci->insert_text : ci->label);
        jb_end_obj(&jb);

        if (ci->additional_import && package_end != UINT32_MAX) {
            jb_key(&jb, "additionalTextEdits");
            jb_start_arr(&jb);
            jb_start_obj(&jb);
            jb_key(&jb, "range");
            jb_start_obj(&jb);
            jb_key(&jb, "start");
            jb_start_obj(&jb);
            jb_kv_int(&jb, "line", import_pos.line);
            jb_kv_int(&jb, "character", import_pos.character);
            jb_end_obj(&jb);
            jb_key(&jb, "end");
            jb_start_obj(&jb);
            jb_kv_int(&jb, "line", import_pos.line);
            jb_kv_int(&jb, "character", import_pos.character);
            jb_end_obj(&jb);
            jb_end_obj(&jb);

            char import_text[SNOVAC_PATH_MAX + 32];
            snprintf(import_text, sizeof(import_text), "%simport %s%s",
                     import_newline, ci->additional_import, import_newline);
            jb_kv_str(&jb, "newText", import_text);
            jb_end_obj(&jb);
            jb_end_arr(&jb);
        }

        jb_end_obj(&jb);
    }

    jb_end_arr(&jb);
    jb_end_obj(&jb);

    complist_free(&list);
    return jb_take(&jb);
}
