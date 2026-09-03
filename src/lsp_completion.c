#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_completion.h"
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

/* Check if acronym matches camelCase or snake_case initials (e.g. "ghr" -> "getHttpRequest") */
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

/* Subsequence match: all prefix chars appear in label in order (case-insensitive) */
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

/* Match scoring tiers based on gopls / rust-analyzer */
static int compute_match_bonus(const char *label, const char *prefix) {
    if (!prefix || !prefix[0]) return 0; /* Empty prefix matches everything with base bonus 0 */
    if (!label) return -1;

    size_t plen = strlen(prefix);

    /* Tier 1: Exact case-sensitive match */
    if (strcmp(label, prefix) == 0) return 120;

    /* Tier 2: Exact case-insensitive match */
    if (strcasecmp(label, prefix) == 0) return 100;

    /* Tier 3: Prefix case-sensitive match */
    if (strncmp(label, prefix, plen) == 0) return 80;

    /* Tier 4: Prefix case-insensitive match */
    if (strncasecmp(label, prefix, plen) == 0) return 60;

    /* Tier 5: Acronym match (camelCase humps) */
    if (acronym_matches(label, prefix)) return 35;

    /* Tier 6: Subsequence fuzzy match */
    if (subsequence_matches(label, prefix)) return 20;

    /* Tier 7: Substring match */
    if (strcasestr(label, prefix) != NULL) return 15;

    /* No match */
    return -1;
}

