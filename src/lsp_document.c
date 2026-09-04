#include "lsp_document.h"
#include "driver_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char *lsp_uri_to_path(const char *uri) {
    if (!uri) return NULL;
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
#ifdef _WIN32
        if (*p == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') {
            p++;
        }
#endif
    }
    
    // URL decode into path
    size_t len = strlen(p);
    char *raw = (char *)malloc(len + 1);
    if (!raw) return NULL;
    
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '%' && i + 2 < len && isxdigit((unsigned char)p[i+1]) && isxdigit((unsigned char)p[i+2])) {
            char hex[3] = { p[i+1], p[i+2], '\0' };
            raw[w++] = (char)strtoul(hex, NULL, 16);
            i += 2;
        } else {
            raw[w++] = p[i];
        }
    }
    raw[w] = '\0';

    char norm[SNOVAC_PATH_MAX];
    normalize_path_into(raw, norm, sizeof(norm));
    free(raw);
    return strdup(norm[0] ? norm : p);
}

char *lsp_path_to_uri(const char *path) {
    if (!path || !path[0]) return NULL;
    if (strncmp(path, "file://", 7) == 0) {
        return strdup(path);
    }
    char norm[SNOVAC_PATH_MAX];
    normalize_path_into(path, norm, sizeof(norm));
    const char *p = norm[0] ? norm : path;
    size_t len = strlen(p);
    char *uri = (char *)malloc(8 + len * 3 + 1);
    if (!uri) return NULL;

    strcpy(uri, "file://");
#ifdef _WIN32
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        strcat(uri, "/");
    }
#else
    if (p[0] != '/') {
        strcat(uri, "/");
    }
#endif

    size_t w = strlen(uri);
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (isalnum((unsigned char)c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            uri[w++] = c;
        } else {
            sprintf(uri + w, "%%%02X", (unsigned char)c);
            w += 3;
        }
    }
    uri[w] = '\0';
    return uri;
}

static void rebuild_line_index(LspDocument *doc) {
    doc->line_count = 0;
    if (doc->line_cap < 16) {
        doc->line_cap = 16;
        doc->line_offsets = (uint32_t *)malloc(sizeof(uint32_t) * doc->line_cap);
    }
    
    // Line 0 starts at offset 0
    doc->line_offsets[doc->line_count++] = 0;

    for (size_t i = 0; i < doc->text_len; i++) {
        if (doc->text[i] == '\n') {
            if (doc->line_count >= doc->line_cap) {
                doc->line_cap *= 2;
                doc->line_offsets = (uint32_t *)realloc(doc->line_offsets, sizeof(uint32_t) * doc->line_cap);
            }
            doc->line_offsets[doc->line_count++] = (uint32_t)(i + 1);
        }
    }
}

void lsp_docstore_init(LspDocStore *store) {
    store->len = 0;
    store->cap = 8;
    store->docs = (LspDocument **)malloc(sizeof(LspDocument *) * store->cap);
}

static void free_document(LspDocument *doc) {
    if (!doc) return;
    if (doc->uri) free(doc->uri);
    if (doc->path) free(doc->path);
    if (doc->text) free(doc->text);
    if (doc->line_offsets) free(doc->line_offsets);
    free(doc);
}

void lsp_docstore_destroy(LspDocStore *store) {
    if (!store) return;
    for (size_t i = 0; i < store->len; i++) {
        free_document(store->docs[i]);
    }
    if (store->docs) free(store->docs);
    store->docs = NULL;
    store->len = 0;
    store->cap = 0;
}

LspDocument *lsp_docstore_open(LspDocStore *store, const char *uri, int version, const char *text, size_t text_len) {
    LspDocument *existing = lsp_docstore_get(store, uri);
    if (existing) {
        return lsp_docstore_update(store, uri, version, text, text_len);
    }

    LspDocument *doc = (LspDocument *)malloc(sizeof(LspDocument));
    if (!doc) return NULL;
    doc->uri = strdup(uri);
    doc->path = lsp_uri_to_path(uri);
    doc->version = version;
    doc->text_len = text_len;
    doc->text = (char *)malloc(text_len + 1);
    if (text && text_len > 0) {
        memcpy(doc->text, text, text_len);
    }
    doc->text[text_len] = '\0';
    doc->line_offsets = NULL;
    doc->line_count = 0;
    doc->line_cap = 0;
    rebuild_line_index(doc);

    if (store->len >= store->cap) {
        store->cap *= 2;
        store->docs = (LspDocument **)realloc(store->docs, sizeof(LspDocument *) * store->cap);
    }
    store->docs[store->len++] = doc;
    return doc;
}

