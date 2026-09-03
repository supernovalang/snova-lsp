#include "lsp_semantic.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    LspSemanticToken *items;
    size_t len;
    size_t cap;
} TokenList;

static void add_token(TokenList *list, uint32_t line, uint32_t start,
                      uint32_t length, uint32_t type) {
    if (!length) return;
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 32;
        list->items = realloc(list->items, list->cap * sizeof(*list->items));
    }
    list->items[list->len++] = (LspSemanticToken){line, start, length, type, 0};
}

static void add_interpolation_tokens(TokenList *list, const LspDocument *doc,
                                     const SnToken *tok) {
    const char *s = tok->text;
    size_t n = tok->span.len;
    if (!s || n < 4) return;
    size_t i = 0;
    while (i + 1 < n) {
        if (s[i] == '$' && s[i + 1] == '{') {
            size_t begin = i;
            int depth = 1;
            LspPosition delimiter = lsp_offset_to_pos(doc, tok->span.offset + (uint32_t)begin);
            add_token(list, delimiter.line, delimiter.character, 2, LSP_SEMANTIC_TYPE_OPERATOR);
            i += 2;
            int in_string = 0;
            while (i < n && depth) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (s[i] == '"') {
                    in_string = !in_string;
                    i++;
                    continue;
                }
                if (!in_string && s[i] == '{') depth++;
                else if (!in_string && s[i] == '}') depth--;
                if (depth && (isalnum((unsigned char)s[i]) || s[i] == '_')) {
                    size_t id = i++;
                    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
                    LspPosition p = lsp_offset_to_pos(doc, tok->span.offset + (uint32_t)id);
                    add_token(list, p.line, p.character, (uint32_t)(i - id),
                              LSP_SEMANTIC_TYPE_VARIABLE);
                    continue;
                }
                i++;
            }
        } else {
            i++;
        }
    }
}

char *lsp_semantic_tokens_query(LspAnalysisEngine *engine, const LspDocument *doc) {
    if (!doc) return NULL;
    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) a = lsp_engine_analyze_document(engine, NULL, doc);
    if (!a) return NULL;

    TokenList list = {0};
    for (size_t i = 0; i < a->tokens.len; i++) {
        const SnToken *tok = &a->tokens.data[i];
        LspPosition p = lsp_offset_to_pos(doc, tok->span.offset);
        uint32_t type = LSP_SEMANTIC_TYPE_VARIABLE;
        if (tok->kind == SN_TOK_STRING) {
            if (tok->has_interpolation || (tok->text && strchr(tok->text, '$'))) {
                add_interpolation_tokens(&list, doc, tok);
                /* String content and embedded expressions are separate
                 * tokens; overlapping a full string token would violate the
                 * semantic-token ordering contract. */
                continue;
            }
            type = LSP_SEMANTIC_TYPE_STRING;
        } else if (tok->kind >= SN_TOK_PACKAGE && tok->kind <= SN_TOK_FALSE) {
            type = LSP_SEMANTIC_TYPE_KEYWORD;
        } else if (tok->kind == SN_TOK_INT || tok->kind == SN_TOK_LONG ||
                   tok->kind == SN_TOK_DOUBLE || tok->kind == SN_TOK_DECIMAL) {
            type = LSP_SEMANTIC_TYPE_NUMBER;
        } else if (tok->kind == SN_TOK_FUNC || tok->kind == SN_TOK_METHOD) {
            type = LSP_SEMANTIC_TYPE_KEYWORD;
        } else if (tok->kind != SN_TOK_IDENT) {
            continue;
        }
        add_token(&list, p.line, p.character, tok->span.len, type);
    }

    JsonBuilder jb;
    jb_init(&jb); jb_start_obj(&jb); jb_key(&jb, "data"); jb_start_arr(&jb);
    uint32_t prev_line = 0, prev_start = 0;
    for (size_t i = 0; i < list.len; i++) {
        LspSemanticToken *t = &list.items[i];
        uint32_t dl = t->line - prev_line;
        uint32_t ds = dl ? t->start : t->start - prev_start;
        jb_int(&jb, dl); jb_int(&jb, ds); jb_int(&jb, t->length);
        jb_int(&jb, t->type); jb_int(&jb, t->modifiers);
        prev_line = t->line; prev_start = t->start;
    }
    jb_end_arr(&jb); jb_end_obj(&jb);
    free(list.items);
    return jb_take(&jb);
}
