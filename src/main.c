#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_protocol.h"
#include "lsp_transport.h"
#include "lsp_document.h"
#include "lsp_analysis.h"
#include "lsp_completion.h"
#include "lsp_hover.h"
#include "lsp_definition.h"
#include "lsp_symbols.h"
#include "lsp_code_action.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void publish_diagnostics(LspTransport *t, const LspDocument *doc, const LspDocAnalysis *a) {
    if (!t || !doc || !a) return;

    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    jb_kv_str(&jb, "jsonrpc", "2.0");
    jb_kv_str(&jb, "method", "textDocument/publishDiagnostics");
    
    jb_key(&jb, "params");
    jb_start_obj(&jb);
    jb_kv_str(&jb, "uri", doc->uri);
    jb_kv_int(&jb, "version", doc->version);

    jb_key(&jb, "diagnostics");
    jb_start_arr(&jb);

    for (size_t i = 0; i < a->diags.len; i++) {
        const CapturedDiag *cd = &a->diags.items[i];
        
        LspRange r = lsp_span_to_range(doc, cd->span.offset, cd->span.len, cd->span.line, cd->span.col);

        jb_start_obj(&jb);
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

        jb_kv_int(&jb, "severity", (int)(cd->level == SN_DIAG_WARNING ? LSP_SEVERITY_WARNING : LSP_SEVERITY_ERROR));
        if (cd->code > 0) {
            char code_str[32];
            snprintf(code_str, sizeof(code_str), "SNOVA%04d", cd->code);
            jb_kv_str(&jb, "code", code_str);
        }
        jb_kv_str(&jb, "source", "snovac");
        jb_kv_str(&jb, "message", cd->message ? cd->message : "");
        jb_end_obj(&jb);
    }

    jb_end_arr(&jb);
    jb_end_obj(&jb);
    jb_end_obj(&jb);

    char *json_str = jb_take(&jb);
    if (json_str) {
        lsp_transport_write_message(t, json_str, strlen(json_str));
        free(json_str);
    }
}

static void send_response(LspTransport *t, const JsonVal *id, const char *result_json_raw, bool is_null) {
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    jb_kv_str(&jb, "jsonrpc", "2.0");

    if (id) {
        if (id->kind == JSON_NUMBER) {
            jb_kv_int(&jb, "id", (int64_t)id->num_val);
        } else if (id->kind == JSON_STRING) {
            jb_kv_str(&jb, "id", id->str_val);
        } else {
            jb_kv_null(&jb, "id");
        }
    } else {
        jb_kv_null(&jb, "id");
    }

    if (is_null || !result_json_raw) {
        jb_kv_null(&jb, "result");
    } else {
        jb_kv_raw(&jb, "result", result_json_raw);
    }

    jb_end_obj(&jb);
    char *msg = jb_take(&jb);
    if (msg) {
        lsp_transport_write_message(t, msg, strlen(msg));
        free(msg);
    }
}

