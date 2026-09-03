#include "lsp_completion.h"
#include "lsp_analysis.h"
#include "lsp_document.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_general_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "func getHttpRequest(): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "func process(requestId: int, requestPayload: string): unit {\n"
        "    let localCount = 10;\n"
        "    let req = 42;\n"
        "    req\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snova", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion at line 9 ("    req"), character 7 (after 'req')
    LspPosition pos = { .line = 9, .character = 7 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    // Verify first item is exact match "req" (local variable) with preselect true
    const JsonVal *first = json_arr_at(items, 0);
    assert(strcmp(json_get_str(first, "label", ""), "req") == 0);
    assert(json_get_bool(first, "preselect", false) == true);
    assert(strcmp(json_get_str(first, "sortText", ""), "00000") == 0);

    // Verify "requestId" and "requestPayload" (parameters) rank right after exact match
    bool found_param = false;
    for (size_t i = 1; i < count; i++) {
        const JsonVal *item = json_arr_at(items, i);
        const char *label = json_get_str(item, "label", "");
        if (strcmp(label, "requestId") == 0 || strcmp(label, "requestPayload") == 0) {
            found_param = true;
            break;
        }
    }
    assert(found_param);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_general_ranking passed\n");
}

static void test_acronym_matching(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "func getHttpRequest(): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "func test(): unit {\n"
        "    ghr\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snova", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion after typing "ghr" at line 7, col 7
    LspPosition pos = { .line = 7, .character = 7 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    assert(json_arr_len(items) > 0);

    // Verify "getHttpRequest" matched via acronym "ghr"
    const JsonVal *first = json_arr_at(items, 0);
    assert(strstr(json_get_str(first, "label", ""), "getHttpRequest") != NULL);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_acronym_matching passed\n");
}

static void test_type_context_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "class UserAccount {\n"
        "    let id: int;\n"
        "}\n"
        "\n"
        "func test(u: ): unit {\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snova", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion in type position after `u: ` at line 6, col 14
    LspPosition pos = { .line = 6, .character = 14 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    // Verify types are offered (e.g. UserAccount, int, string, etc.)
    bool found_user_type = false;
    for (size_t i = 0; i < count; i++) {
        const JsonVal *item = json_arr_at(items, i);
        if (strcmp(json_get_str(item, "label", ""), "UserAccount") == 0) {
            found_user_type = true;
            break;
        }
    }
    assert(found_user_type);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_type_context_ranking passed\n");
}

static void test_decorator_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "@\n"
        "func handler(): unit {}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snova", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion after '@' at line 2, col 1
    LspPosition pos = { .line = 2, .character = 1 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    // Top items should be decorators (native, route, get, post, test, etc.)
    const JsonVal *first = json_arr_at(items, 0);
    assert(strcmp(json_get_str(first, "detail", ""), "decorator") == 0);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_decorator_ranking passed\n");
}

static void test_deps_recommendation(void) {
    // 1. Setup temporary workspace with .snovalang/deps/**/*.snova
    system("rm -rf /tmp/snova_lsp_deps_test");
    system("mkdir -p /tmp/snova_lsp_deps_test/.snovalang/deps/snova-remote/src");
    system("mkdir -p /tmp/snova_lsp_deps_test/src");

    FILE *f_manifest = fopen("/tmp/snova_lsp_deps_test/mod.sno", "w");
    if (f_manifest) {
        fprintf(f_manifest, "module app\n\nsnova \"1.0.0\"\n");
        fclose(f_manifest);
    }

    FILE *f_dep = fopen("/tmp/snova_lsp_deps_test/.snovalang/deps/snova-remote/src/Remote.snova", "w");
    if (f_dep) {
        fprintf(f_dep,
            "package Snova.Remote\n\n"
            "public class RemoteClient {\n"
            "    public var endpoint: string\n"
            "}\n\n"
            "public func fetchRemoteData(): string {\n"
            "    return \"payload\"\n"
            "}\n");
        fclose(f_dep);
    }

    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, "/tmp/snova_lsp_deps_test");

    const char *code =
        "package main\n"
        "\n"
        "func app(): unit {\n"
        "    fetchRemote\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///tmp/snova_lsp_deps_test/src/App.snova", 1, code, strlen(code));
    assert(doc != NULL);

    // Query completion after typing "fetchRemote"
    LspPosition pos = { .line = 3, .character = 15 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    bool found_remote_func = false;
    for (size_t i = 0; i < count; i++) {
        const JsonVal *item = json_arr_at(items, i);
        const char *label = json_get_str(item, "label", "");
        if (strcmp(label, "fetchRemoteData") == 0) {
            found_remote_func = true;
            const char *detail = json_get_str(item, "detail", "");
            assert(strstr(detail, "Snova.Remote") != NULL);
            break;
        }
    }
    assert(found_remote_func);

    json_pool_destroy(pool);
    free(json_res);

    // Also test type recommendation: "Remote" -> RemoteClient
    const char *code2 =
        "package main\n"
        "\n"
        "func app(): unit {\n"
        "    let c: Remote\n"
        "}\n";
    doc = lsp_docstore_open(&store, "file:///tmp/snova_lsp_deps_test/src/App.snova", 2, code2, strlen(code2));
    assert(doc != NULL);

    LspPosition pos2 = { .line = 3, .character = 17 };
    char *json_res2 = lsp_completion_query(&engine, &store, doc, pos2);
    assert(json_res2 != NULL);

    JsonPool *pool2 = json_pool_create(strlen(json_res2) + 1024);
    JsonVal *root2 = json_parse(pool2, json_res2, strlen(json_res2), &err);
    assert(root2 != NULL);

    const JsonVal *items2 = json_get_arr(root2, "items");
    assert(items2 != NULL);
    bool found_remote_type = false;
    for (size_t i = 0; i < json_arr_len(items2); i++) {
        const JsonVal *item = json_arr_at(items2, i);
        const char *label = json_get_str(item, "label", "");
        if (strcmp(label, "RemoteClient") == 0) {
            found_remote_type = true;
            break;
        }
    }
    assert(found_remote_type);

    json_pool_destroy(pool2);
    free(json_res2);

    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);

    system("rm -rf /tmp/snova_lsp_deps_test");
    printf("✓ test_deps_recommendation passed\n");
}

int main(void) {
    printf("Running LSP completion ranking unit tests...\n");
    test_general_ranking();
    test_acronym_matching();
    test_type_context_ranking();
    test_decorator_ranking();
    test_deps_recommendation();
    printf("All completion tests passed successfully! (5/5)\n");
    return 0;
}
