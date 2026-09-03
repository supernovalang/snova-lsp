#ifndef SNOVA_LSP_PROTOCOL_H
#define SNOVA_LSP_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* LSP JSON-RPC Error Codes */
#define LSP_ERR_PARSE_ERROR      -32700
#define LSP_ERR_INVALID_REQUEST  -32600
#define LSP_ERR_METHOD_NOT_FOUND -32601
#define LSP_ERR_INVALID_PARAMS   -32602
#define LSP_ERR_INTERNAL_ERROR   -32603
#define LSP_ERR_SERVER_NOT_INIT  -32002
#define LSP_ERR_REQUEST_CANCELLED -32800

/* Diagnostic Severity */
typedef enum {
    LSP_SEVERITY_ERROR       = 1,
    LSP_SEVERITY_WARNING     = 2,
    LSP_SEVERITY_INFORMATION = 3,
    LSP_SEVERITY_HINT        = 4
} LspSeverity;

/* Completion Item Kind */
typedef enum {
    LSP_COMPLETION_TEXT          = 1,
    LSP_COMPLETION_METHOD        = 2,
    LSP_COMPLETION_FUNCTION      = 3,
    LSP_COMPLETION_CONSTRUCTOR   = 4,
    LSP_COMPLETION_FIELD         = 5,
    LSP_COMPLETION_VARIABLE      = 6,
    LSP_COMPLETION_CLASS         = 7,
    LSP_COMPLETION_INTERFACE     = 8,
    LSP_COMPLETION_MODULE        = 9,
    LSP_COMPLETION_PROPERTY      = 10,
    LSP_COMPLETION_UNIT          = 11,
    LSP_COMPLETION_VALUE         = 12,
    LSP_COMPLETION_ENUM          = 13,
    LSP_COMPLETION_KEYWORD       = 14,
    LSP_COMPLETION_SNIPPET       = 15,
    LSP_COMPLETION_COLOR         = 16,
    LSP_COMPLETION_FILE          = 17,
    LSP_COMPLETION_REFERENCE     = 18,
    LSP_COMPLETION_FOLDER        = 19,
    LSP_COMPLETION_ENUM_MEMBER   = 20,
    LSP_COMPLETION_CONSTANT      = 21,
    LSP_COMPLETION_STRUCT        = 22,
    LSP_COMPLETION_EVENT         = 23,
    LSP_COMPLETION_OPERATOR      = 24,
    LSP_COMPLETION_TYPE_PARAM    = 25
} LspCompletionKind;

/* Symbol Kind */
typedef enum {
    LSP_SYMBOL_FILE          = 1,
    LSP_SYMBOL_MODULE        = 2,
    LSP_SYMBOL_NAMESPACE     = 3,
    LSP_SYMBOL_PACKAGE       = 4,
    LSP_SYMBOL_CLASS         = 5,
    LSP_SYMBOL_METHOD        = 6,
    LSP_SYMBOL_PROPERTY      = 7,
    LSP_SYMBOL_FIELD         = 8,
    LSP_SYMBOL_CONSTRUCTOR   = 9,
    LSP_SYMBOL_ENUM          = 10,
    LSP_SYMBOL_INTERFACE     = 11,
    LSP_SYMBOL_FUNCTION      = 12,
    LSP_SYMBOL_VARIABLE      = 13,
    LSP_SYMBOL_CONSTANT      = 14,
    LSP_SYMBOL_STRING        = 15,
    LSP_SYMBOL_NUMBER        = 16,
    LSP_SYMBOL_BOOLEAN       = 17,
    LSP_SYMBOL_ARRAY         = 18,
    LSP_SYMBOL_OBJECT        = 19,
    LSP_SYMBOL_KEY           = 20,
    LSP_SYMBOL_NULL          = 21,
    LSP_SYMBOL_ENUM_MEMBER   = 22,
    LSP_SYMBOL_STRUCT        = 23,
    LSP_SYMBOL_EVENT         = 24,
    LSP_SYMBOL_OPERATOR      = 25,
    LSP_SYMBOL_TYPE_PARAM    = 26
} LspSymbolKind;

/* Semantic Token Types */
typedef enum {
    LSP_SEMANTIC_TYPE_NAMESPACE = 0,
    LSP_SEMANTIC_TYPE_TYPE,
    LSP_SEMANTIC_TYPE_CLASS,
    LSP_SEMANTIC_TYPE_ENUM,
    LSP_SEMANTIC_TYPE_INTERFACE,
    LSP_SEMANTIC_TYPE_STRUCT,
    LSP_SEMANTIC_TYPE_TYPE_PARAM,
    LSP_SEMANTIC_TYPE_PARAMETER,
    LSP_SEMANTIC_TYPE_VARIABLE,
    LSP_SEMANTIC_TYPE_PROPERTY,
    LSP_SEMANTIC_TYPE_ENUM_MEMBER,
    LSP_SEMANTIC_TYPE_EVENT,
    LSP_SEMANTIC_TYPE_FUNCTION,
    LSP_SEMANTIC_TYPE_METHOD,
    LSP_SEMANTIC_TYPE_MACRO,
    LSP_SEMANTIC_TYPE_KEYWORD,
    LSP_SEMANTIC_TYPE_MODIFIER,
    LSP_SEMANTIC_TYPE_COMMENT,
    LSP_SEMANTIC_TYPE_STRING,
    LSP_SEMANTIC_TYPE_NUMBER,
    LSP_SEMANTIC_TYPE_REGEXP,
    LSP_SEMANTIC_TYPE_OPERATOR,
    LSP_SEMANTIC_TYPE_DECORATOR,
    LSP_SEMANTIC_TYPE_COUNT
} LspSemanticTokenType;

/* Semantic Token Modifiers */
typedef enum {
    LSP_SEMANTIC_MOD_DECLARATION    = (1 << 0),
    LSP_SEMANTIC_MOD_DEFINITION     = (1 << 1),
    LSP_SEMANTIC_MOD_READONLY       = (1 << 2),
    LSP_SEMANTIC_MOD_STATIC         = (1 << 3),
    LSP_SEMANTIC_MOD_DEPRECATED     = (1 << 4),
    LSP_SEMANTIC_MOD_ABSTRACT       = (1 << 5),
    LSP_SEMANTIC_MOD_ASYNC          = (1 << 6),
    LSP_SEMANTIC_MOD_MODIFICATION   = (1 << 7),
    LSP_SEMANTIC_MOD_DOCUMENTATION  = (1 << 8),
    LSP_SEMANTIC_MOD_DEFAULT_LIB    = (1 << 9)
} LspSemanticTokenModifier;

/* Text Document Sync Kind */
typedef enum {
    LSP_SYNC_NONE        = 0,
    LSP_SYNC_FULL        = 1,
    LSP_SYNC_INCREMENTAL = 2
} LspTextDocumentSyncKind;

/* 0-based LSP Position */
typedef struct {
    uint32_t line;
    uint32_t character;
} LspPosition;

/* 0-based LSP Range */
typedef struct {
    LspPosition start;
    LspPosition end;
} LspRange;

/* Location */
typedef struct {
    const char *uri;
    LspRange range;
} LspLocation;

/* Diagnostic */
typedef struct {
    LspRange range;
    LspSeverity severity;
    int code_num;
    const char *code_str; /* e.g. "SNOVA0023" */
    const char *source;   /* e.g. "snovac" */
    const char *message;
} LspDiagnostic;

#endif /* SNOVA_LSP_PROTOCOL_H */