/* Insertion sort by finalScore descending, with length and alphabetical tie-breaking */
static void rank_and_sort_candidates(CompList *list, const char *prefix) {
    if (!list || list->len == 0) return;

    /* 1. Calculate scores and filter out non-matching candidates */
    for (size_t i = 0; i < list->len; i++) {
        CompItem *ci = &list->items[i];
        int mb = compute_match_bonus(ci->label, prefix);
        if (prefix && prefix[0] && mb < 0) {
            /* Mark as discarded */
            ci->final_score = -1;
            continue;
        }
        ci->match_bonus = (mb >= 0) ? mb : 0;
        /* Gopls three-tier formula: finalScore = baseScore + (typeBonus * 3) + matchBonus */
        ci->final_score = ci->base_score + (ci->type_bonus * 3) + ci->match_bonus;
    }

    /* 2. Compact valid items */
    size_t dst = 0;
    for (size_t src = 0; src < list->len; src++) {
        if (list->items[src].final_score >= 0) {
            if (dst != src) {
                list->items[dst] = list->items[src];
            }
            dst++;
        } else {
            /* Free discarded item */
            if (list->items[src].label) free(list->items[src].label);
            if (list->items[src].detail) free(list->items[src].detail);
            if (list->items[src].doc) free(list->items[src].doc);
            if (list->items[src].insert_text) free(list->items[src].insert_text);
            if (list->items[src].filter_text) free(list->items[src].filter_text);
            if (list->items[src].additional_import) free(list->items[src].additional_import);
        }
    }
    list->len = dst;

    /* 3. Insertion sort */
    for (size_t i = 1; i < list->len; i++) {
        CompItem key = list->items[i];
        size_t j = i;
        while (j > 0) {
            CompItem *prev = &list->items[j - 1];
            bool swap = false;
            if (key.final_score > prev->final_score) {
                swap = true;
            } else if (key.final_score == prev->final_score) {
                /* Tie breaker: shorter label first, then alphabetical */
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
    CTX_IMPORT_LINE,
    CTX_ARGUMENT,
    CTX_STRUCT_LIT,
    CTX_MATCH_ARM
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

    // Find target line in document
    const char *p = doc_text;
    uint32_t cur_line = 0;
    while (*p && cur_line < target_line) {
        if (*p == '\n') cur_line++;
        p++;
    }

    // Copy current line
    size_t li = 0;
    while (*p && *p != '\r' && *p != '\n' && li + 1 < line_buf_sz) {
        line_buf[li++] = *p++;
    }
    line_buf[li] = '\0';

    size_t col = target_col;
    if (col > li) col = li;

    // Check following paren
    size_t fp = col;
    while (fp < li && (line_buf[fp] == ' ' || line_buf[fp] == '\t')) fp++;
    if (fp < li && line_buf[fp] == '(') {
        *out_has_following_paren = true;
    }

    // Extract prefix immediately before cursor
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

    // Check preceding characters
    size_t prec = start;
    while (prec > 0 && (line_buf[prec - 1] == ' ' || line_buf[prec - 1] == '\t')) prec--;

    if (prec > 0 && line_buf[prec - 1] == '.') {
        *out_ctx = CTX_MEMBER;
        // Extract receiver before dot
        size_t r_end = prec - 1;
        while (r_end > 0 && (line_buf[r_end - 1] == ' ' || line_buf[r_end - 1] == '\t')) r_end--;
        size_t r_start = r_end;
        while (r_start > 0) {
            char c = line_buf[r_start - 1];
            if (isalnum((unsigned char)c) || c == '_' || c == ')' || c == ']') {
                r_start--;
            } else {
                break;
            }
        }
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

    // Check if line starts with import / using
    const char *trimmed = line_buf;
    while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
    if (strncmp(trimmed, "import ", 7) == 0 || strncmp(trimmed, "using ", 6) == 0) {
        *out_ctx = CTX_IMPORT_LINE;
        return;
    }

    // Check if after ':' or 'as' or 'is' or '<'
    if (prec > 0 && line_buf[prec - 1] == ':') {
        *out_ctx = CTX_TYPE_POS;
        return;
    }
    if (prec > 0 && line_buf[prec - 1] == '<') {
        *out_ctx = CTX_TYPE_POS;
        return;
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
        return; // Only types in type position
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

    // Keywords (Base score 40 so locals and members rank higher)
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

/* ── Scope Analysis: Local Variables & Parameters ─────────────────────────── */

static void collect_scope_locals(CompList *list, const char *doc_text, uint32_t cursor_line, bool has_following_paren) {
    (void)has_following_paren;
    if (!doc_text) return;

    // Scan lines up to cursor_line for parameters of enclosing routine
    char last_params[512] = {0};
    const char *p = doc_text;
    uint32_t cl = 0;
    while (*p && cl <= cursor_line) {
        char line[4096] = {0};
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n + 1 < sizeof(line)) { line[n] = p[n]; n++; }
        line[n] = '\0';
        p += n;
        if (*p == '\n') p++;

        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        if (strncmp(trimmed, "func ", 5) == 0 || strncmp(trimmed, "method ", 7) == 0 ||
            strncmp(trimmed, "async func ", 11) == 0 || strncmp(trimmed, "async method ", 13) == 0 ||
            strncmp(trimmed, "constructor(", 12) == 0 || strncmp(trimmed, "public constructor(", 19) == 0) {
            const char *op = strchr(trimmed, '(');
            const char *cp = op ? strrchr(trimmed, ')') : NULL;
            if (op && cp && cp > op) {
                size_t len = (size_t)(cp - op - 1);
                if (len >= sizeof(last_params)) len = sizeof(last_params) - 1;
                memcpy(last_params, op + 1, len);
                last_params[len] = '\0';
            }
        }
        cl++;
    }

    if (last_params[0]) {
        char params_copy[512];
        strncpy(params_copy, last_params, sizeof(params_copy) - 1);
        params_copy[sizeof(params_copy) - 1] = '\0';
        char *token = strtok(params_copy, ",");
        while (token) {
            while (*token == ' ' || *token == '\t') token++;
            char pname[128] = {0}, ptype[128] = {0};
            size_t i = 0;
            while (token[i] && token[i] != ':' && token[i] != ' ' && token[i] != '=' && i < sizeof(pname) - 1) {
                pname[i] = token[i]; i++;
            }
            pname[i] = '\0';

            const char *colon = strchr(token, ':');
            if (colon) {
                colon++;
                while (*colon == ' ') colon++;
                size_t j = 0;
                while (colon[j] && colon[j] != ',' && colon[j] != '=' && j < sizeof(ptype) - 1) {
                    ptype[j] = colon[j]; j++;
                }
                while (j > 0 && ptype[j - 1] == ' ') ptype[--j] = '\0';
            }

            if (pname[0]) {
                char detail[512];
                if (ptype[0]) snprintf(detail, sizeof(detail), "param %s: %s", pname, ptype);
                else snprintf(detail, sizeof(detail), "param %s", pname);
                /* Highest base score: 95 for parameters */
                complist_add(list, pname, LSP_COMPLETION_VARIABLE, detail, "Function/method parameter", pname, 1, NULL, 95, 15);
            }
            token = strtok(NULL, ",");
        }
    }

    // Scan lines up to cursor_line for local let / var declarations
    p = doc_text;
    cl = 0;
    while (*p && cl <= cursor_line) {
        char line[4096] = {0};
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n + 1 < sizeof(line)) { line[n] = p[n]; n++; }
        line[n] = '\0';
        p += n;
        if (*p == '\n') p++;

        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        char vname[128] = {0};
        bool is_var = false;
        if (strncmp(trimmed, "let ", 4) == 0) {
            trimmed += 4;
        } else if (strncmp(trimmed, "var ", 4) == 0) {
            trimmed += 4;
            is_var = true;
        } else {
            cl++;
            continue;
        }

        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        size_t vi = 0;
        while (trimmed[vi] && (isalnum((unsigned char)trimmed[vi]) || trimmed[vi] == '_') && vi < sizeof(vname) - 1) {
            vname[vi] = trimmed[vi]; vi++;
        }
        vname[vi] = '\0';

        if (vname[0]) {
            char vtype[128] = {0};
            const char *colon = strchr(trimmed, ':');
            if (colon) {
                colon++;
                while (*colon == ' ') colon++;
                size_t j = 0;
                while (colon[j] && colon[j] != '=' && colon[j] != ';' && j < sizeof(vtype) - 1) {
                    vtype[j] = colon[j]; j++;
                }
                while (j > 0 && vtype[j - 1] == ' ') vtype[--j] = '\0';
            }

            char detail[512];
            const char *kw = is_var ? "var" : "let";
            if (vtype[0]) snprintf(detail, sizeof(detail), "%s %s: %s", kw, vname, vtype);
            else snprintf(detail, sizeof(detail), "%s %s", kw, vname);

            /* High base score: 90 for local variables */
            complist_add(list, vname, LSP_COMPLETION_VARIABLE, detail, "Local variable", vname, 1, NULL, 90, 10);
        }
        cl++;
    }
}

/* ── Decl & Symbol Collection ─────────────────────────────────────────────── */

static void collect_decl_symbols(CompList *list, const SnDecl *d, ComplContext ctx, bool has_following_paren) {
    if (!d || !d->name) return;

    LspCompletionKind kind = LSP_COMPLETION_VARIABLE;
    const char *kind_str = "declaration";
    int base_score = 80;
    int type_bonus = 0;

    switch (d->kind) {
        case SN_DECL_CLASS:
            kind = LSP_COMPLETION_CLASS;
            kind_str = "class";
            type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
            break;
        case SN_DECL_STRUCT:
            kind = LSP_COMPLETION_STRUCT;
            kind_str = "struct";
            type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
            break;
        case SN_DECL_INTERFACE:
            kind = LSP_COMPLETION_INTERFACE;
            kind_str = "interface";
            type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
            break;
        case SN_DECL_ENUM:
            kind = LSP_COMPLETION_ENUM;
            kind_str = "enum";
            type_bonus = (ctx == CTX_TYPE_POS) ? 30 : 0;
            break;
        case SN_DECL_FUNC:
            kind = LSP_COMPLETION_FUNCTION;
            kind_str = "func";
            break;
        case SN_DECL_METHOD:
            kind = LSP_COMPLETION_METHOD;
            kind_str = "method";
            base_score = (ctx == CTX_MEMBER) ? 85 : 80;
            break;
        case SN_DECL_FIELD:
            kind = LSP_COMPLETION_FIELD;
            kind_str = d->is_mutable ? "var field" : "let field";
            base_score = (ctx == CTX_MEMBER) ? 85 : 75;
            break;
        case SN_DECL_CONST:
            kind = LSP_COMPLETION_CONSTANT;
            kind_str = "const";
            break;
        case SN_DECL_VARIANT:
            kind = LSP_COMPLETION_ENUM_MEMBER;
            kind_str = "enum variant";
            break;
        default:
            break;
    }

    if (ctx == CTX_TYPE_POS && kind != LSP_COMPLETION_CLASS && kind != LSP_COMPLETION_STRUCT &&
        kind != LSP_COMPLETION_INTERFACE && kind != LSP_COMPLETION_ENUM) {
        return;
    }

    char detail[256];
    snprintf(detail, sizeof(detail), "%s %s", kind_str, d->name);

    char insert_text[256];
    int insert_fmt = 1;
    if ((kind == LSP_COMPLETION_FUNCTION || kind == LSP_COMPLETION_METHOD) && !has_following_paren) {
        snprintf(insert_text, sizeof(insert_text), "%s($1)", d->name);
        insert_fmt = 2;
    } else {
        snprintf(insert_text, sizeof(insert_text), "%s", d->name);
    }

    complist_add(list, d->name, kind, detail, NULL, insert_text, insert_fmt, NULL, base_score, type_bonus);

    // If member access on a type, also add its members
    if (d->members.len > 0) {
        for (size_t i = 0; i < d->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(d->members, const SnDecl, i);
            if (ctx == CTX_MEMBER || ctx == CTX_GENERAL) {
                collect_decl_symbols(list, m, ctx, has_following_paren);
            }
        }
    }
    if (d->variants.len > 0) {
        for (size_t i = 0; i < d->variants.len; i++) {
            const SnDecl *v = SN_LIST_AT(d->variants, const SnDecl, i);
            collect_decl_symbols(list, v, ctx, has_following_paren);
        }
    }
}

/* ── Cross-Package & Prelude Resolver Symbols ────────────────────────────── */

static void collect_scope_symbols(CompList *list, SnScope *scope, ComplContext ctx, int base_score, bool has_following_paren) {
    if (!scope || !scope->buckets) return;
    for (size_t b = 0; b < scope->nbuckets; b++) {
        for (SnSymbol *sym = scope->buckets[b]; sym; sym = sym->next) {
            if (!sym->name || !sym->name[0]) continue;

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
                case SN_SYM_FIELD:
                    kind = LSP_COMPLETION_FIELD;
                    kind_str = "field";
                    break;
                case SN_SYM_CONST:
                    kind = LSP_COMPLETION_CONSTANT;
                    kind_str = "const";
                    break;
                case SN_SYM_VARIANT:
                    kind = LSP_COMPLETION_ENUM_MEMBER;
                    kind_str = "variant";
                    break;
                case SN_SYM_PARAM:
                    kind = LSP_COMPLETION_VARIABLE;
                    kind_str = "param";
                    break;
                case SN_SYM_LOCAL:
                    kind = LSP_COMPLETION_VARIABLE;
                    kind_str = "local";
                    break;
                case SN_SYM_PACKAGE:
                    kind = LSP_COMPLETION_MODULE;
                    kind_str = "package";
                    break;
                default:
                    break;
            }

            if (ctx == CTX_TYPE_POS && sym->kind != SN_SYM_TYPE) {
                continue;
            }

            char detail[256];
            snprintf(detail, sizeof(detail), "%s %s", kind_str, sym->name);

            char insert_text[256];
            int insert_fmt = 1;
            if ((sym->kind == SN_SYM_FUNC || sym->kind == SN_SYM_METHOD) && !has_following_paren) {
                snprintf(insert_text, sizeof(insert_text), "%s($1)", sym->name);
                insert_fmt = 2;
            } else {
                snprintf(insert_text, sizeof(insert_text), "%s", sym->name);
            }

            complist_add(list, sym->name, kind, detail, NULL, insert_text, insert_fmt, NULL, base_score, type_bonus);
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

    // 1. In-scope parameters & local variables (highest priority)
    if (ctx != CTX_MEMBER && ctx != CTX_DECORATOR && ctx != CTX_IMPORT_LINE) {
        collect_scope_locals(&list, doc->text, pos.line, has_following_paren);
    }

    // 2. Current AST declarations in the active unit
    if (a && a->has_ast) {
        for (size_t i = 0; i < a->unit.decls.len; i++) {
            const SnDecl *d = SN_LIST_AT(a->unit.decls, const SnDecl, i);
            collect_decl_symbols(&list, d, ctx, has_following_paren);
        }
    }

    // 3. Current package & imported symbols & prelude from resolver
    if (a && a->has_resolved) {
        const char *pkg_name = a->unit.package ? a->unit.package : "";
        SnScope *pkg_scope = sn_resolver_package_scope(&a->resolver, pkg_name);
        if (pkg_scope) {
            collect_scope_symbols(&list, pkg_scope, ctx, 80, has_following_paren);
        }

        // Imports
        for (size_t i = 0; i < a->unit.imports.len; i++) {
            const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
            SnScope *imp_scope = sn_resolver_package_scope(&a->resolver, imp);
            if (imp_scope) {
                collect_scope_symbols(&list, imp_scope, ctx, 75, has_following_paren);
            }
        }

        // Prelude
        if (a->resolver.prelude_scope) {
            collect_scope_symbols(&list, a->resolver.prelude_scope, ctx, 70, has_following_paren);
        }

        // Type member scopes (when in member access)
        if (ctx == CTX_MEMBER && receiver_buf[0]) {
            for (SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
                if (te->type_decl && te->type_decl->name && strcmp(te->type_decl->name, receiver_buf) == 0) {
                    collect_scope_symbols(&list, te->member_scope, ctx, 85, has_following_paren);
                }
            }
        }
    }

    // 4. Keywords, snippets, types, decorators
    add_keywords_and_snippets(&list, ctx, has_following_paren);

    // 5. Rank and Sort using Gopls / Rust-Analyzer 3-tier algorithm
    rank_and_sort_candidates(&list, prefix_buf);

    // 6. Build JSON-RPC response
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    jb_kv_bool(&jb, "isIncomplete", false);
    jb_key(&jb, "items");
    jb_start_arr(&jb);

    // Replacement range for prefix
    uint32_t start_col = pos.character >= (uint32_t)strlen(prefix_buf) ? pos.character - (uint32_t)strlen(prefix_buf) : 0;
    uint32_t end_col = pos.character;

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

        jb_end_obj(&jb);
    }

    jb_end_arr(&jb);
    jb_end_obj(&jb);

    complist_free(&list);
    return jb_take(&jb);
}
