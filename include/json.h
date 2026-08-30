#ifndef JSON_H
#define JSON_H

// Minimal read-only JSON parser, written for the glTF loader. Enough of RFC 8259
// to read a glTF chunk: objects, arrays, strings (with \u escapes), numbers,
// true/false/null. No streaming, no writing, no comments, no trailing commas.
//
// The whole document lives in one arena owned by JsonDoc, so every JsonValue and
// every string is valid until json_free() and must not be freed individually.

#include <stddef.h>
#include <stdbool.h>

typedef enum JsonType {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonDoc   JsonDoc;

// Parse text[0..len). Returns NULL on failure and, when errbuf is non-NULL,
// writes a NUL-terminated reason into it (byte offset included where useful).
JsonDoc* json_parse(const char* text, size_t len, char* errbuf, size_t errbuf_len);
void     json_free(JsonDoc* doc);

// Root value of the document. NULL only if doc is NULL.
const JsonValue* json_root(const JsonDoc* doc);

JsonType json_type(const JsonValue* v);   // JSON_NULL for a NULL pointer

// Object access. Returns NULL when v is not an object or the key is absent, so
// callers can chain lookups without checking every step.
const JsonValue* json_member(const JsonValue* v, const char* key);

// Array access. json_count returns 0 for non-arrays; json_at returns NULL when
// out of range. json_count also returns the member count of an object.
size_t           json_count(const JsonValue* v);
const JsonValue* json_at(const JsonValue* v, size_t index);

// Scalars. Each returns `fallback` when v is NULL or of the wrong type, so a
// missing optional field needs no branch at the call site.
double      json_number(const JsonValue* v, double fallback);
bool        json_bool(const JsonValue* v, bool fallback);
const char* json_string(const JsonValue* v, const char* fallback);

// Convenience: v[key] as a number/string with a fallback.
double      json_member_number(const JsonValue* v, const char* key, double fallback);
const char* json_member_string(const JsonValue* v, const char* key, const char* fallback);

#endif
