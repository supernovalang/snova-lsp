#ifndef SNOVA_LSP_DOCUMENT_H
#define SNOVA_LSP_DOCUMENT_H

#include "lsp_protocol.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char *uri;
    char *path;
    int version;
    char *text;
    size_t text_len;

    // Line start byte offsets table for fast conversion
    uint32_t *line_offsets;
    size_t line_count;
    size_t line_cap;
} LspDocument;

typedef struct {
    LspDocument **docs;
    size_t len;
    size_t cap;
} LspDocStore;

void lsp_docstore_init(LspDocStore *store);
void lsp_docstore_destroy(LspDocStore *store);

LspDocument *lsp_docstore_open(LspDocStore *store, const char *uri, int version, const char *text, size_t text_len);
LspDocument *lsp_docstore_update(LspDocStore *store, const char *uri, int version, const char *text, size_t text_len);
LspDocument *lsp_docstore_get(LspDocStore *store, const char *uri);
LspDocument *lsp_docstore_get_by_path(LspDocStore *store, const char *path);
bool lsp_docstore_close(LspDocStore *store, const char *uri);

/* Position and Offset conversions */
uint32_t lsp_pos_to_offset(const LspDocument *doc, LspPosition pos);
LspPosition lsp_offset_to_pos(const LspDocument *doc, uint32_t offset);
LspRange lsp_span_to_range(const LspDocument *doc, uint32_t offset, uint32_t len, uint32_t line, uint32_t col);

/* URI utilities */
char *lsp_uri_to_path(const char *uri);
char *lsp_path_to_uri(const char *path);

#endif /* SNOVA_LSP_DOCUMENT_H */