LspDocument *lsp_docstore_update(LspDocStore *store, const char *uri, int version, const char *text, size_t text_len) {
    LspDocument *doc = lsp_docstore_get(store, uri);
    if (!doc) {
        return lsp_docstore_open(store, uri, version, text, text_len);
    }
    doc->version = version;
    if (doc->text) free(doc->text);
    doc->text_len = text_len;
    doc->text = (char *)malloc(text_len + 1);
    if (text && text_len > 0) {
        memcpy(doc->text, text, text_len);
    }
    doc->text[text_len] = '\0';
    rebuild_line_index(doc);
    return doc;
}

LspDocument *lsp_docstore_get(LspDocStore *store, const char *uri) {
    if (!store || !uri) return NULL;
    for (size_t i = 0; i < store->len; i++) {
        if (strcmp(store->docs[i]->uri, uri) == 0) {
            return store->docs[i];
        }
    }
    return NULL;
}

LspDocument *lsp_docstore_get_by_path(LspDocStore *store, const char *path) {
    if (!store || !path) return NULL;
    char norm[SNOVAC_PATH_MAX];
    normalize_path_into(path, norm, sizeof(norm));
    for (size_t i = 0; i < store->len; i++) {
        if (!store->docs[i]->path) continue;
        char doc_norm[SNOVAC_PATH_MAX];
        normalize_path_into(store->docs[i]->path, doc_norm, sizeof(doc_norm));
#ifdef _WIN32
        if (strcasecmp(norm, doc_norm) == 0) return store->docs[i];
#else
        if (strcmp(norm, doc_norm) == 0) return store->docs[i];
#endif
    }
    return NULL;
}

bool lsp_docstore_close(LspDocStore *store, const char *uri) {
    if (!store || !uri) return false;
    for (size_t i = 0; i < store->len; i++) {
        if (strcmp(store->docs[i]->uri, uri) == 0) {
            free_document(store->docs[i]);
            for (size_t j = i; j + 1 < store->len; j++) {
                store->docs[j] = store->docs[j + 1];
            }
            store->len--;
            return true;
        }
    }
    return false;
}

uint32_t lsp_pos_to_offset(const LspDocument *doc, LspPosition pos) {
    if (!doc || doc->line_count == 0) return 0;
    if (pos.line >= doc->line_count) {
        return (uint32_t)doc->text_len;
    }
    uint32_t line_start = doc->line_offsets[pos.line];
    uint32_t next_line = (pos.line + 1 < doc->line_count) ? doc->line_offsets[pos.line + 1] : (uint32_t)doc->text_len;
    uint32_t max_char = (next_line > line_start) ? (next_line - line_start) : 0;
    if (pos.character > max_char) {
        return line_start + max_char;
    }
    return line_start + pos.character;
}

LspPosition lsp_offset_to_pos(const LspDocument *doc, uint32_t offset) {
    LspPosition pos = {0, 0};
    if (!doc || doc->line_count == 0) return pos;
    if (offset > doc->text_len) offset = (uint32_t)doc->text_len;

    // Binary search line
    size_t low = 0;
    size_t high = doc->line_count - 1;
    size_t line = 0;
    while (low <= high) {
        size_t mid = (low + high) / 2;
        if (doc->line_offsets[mid] <= offset) {
            line = mid;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }

    pos.line = (uint32_t)line;
    pos.character = (uint32_t)(offset - doc->line_offsets[line]);
    return pos;
}

LspRange lsp_span_to_range(const LspDocument *doc, uint32_t offset, uint32_t len, uint32_t line, uint32_t col) {
    (void)line;
    (void)col;
    LspRange r;
    if (doc) {
        r.start = lsp_offset_to_pos(doc, offset);
        r.end = lsp_offset_to_pos(doc, offset + (len > 0 ? len : 1));
    } else {
        r.start.line = line > 0 ? line - 1 : 0;
        r.start.character = col > 0 ? col - 1 : 0;
        r.end.line = r.start.line;
        r.end.character = r.start.character + (len > 0 ? len : 1);
    }
    return r;
}
