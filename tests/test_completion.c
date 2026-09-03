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

int main(void) {
    printf("Running LSP completion ranking unit tests...\n");
    test_general_ranking();
    test_acronym_matching();
    test_type_context_ranking();
    test_decorator_ranking();
    printf("All completion tests passed successfully! (4/4)\n");
    return 0;
}
