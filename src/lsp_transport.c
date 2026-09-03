#include "lsp_transport.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void lsp_transport_init(LspTransport *t, FILE *in, FILE *out, const char *log_path) {
    t->in = in ? in : stdin;
    t->out = out ? out : stdout;
    t->log_file = NULL;
    if (log_path && log_path[0]) {
        t->log_file = fopen(log_path, "a");
    }
    t->read_buf_cap = 65536;
    t->read_buf_len = 0;
    t->read_buf = (char *)malloc(t->read_buf_cap);
}

void lsp_transport_destroy(LspTransport *t) {
    if (t->log_file) {
        fclose(t->log_file);
        t->log_file = NULL;
    }
    if (t->read_buf) {
        free(t->read_buf);
        t->read_buf = NULL;
    }
    t->read_buf_len = 0;
    t->read_buf_cap = 0;
}

void lsp_log(LspTransport *t, const char *fmt, ...) {
    FILE *f = t ? t->log_file : NULL;
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

static bool read_more(LspTransport *t) {
    if (t->read_buf_len >= t->read_buf_cap) {
        size_t new_cap = t->read_buf_cap * 2;
        char *new_buf = (char *)realloc(t->read_buf, new_cap);
        if (!new_buf) return false;
        t->read_buf = new_buf;
        t->read_buf_cap = new_cap;
    }
    /*
     * stdin is normally a pipe. Reading the whole free buffer with fread can
     * wait for that many bytes on some C runtimes, delaying small JSON-RPC
     * requests (notably initialize) until the client times out. Read one byte
     * at a time so the framing loop can process partial pipe input promptly.
     */
    size_t n = fread(t->read_buf + t->read_buf_len, 1, 1, t->in);
    if (n == 0) {
        return false;
    }
    t->read_buf_len += n;
    return true;
}

char *lsp_transport_read_message(LspTransport *t, size_t *out_len) {
    while (1) {
        // Look for "\r\n\r\n" or "\n\n" header separator
        char *header_end = NULL;
        size_t header_sep_len = 0;
        for (size_t i = 0; i + 3 < t->read_buf_len; i++) {
            if (t->read_buf[i] == '\r' && t->read_buf[i+1] == '\n' &&
                t->read_buf[i+2] == '\r' && t->read_buf[i+3] == '\n') {
                header_end = t->read_buf + i;
                header_sep_len = 4;
                break;
            }
        }
        if (!header_end) {
            for (size_t i = 0; i + 1 < t->read_buf_len; i++) {
                if (t->read_buf[i] == '\n' && t->read_buf[i+1] == '\n') {
                    header_end = t->read_buf + i;
                    header_sep_len = 2;
                    break;
                }
            }
        }

        if (!header_end) {
            if (!read_more(t)) {
                return NULL; // EOF or error
            }
            continue;
        }

        // Parse Content-Length from header
        size_t header_len = (size_t)(header_end - t->read_buf);
        size_t content_length = 0;
        bool found_cl = false;

        const char *p = t->read_buf;
        while (p < header_end) {
            if (strncasecmp(p, "Content-Length:", 15) == 0) {
                p += 15;
                while (p < header_end && (*p == ' ' || *p == '\t')) p++;
                content_length = (size_t)strtoul(p, NULL, 10);
                found_cl = true;
                break;
            }
            // Advance to next line
            while (p < header_end && *p != '\n') p++;
            if (p < header_end && *p == '\n') p++;
        }

        if (!found_cl) {
            lsp_log(t, "Missing Content-Length header");
            // Skip bad header
            size_t shift = header_len + header_sep_len;
            memmove(t->read_buf, t->read_buf + shift, t->read_buf_len - shift);
            t->read_buf_len -= shift;
            continue;
        }

        size_t body_start = header_len + header_sep_len;
        size_t total_msg_len = body_start + content_length;

        while (t->read_buf_len < total_msg_len) {
            if (!read_more(t)) {
                return NULL;
            }
        }

        char *msg = (char *)malloc(content_length + 1);
        if (!msg) return NULL;
        memcpy(msg, t->read_buf + body_start, content_length);
        msg[content_length] = '\0';

        if (out_len) *out_len = content_length;

        // Shift remainder of buffer
        size_t remaining = t->read_buf_len - total_msg_len;
        if (remaining > 0) {
            memmove(t->read_buf, t->read_buf + total_msg_len, remaining);
        }
        t->read_buf_len = remaining;

        lsp_log(t, "<-- RECV [%zu bytes]: %s", content_length, msg);
        return msg;
    }
}

bool lsp_transport_write_message(LspTransport *t, const char *json_payload, size_t len) {
    if (!json_payload) return false;
    if (len == 0) len = strlen(json_payload);

    lsp_log(t, "--> SEND [%zu bytes]: %s", len, json_payload);

    int r = fprintf(t->out, "Content-Length: %zu\r\n\r\n%s", len, json_payload);
    if (r < 0) return false;
    fflush(t->out);
    return true;
}