int main(int argc, char **argv) {
    const char *log_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        }
    }

    LspTransport transport;
    lsp_transport_init(&transport, stdin, stdout, log_path);

    LspDocStore doc_store;
    lsp_docstore_init(&doc_store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    lsp_log(&transport, "snova-lsp started");

    bool running = true;
    while (running) {
        size_t msg_len = 0;
        char *msg = lsp_transport_read_message(&transport, &msg_len);
        if (!msg) {
            break; // EOF
        }

        JsonPool *pool = json_pool_create(msg_len + 4096);
        const char *err = NULL;
        JsonVal *req = json_parse(pool, msg, msg_len, &err);
        if (!req || req->kind != JSON_OBJECT) {
            lsp_log(&transport, "Failed to parse JSON-RPC message: %s", err ? err : "unknown");
            json_pool_destroy(pool);
            free(msg);
            continue;
        }

        const char *method = json_get_str(req, "method", "");
        const JsonVal *id = json_get(req, "id");
        const JsonVal *params = json_get_obj(req, "params");

        if (strcmp(method, "initialize") == 0) {
            const char *root_uri = json_get_str(params, "rootUri", NULL);
            const char *root_path = json_get_str(params, "rootPath", NULL);
            if (root_uri) {
                char *p = lsp_uri_to_path(root_uri);
                if (p) {
                    lsp_engine_set_workspace_root(&engine, p);
                    free(p);
                }
            } else if (root_path) {
                lsp_engine_set_workspace_root(&engine, root_path);
            }

            JsonBuilder res;
            jb_init(&res);
            jb_start_obj(&res);
            jb_key(&res, "capabilities");
            jb_start_obj(&res);
            jb_kv_int(&res, "textDocumentSync", (int)LSP_SYNC_FULL);
            jb_kv_bool(&res, "hoverProvider", true);
            jb_kv_bool(&res, "definitionProvider", true);
            jb_kv_bool(&res, "documentSymbolProvider", true);
            jb_kv_bool(&res, "codeActionProvider", true);

            jb_key(&res, "completionProvider");
            jb_start_obj(&res);
            jb_kv_bool(&res, "resolveProvider", false);
            jb_key(&res, "triggerCharacters");
            jb_start_arr(&res);
            jb_str(&res, ".");
            jb_str(&res, ":");
            jb_str(&res, "@");
            jb_end_arr(&res);
            jb_end_obj(&res);

            jb_end_obj(&res);

            jb_key(&res, "serverInfo");
            jb_start_obj(&res);
            jb_kv_str(&res, "name", "snova-lsp");
            jb_kv_str(&res, "version", "1.0.0");
            jb_end_obj(&res);

            jb_end_obj(&res);

            char *res_str = jb_take(&res);
            send_response(&transport, id, res_str, false);
            if (res_str) free(res_str);
        } else if (strcmp(method, "initialized") == 0) {
            lsp_log(&transport, "Client initialized");
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            if (td) {
                const char *uri = json_get_str(td, "uri", "");
                int version = (int)json_get_int(td, "version", 0);
                const char *text = json_get_str(td, "text", "");
                LspDocument *doc = lsp_docstore_open(&doc_store, uri, version, text, strlen(text));
                if (doc) {
                    LspDocAnalysis *a = lsp_engine_analyze_document(&engine, &doc_store, doc);
                    if (a) {
                        publish_diagnostics(&transport, doc, a);
                    }
                }
            }
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            const JsonVal *content_changes = json_get_arr(params, "contentChanges");
            if (td && content_changes && json_arr_len(content_changes) > 0) {
                const char *uri = json_get_str(td, "uri", "");
                int version = (int)json_get_int(td, "version", 0);
                const JsonVal *last_change = json_arr_at(content_changes, json_arr_len(content_changes) - 1);
                const char *text = json_get_str(last_change, "text", "");
                LspDocument *doc = lsp_docstore_update(&doc_store, uri, version, text, strlen(text));
                if (doc) {
                    LspDocAnalysis *a = lsp_engine_analyze_document(&engine, &doc_store, doc);
                    if (a) {
                        publish_diagnostics(&transport, doc, a);
                    }
                }
            }
        } else if (strcmp(method, "textDocument/didClose") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            if (td) {
                const char *uri = json_get_str(td, "uri", "");
                lsp_engine_remove_analysis(&engine, uri);
                lsp_docstore_close(&doc_store, uri);
            }
        } else if (strcmp(method, "textDocument/completion") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            const JsonVal *pos_obj = json_get_obj(params, "position");
            if (td && pos_obj) {
                const char *uri = json_get_str(td, "uri", "");
                LspPosition pos = {
                    .line = (uint32_t)json_get_int(pos_obj, "line", 0),
                    .character = (uint32_t)json_get_int(pos_obj, "character", 0)
                };
                LspDocument *doc = lsp_docstore_get(&doc_store, uri);
                char *res_json = lsp_completion_query(&engine, &doc_store, doc, pos);
                send_response(&transport, id, res_json, res_json == NULL);
                if (res_json) free(res_json);
            } else {
                send_response(&transport, id, NULL, true);
            }
        } else if (strcmp(method, "textDocument/hover") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            const JsonVal *pos_obj = json_get_obj(params, "position");
            if (td && pos_obj) {
                const char *uri = json_get_str(td, "uri", "");
                LspPosition pos = {
                    .line = (uint32_t)json_get_int(pos_obj, "line", 0),
                    .character = (uint32_t)json_get_int(pos_obj, "character", 0)
                };
                LspDocument *doc = lsp_docstore_get(&doc_store, uri);
                char *res_json = lsp_hover_query(&engine, doc, pos);
                send_response(&transport, id, res_json, res_json == NULL);
                if (res_json) free(res_json);
            } else {
                send_response(&transport, id, NULL, true);
            }
        } else if (strcmp(method, "textDocument/definition") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            const JsonVal *pos_obj = json_get_obj(params, "position");
            if (td && pos_obj) {
                const char *uri = json_get_str(td, "uri", "");
                LspPosition pos = {
                    .line = (uint32_t)json_get_int(pos_obj, "line", 0),
                    .character = (uint32_t)json_get_int(pos_obj, "character", 0)
                };
                LspDocument *doc = lsp_docstore_get(&doc_store, uri);
                char *res_json = lsp_definition_query(&engine, &doc_store, doc, pos);
                send_response(&transport, id, res_json, res_json == NULL);
                if (res_json) free(res_json);
            } else {
                send_response(&transport, id, NULL, true);
            }
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            if (td) {
                const char *uri = json_get_str(td, "uri", "");
                LspDocument *doc = lsp_docstore_get(&doc_store, uri);
                char *res_json = lsp_document_symbols_query(&engine, doc);
                send_response(&transport, id, res_json, res_json == NULL);
                if (res_json) free(res_json);
            } else {
                send_response(&transport, id, NULL, true);
            }
        } else if (strcmp(method, "textDocument/codeAction") == 0) {
            const JsonVal *td = json_get_obj(params, "textDocument");
            const JsonVal *range_obj = json_get_obj(params, "range");
            const JsonVal *context = json_get_obj(params, "context");
            const JsonVal *diags = context ? json_get_arr(context, "diagnostics") : NULL;
            if (td && range_obj) {
                const char *uri = json_get_str(td, "uri", "");
                const JsonVal *start_obj = json_get_obj(range_obj, "start");
                const JsonVal *end_obj = json_get_obj(range_obj, "end");
                LspRange range = {
                    .start = {
                        .line = (uint32_t)(start_obj ? json_get_int(start_obj, "line", 0) : 0),
                        .character = (uint32_t)(start_obj ? json_get_int(start_obj, "character", 0) : 0)
                    },
                    .end = {
                        .line = (uint32_t)(end_obj ? json_get_int(end_obj, "line", 0) : 0),
                        .character = (uint32_t)(end_obj ? json_get_int(end_obj, "character", 0) : 0)
                    }
                };
                LspDocument *doc = lsp_docstore_get(&doc_store, uri);
                char *res_json = lsp_code_action_query(&engine, &doc_store, doc, range, diags);
                send_response(&transport, id, res_json, res_json == NULL);
                if (res_json) free(res_json);
            } else {
                send_response(&transport, id, "[]", false);
            }
        } else if (strcmp(method, "shutdown") == 0) {
            send_response(&transport, id, NULL, true);
        } else if (strcmp(method, "exit") == 0) {
            running = false;
        } else {
            if (id) {
                send_response(&transport, id, NULL, true);
            }
        }

        json_pool_destroy(pool);
        free(msg);
    }

    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&doc_store);
    lsp_transport_destroy(&transport);
    return 0;
}
