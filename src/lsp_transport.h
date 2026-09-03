#ifndef SNOVA_LSP_TRANSPORT_H
#define SNOVA_LSP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    FILE *in;
    FILE *out;
    FILE *log_file;
    char *read_buf;
    size_t read_buf_cap;
    size_t read_buf_len;
} LspTransport;

void lsp_transport_init(LspTransport *t, FILE *in, FILE *out, const char *log_path);
void lsp_transport_destroy(LspTransport *t);

/* Reads next JSON-RPC payload from transport. Returns dynamically allocated string (caller frees), or NULL on EOF/error. */
char *lsp_transport_read_message(LspTransport *t, size_t *out_len);

/* Writes JSON-RPC payload to transport. Adds Content-Length header and flushes. */
bool lsp_transport_write_message(LspTransport *t, const char *json_payload, size_t len);

/* Debug logging */
void lsp_log(LspTransport *t, const char *fmt, ...);

#endif /* SNOVA_LSP_TRANSPORT_H */
