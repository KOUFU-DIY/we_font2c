#include "font2c.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum JsonType {
    JSON_NULL,
    JSON_BOOL,
    JSON_INTEGER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct JsonArray {
    JsonValue **items;
    size_t count;
    size_t capacity;
} JsonArray;

typedef struct JsonMember {
    char *key;
    JsonValue *value;
} JsonMember;

typedef struct JsonObject {
    JsonMember *items;
    size_t count;
    size_t capacity;
} JsonObject;

struct JsonValue {
    JsonType type;
    union {
        bool boolean_value;
        long long integer_value;
        char *string_value;
        JsonArray array_value;
        JsonObject object_value;
    } u;
};

typedef struct JsonParser {
    const char *source_name;
    const char *text;
    size_t length;
    size_t position;
} JsonParser;

static size_t parser_line(const JsonParser *parser)
{
    size_t line = 1;
    size_t i;
    for (i = 0; i < parser->position && i < parser->length; ++i) {
        if (parser->text[i] == '\n') {
            ++line;
        }
    }
    return line;
}

static size_t parser_column(const JsonParser *parser)
{
    size_t column = 1;
    size_t i = parser->position;
    while (i > 0 && parser->text[i - 1] != '\n') {
        --i;
        ++column;
    }
    return column;
}

static bool parser_fail(JsonParser *parser, char *error, size_t error_cap, const char *fmt, ...)
{
    char detail[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
    return failf(error, error_cap, "%s:%zu:%zu: %s", parser->source_name, parser_line(parser), parser_column(parser), detail);
}

static void parser_skip_ws(JsonParser *parser)
{
    while (parser->position < parser->length) {
        char ch = parser->text[parser->position];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++parser->position;
        } else {
            break;
        }
    }
}

static JsonValue *json_new_value(JsonType type)
{
    JsonValue *value = (JsonValue *)calloc(1, sizeof(*value));
    if (value != NULL) {
        value->type = type;
    }
    return value;
}

static void json_free_value(JsonValue *value);

static bool json_array_push(JsonArray *array, JsonValue *value, char *error, size_t error_cap)
{
    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity == 0 ? 8 : array->capacity * 2;
        JsonValue **new_items = (JsonValue **)realloc(array->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = value;
    return true;
}

static bool json_object_push(JsonObject *object, char *key, JsonValue *value, char *error, size_t error_cap)
{
    if (object->count == object->capacity) {
        size_t new_capacity = object->capacity == 0 ? 8 : object->capacity * 2;
        JsonMember *new_items = (JsonMember *)realloc(object->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        object->items = new_items;
        object->capacity = new_capacity;
    }
    object->items[object->count].key = key;
    object->items[object->count].value = value;
    ++object->count;
    return true;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static bool parse_hex16(const char *text, uint32_t *value)
{
    int i;
    *value = 0;
    for (i = 0; i < 4; ++i) {
        int digit = hex_value(text[i]);
        if (digit < 0) {
            return false;
        }
        *value = (*value << 4) | (uint32_t)digit;
    }
    return true;
}

static bool json_parse_string(JsonParser *parser, char **out_string, char *error, size_t error_cap)
{
    StringBuilder sb = {0};
    uint32_t codepoint;

    ++parser->position;
    while (parser->position < parser->length) {
        char ch = parser->text[parser->position++];
        if (ch == '"') {
            *out_string = sb.data != NULL ? sb.data : dup_string("");
            if (*out_string == NULL) {
                sb_free(&sb);
                return failf(error, error_cap, "out of memory");
            }
            return true;
        }
        if ((unsigned char)ch < 0x20) {
            sb_free(&sb);
            return parser_fail(parser, error, error_cap, "control character is not allowed in a JSON string");
        }
        if (ch != '\\') {
            if (!sb_append_char(&sb, ch, error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            continue;
        }

        if (parser->position >= parser->length) {
            sb_free(&sb);
            return parser_fail(parser, error, error_cap, "unterminated escape sequence");
        }

        ch = parser->text[parser->position++];
        switch (ch) {
        case '"':
        case '\\':
        case '/':
            if (!sb_append_char(&sb, ch, error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        case 'b':
            if (!sb_append_char(&sb, '\b', error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        case 'f':
            if (!sb_append_char(&sb, '\f', error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        case 'n':
            if (!sb_append_char(&sb, '\n', error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        case 'r':
            if (!sb_append_char(&sb, '\r', error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        case 't':
            if (!sb_append_char(&sb, '\t', error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        case 'u': {
            if (parser->position + 4 > parser->length || !parse_hex16(parser->text + parser->position, &codepoint)) {
                sb_free(&sb);
                return parser_fail(parser, error, error_cap, "invalid \\u escape");
            }
            parser->position += 4;

            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                uint32_t low_surrogate;
                if (parser->position + 6 > parser->length || parser->text[parser->position] != '\\' ||
                    parser->text[parser->position + 1] != 'u' ||
                    !parse_hex16(parser->text + parser->position + 2, &low_surrogate) ||
                    low_surrogate < 0xDC00 || low_surrogate > 0xDFFF) {
                    sb_free(&sb);
                    return parser_fail(parser, error, error_cap, "invalid UTF-16 surrogate pair");
                }
                parser->position += 6;
                codepoint = 0x10000u + (((codepoint - 0xD800u) << 10) | (low_surrogate - 0xDC00u));
            } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                sb_free(&sb);
                return parser_fail(parser, error, error_cap, "unexpected low surrogate");
            }

            if (!sb_append_utf8(&sb, codepoint, error, error_cap)) {
                sb_free(&sb);
                return false;
            }
            break;
        }
        default:
            sb_free(&sb);
            return parser_fail(parser, error, error_cap, "invalid escape character '%c'", ch);
        }
    }

    sb_free(&sb);
    return parser_fail(parser, error, error_cap, "unterminated string");
}

static bool json_parse_value(JsonParser *parser, JsonValue **out_value, char *error, size_t error_cap);

static bool json_parse_array(JsonParser *parser, JsonValue **out_value, char *error, size_t error_cap)
{
    JsonValue *value = json_new_value(JSON_ARRAY);
    if (value == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    ++parser->position;
    parser_skip_ws(parser);
    if (parser->position < parser->length && parser->text[parser->position] == ']') {
        ++parser->position;
        *out_value = value;
        return true;
    }

    while (parser->position < parser->length) {
        JsonValue *item = NULL;
        parser_skip_ws(parser);
        if (!json_parse_value(parser, &item, error, error_cap)) {
            json_free_value(value);
            return false;
        }
        if (!json_array_push(&value->u.array_value, item, error, error_cap)) {
            json_free_value(item);
            json_free_value(value);
            return false;
        }
        parser_skip_ws(parser);
        if (parser->position >= parser->length) {
            json_free_value(value);
            return parser_fail(parser, error, error_cap, "unterminated array");
        }
        if (parser->text[parser->position] == ']') {
            ++parser->position;
            *out_value = value;
            return true;
        }
        if (parser->text[parser->position] != ',') {
            json_free_value(value);
            return parser_fail(parser, error, error_cap, "expected ',' or ']'");
        }
        ++parser->position;
    }

    json_free_value(value);
    return parser_fail(parser, error, error_cap, "unterminated array");
}

static bool json_parse_object(JsonParser *parser, JsonValue **out_value, char *error, size_t error_cap)
{
    JsonValue *value = json_new_value(JSON_OBJECT);
    if (value == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    ++parser->position;
    parser_skip_ws(parser);
    if (parser->position < parser->length && parser->text[parser->position] == '}') {
        ++parser->position;
        *out_value = value;
        return true;
    }

    while (parser->position < parser->length) {
        char *key = NULL;
        JsonValue *member_value = NULL;

        parser_skip_ws(parser);
        if (parser->position >= parser->length || parser->text[parser->position] != '"') {
            json_free_value(value);
            return parser_fail(parser, error, error_cap, "expected object key string");
        }
        if (!json_parse_string(parser, &key, error, error_cap)) {
            json_free_value(value);
            return false;
        }

        parser_skip_ws(parser);
        if (parser->position >= parser->length || parser->text[parser->position] != ':') {
            free(key);
            json_free_value(value);
            return parser_fail(parser, error, error_cap, "expected ':' after object key");
        }
        ++parser->position;

        parser_skip_ws(parser);
        if (!json_parse_value(parser, &member_value, error, error_cap)) {
            free(key);
            json_free_value(value);
            return false;
        }

        if (!json_object_push(&value->u.object_value, key, member_value, error, error_cap)) {
            free(key);
            json_free_value(member_value);
            json_free_value(value);
            return false;
        }

        parser_skip_ws(parser);
        if (parser->position >= parser->length) {
            json_free_value(value);
            return parser_fail(parser, error, error_cap, "unterminated object");
        }
        if (parser->text[parser->position] == '}') {
            ++parser->position;
            *out_value = value;
            return true;
        }
        if (parser->text[parser->position] != ',') {
            json_free_value(value);
            return parser_fail(parser, error, error_cap, "expected ',' or '}'");
        }
        ++parser->position;
    }

    json_free_value(value);
    return parser_fail(parser, error, error_cap, "unterminated object");
}

static bool json_parse_number(JsonParser *parser, JsonValue **out_value, char *error, size_t error_cap)
{
    size_t start = parser->position;
    size_t length;
    char *copy;
    char *end_ptr = NULL;
    long long number;
    JsonValue *value;

    if (parser->text[parser->position] == '-') {
        ++parser->position;
    }
    if (parser->position >= parser->length || !isdigit((unsigned char)parser->text[parser->position])) {
        return parser_fail(parser, error, error_cap, "invalid number");
    }
    if (parser->text[parser->position] == '0') {
        ++parser->position;
        if (parser->position < parser->length && isdigit((unsigned char)parser->text[parser->position])) {
            return parser_fail(parser, error, error_cap, "leading zero is not allowed");
        }
    } else {
        while (parser->position < parser->length && isdigit((unsigned char)parser->text[parser->position])) {
            ++parser->position;
        }
    }
    if (parser->position < parser->length &&
        (parser->text[parser->position] == '.' || parser->text[parser->position] == 'e' || parser->text[parser->position] == 'E')) {
        return parser_fail(parser, error, error_cap, "floating-point numbers are not supported");
    }

    length = parser->position - start;
    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return failf(error, error_cap, "out of memory");
    }
    memcpy(copy, parser->text + start, length);
    copy[length] = '\0';

    errno = 0;
    number = strtoll(copy, &end_ptr, 10);
    if (errno != 0 || end_ptr == NULL || *end_ptr != '\0') {
        free(copy);
        return parser_fail(parser, error, error_cap, "invalid integer");
    }
    free(copy);

    value = json_new_value(JSON_INTEGER);
    if (value == NULL) {
        return failf(error, error_cap, "out of memory");
    }
    value->u.integer_value = number;
    *out_value = value;
    return true;
}

static bool json_parse_literal(JsonParser *parser, const char *literal, JsonType type, bool bool_value, JsonValue **out_value, char *error, size_t error_cap)
{
    size_t length = strlen(literal);
    JsonValue *value;
    if (parser->position + length > parser->length || memcmp(parser->text + parser->position, literal, length) != 0) {
        return parser_fail(parser, error, error_cap, "expected '%s'", literal);
    }
    parser->position += length;
    value = json_new_value(type);
    if (value == NULL) {
        return failf(error, error_cap, "out of memory");
    }
    if (type == JSON_BOOL) {
        value->u.boolean_value = bool_value;
    }
    *out_value = value;
    return true;
}

static bool json_parse_value(JsonParser *parser, JsonValue **out_value, char *error, size_t error_cap)
{
    JsonValue *value = NULL;
    parser_skip_ws(parser);
    if (parser->position >= parser->length) {
        return parser_fail(parser, error, error_cap, "unexpected end of input");
    }
    switch (parser->text[parser->position]) {
    case '{':
        return json_parse_object(parser, out_value, error, error_cap);
    case '[':
        return json_parse_array(parser, out_value, error, error_cap);
    case '"':
        value = json_new_value(JSON_STRING);
        if (value == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        if (!json_parse_string(parser, &value->u.string_value, error, error_cap)) {
            json_free_value(value);
            return false;
        }
        *out_value = value;
        return true;
    case 't':
        return json_parse_literal(parser, "true", JSON_BOOL, true, out_value, error, error_cap);
    case 'f':
        return json_parse_literal(parser, "false", JSON_BOOL, false, out_value, error, error_cap);
    case 'n':
        return json_parse_literal(parser, "null", JSON_NULL, false, out_value, error, error_cap);
    default:
        if (parser->text[parser->position] == '-' || isdigit((unsigned char)parser->text[parser->position])) {
            return json_parse_number(parser, out_value, error, error_cap);
        }
        return parser_fail(parser, error, error_cap, "unexpected character '%c'", parser->text[parser->position]);
    }
}

static void json_free_value(JsonValue *value)
{
    size_t i;
    if (value == NULL) {
        return;
    }
    switch (value->type) {
    case JSON_STRING:
        free(value->u.string_value);
        break;
    case JSON_ARRAY:
        for (i = 0; i < value->u.array_value.count; ++i) {
            json_free_value(value->u.array_value.items[i]);
        }
        free(value->u.array_value.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < value->u.object_value.count; ++i) {
            free(value->u.object_value.items[i].key);
            json_free_value(value->u.object_value.items[i].value);
        }
        free(value->u.object_value.items);
        break;
    default:
        break;
    }
    free(value);
}

static bool parse_json_document(const char *source_name, const char *text, JsonValue **out_root, char *error, size_t error_cap)
{
    JsonParser parser;
    size_t bom = 0;
    size_t text_length = strlen(text);

    if (text_length >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        bom = 3;
    }

    parser.source_name = source_name;
    parser.text = text + bom;
    parser.length = strlen(text + bom);
    parser.position = 0;

    if (!json_parse_value(&parser, out_root, error, error_cap)) {
        return false;
    }
    parser_skip_ws(&parser);
    if (parser.position != parser.length) {
        json_free_value(*out_root);
        *out_root = NULL;
        return parser_fail(&parser, error, error_cap, "trailing content after JSON root");
    }
    return true;
}

static const JsonValue *json_object_get(const JsonValue *object, const char *key)
{
    size_t i;
    if (object == NULL || object->type != JSON_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object->u.object_value.count; ++i) {
        if (strcmp(object->u.object_value.items[i].key, key) == 0) {
            return object->u.object_value.items[i].value;
        }
    }
    return NULL;
}

static bool json_require_object(const JsonValue *value, const char *name, const JsonValue **out_object, char *error, size_t error_cap)
{
    if (value == NULL || value->type != JSON_OBJECT) {
        return failf(error, error_cap, "%s must be an object", name);
    }
    *out_object = value;
    return true;
}

static bool json_require_string(const JsonValue *value, const char *name, const char **out_string, char *error, size_t error_cap)
{
    if (value == NULL || value->type != JSON_STRING) {
        return failf(error, error_cap, "%s must be a string", name);
    }
    *out_string = value->u.string_value;
    return true;
}

static bool json_require_integer(const JsonValue *value, const char *name, long long *out_integer, char *error, size_t error_cap)
{
    if (value == NULL || value->type != JSON_INTEGER) {
        return failf(error, error_cap, "%s must be an integer", name);
    }
    *out_integer = value->u.integer_value;
    return true;
}

static bool json_require_array(const JsonValue *value, const char *name, const JsonArray **out_array, char *error, size_t error_cap)
{
    if (value == NULL || value->type != JSON_ARRAY) {
        return failf(error, error_cap, "%s must be an array", name);
    }
    *out_array = &value->u.array_value;
    return true;
}

static bool parse_unicode_token(const char *text, uint32_t *out_codepoint, char *error, size_t error_cap)
{
    size_t length = strlen(text);
    uint32_t codepoint = 0;
    size_t i;
    int digit;

    if (length < 6 || length > 8 || !(text[0] == 'U' || text[0] == 'u') || text[1] != '+') {
        return failf(error, error_cap, "invalid Unicode token '%s'", text);
    }
    for (i = 2; i < length; ++i) {
        digit = hex_value(text[i]);
        if (digit < 0) {
            return failf(error, error_cap, "invalid Unicode token '%s'", text);
        }
        codepoint = (codepoint << 4) | (uint32_t)digit;
    }
    if (!is_scalar_value(codepoint)) {
        return failf(error, error_cap, "invalid Unicode scalar '%s'", text);
    }
    *out_codepoint = codepoint;
    return true;
}

static bool is_valid_symbol_name(const char *text)
{
    size_t i;
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (!(isalpha((unsigned char)text[0]) || text[0] == '_')) {
        return false;
    }
    for (i = 1; text[i] != '\0'; ++i) {
        if (!(isalnum((unsigned char)text[i]) || text[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool utf8_decode_next(const char *text, size_t length, size_t *offset, uint32_t *out_codepoint, char *error, size_t error_cap)
{
    unsigned char first;
    uint32_t codepoint;
    size_t remaining;

    if (*offset >= length) {
        return failf(error, error_cap, "unexpected end of UTF-8 string");
    }

    first = (unsigned char)text[*offset];
    if (first < 0x80) {
        *out_codepoint = first;
        *offset += 1;
        return true;
    }

    if ((first & 0xE0) == 0xC0) {
        remaining = 1;
        codepoint = first & 0x1F;
        if (codepoint == 0) {
            return failf(error, error_cap, "invalid overlong UTF-8 sequence");
        }
    } else if ((first & 0xF0) == 0xE0) {
        remaining = 2;
        codepoint = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        remaining = 3;
        codepoint = first & 0x07;
    } else {
        return failf(error, error_cap, "invalid UTF-8 leading byte 0x%02X", first);
    }

    if (*offset + remaining >= length + 1) {
        return failf(error, error_cap, "truncated UTF-8 sequence");
    }

    while (remaining > 0) {
        unsigned char next = (unsigned char)text[*offset + 1];
        if ((next & 0xC0) != 0x80) {
            return failf(error, error_cap, "invalid UTF-8 continuation byte 0x%02X", next);
        }
        codepoint = (codepoint << 6) | (uint32_t)(next & 0x3F);
        ++(*offset);
        --remaining;
    }
    ++(*offset);

    if (!is_scalar_value(codepoint)) {
        return failf(error, error_cap, "invalid Unicode codepoint U+%X in UTF-8 string", codepoint);
    }
    if ((codepoint <= 0x7F && first >= 0x80) ||
        (codepoint <= 0x7FF && (first & 0xF0) == 0xE0) ||
        (codepoint <= 0xFFFF && (first & 0xF8) == 0xF0)) {
        return failf(error, error_cap, "invalid overlong UTF-8 sequence");
    }

    *out_codepoint = codepoint;
    return true;
}

static bool push_range(CodeRange **items, size_t *count, size_t *capacity, CodeRange item, char *error, size_t error_cap)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);
        CodeRange *new_items = (CodeRange *)realloc(*items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[(*count)++] = item;
    return true;
}

static bool push_codepoint(uint32_t **items, size_t *count, size_t *capacity, uint32_t codepoint, char *error, size_t error_cap)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);
        uint32_t *new_items = (uint32_t *)realloc(*items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[(*count)++] = codepoint;
    return true;
}

static bool parse_chars_literal(const char *text, FontConfig *config, char *error, size_t error_cap)
{
    size_t offset = 0;
    size_t length = strlen(text);
    size_t capacity = 0;

    config->chars_literal = dup_string(text);
    if (config->chars_literal == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    while (offset < length) {
        uint32_t codepoint = 0;
        if (!utf8_decode_next(text, length, &offset, &codepoint, error, error_cap)) {
            return false;
        }
        if (!push_codepoint(&config->char_codepoints, &config->char_count, &capacity, codepoint, error, error_cap)) {
            return false;
        }
    }
    return true;
}

void free_font_config(FontConfig *config)
{
    free(config->json_path);
    free(config->json_dir);
    free(config->json_name);
    free(config->base_name);
    free(config->symbol);
    free(config->font_file);
    free(config->ranges);
    free(config->chars_literal);
    free(config->char_codepoints);
    memset(config, 0, sizeof(*config));
}

bool parse_font_config_file(const char *json_path, FontConfig *config, char *error, size_t error_cap)
{
    char *json_text = NULL;
    JsonValue *root = NULL;
    const JsonValue *root_obj = NULL;
    const JsonValue *font_obj;
    const JsonValue *render_obj;
    const JsonValue *charset_obj;
    const JsonValue *deploy_obj;
    const JsonValue *version_value;
    const JsonValue *value;
    const JsonArray *ranges_array = NULL;
    long long integer_value = 0;
    const char *string_value = NULL;
    size_t i;
    size_t range_capacity = 0;
    bool ok = false;

    memset(config, 0, sizeof(*config));
    config->face_index = 0;

    if (!read_entire_file(json_path, &json_text, NULL, error, error_cap)) {
        goto cleanup;
    }
    if (!parse_json_document(json_path, json_text, &root, error, error_cap)) {
        goto cleanup;
    }

    config->json_path = dup_string(json_path);
    config->json_dir = path_dirname(json_path);
    config->json_name = dup_string(path_filename(json_path));
    config->base_name = path_stem(json_path);
    if (config->json_path == NULL || config->json_dir == NULL || config->json_name == NULL || config->base_name == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }

    if (!json_require_object(root, "root", &root_obj, error, error_cap)) {
        goto cleanup;
    }

    version_value = json_object_get(root_obj, "version");
    if (!json_require_integer(version_value, "version", &integer_value, error, error_cap)) {
        goto cleanup;
    }
    if (integer_value != 1) {
        failf(error, error_cap, "version must be 1");
        goto cleanup;
    }

    value = json_object_get(root_obj, "symbol");
    if (!json_require_string(value, "symbol", &string_value, error, error_cap)) {
        goto cleanup;
    }
    if (!is_valid_symbol_name(string_value)) {
        failf(error, error_cap, "symbol must be a valid C identifier using letters, digits, and underscores");
        goto cleanup;
    }
    config->symbol = dup_string(string_value);
    if (config->symbol == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }

    font_obj = json_object_get(root_obj, "font");
    if (!json_require_object(font_obj, "font", &font_obj, error, error_cap)) {
        goto cleanup;
    }

    value = json_object_get(font_obj, "file");
    if (!json_require_string(value, "font.file", &string_value, error, error_cap)) {
        goto cleanup;
    }
    config->font_file = dup_string(string_value);
    if (config->font_file == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }

    value = json_object_get(font_obj, "size");
    if (!json_require_integer(value, "font.size", &integer_value, error, error_cap)) {
        goto cleanup;
    }
    if (integer_value <= 0 || integer_value > 4096) {
        failf(error, error_cap, "font.size must be between 1 and 4096");
        goto cleanup;
    }
    config->font_size = (int)integer_value;

    value = json_object_get(font_obj, "face_index");
    if (value != NULL) {
        if (!json_require_integer(value, "font.face_index", &integer_value, error, error_cap)) {
            goto cleanup;
        }
        if (integer_value < 0 || integer_value > 0x7FFFFFFF) {
            failf(error, error_cap, "font.face_index must be >= 0");
            goto cleanup;
        }
        config->face_index = (int)integer_value;
    }

    render_obj = json_object_get(root_obj, "render");
    if (!json_require_object(render_obj, "render", &render_obj, error, error_cap)) {
        goto cleanup;
    }
    value = json_object_get(render_obj, "bpp");
    if (!json_require_integer(value, "render.bpp", &integer_value, error, error_cap)) {
        goto cleanup;
    }
    if (integer_value != 1 && integer_value != 2 && integer_value != 4 && integer_value != 8) {
        failf(error, error_cap, "render.bpp must be 1, 2, 4, or 8");
        goto cleanup;
    }
    config->bpp = (int)integer_value;

    charset_obj = json_object_get(root_obj, "charset");
    if (!json_require_object(charset_obj, "charset", &charset_obj, error, error_cap)) {
        goto cleanup;
    }

    value = json_object_get(charset_obj, "ranges");
    if (value != NULL) {
        if (!json_require_array(value, "charset.ranges", &ranges_array, error, error_cap)) {
            goto cleanup;
        }
        for (i = 0; i < ranges_array->count; ++i) {
            const JsonArray *pair = NULL;
            const char *start_text = NULL;
            const char *end_text = NULL;
            uint32_t start_codepoint;
            uint32_t end_codepoint;
            CodeRange range;

            if (!json_require_array(ranges_array->items[i], "charset.ranges[]", &pair, error, error_cap)) {
                goto cleanup;
            }
            if (pair->count != 2) {
                failf(error, error_cap, "charset.ranges[%zu] must contain exactly 2 values", i);
                goto cleanup;
            }
            if (!json_require_string(pair->items[0], "charset.ranges[][0]", &start_text, error, error_cap) ||
                !json_require_string(pair->items[1], "charset.ranges[][1]", &end_text, error, error_cap)) {
                goto cleanup;
            }
            if (!parse_unicode_token(start_text, &start_codepoint, error, error_cap) ||
                !parse_unicode_token(end_text, &end_codepoint, error, error_cap)) {
                goto cleanup;
            }
            if (start_codepoint > end_codepoint) {
                failf(error, error_cap, "charset.ranges[%zu] start must not exceed end", i);
                goto cleanup;
            }
            range.start = start_codepoint;
            range.end = end_codepoint;
            if (!push_range(&config->ranges, &config->range_count, &range_capacity, range, error, error_cap)) {
                goto cleanup;
            }
        }
    }

    value = json_object_get(charset_obj, "chars");
    if (value != NULL) {
        if (!json_require_string(value, "charset.chars", &string_value, error, error_cap)) {
            goto cleanup;
        }
        if (!parse_chars_literal(string_value, config, error, error_cap)) {
            goto cleanup;
        }
    }

    if (config->range_count == 0 && config->char_count == 0) {
        failf(error, error_cap, "charset must contain at least one range or char");
        goto cleanup;
    }

    deploy_obj = json_object_get(root_obj, "deploy");
    if (!json_require_object(deploy_obj, "deploy", &deploy_obj, error, error_cap)) {
        goto cleanup;
    }
    value = json_object_get(deploy_obj, "mode");
    if (!json_require_string(value, "deploy.mode", &string_value, error, error_cap)) {
        goto cleanup;
    }
    if (strcmp(string_value, "internal") == 0) {
        config->deploy_mode = DEPLOY_INTERNAL;
    } else if (strcmp(string_value, "external") == 0) {
        config->deploy_mode = DEPLOY_EXTERNAL;
    } else {
        failf(error, error_cap, "deploy.mode must be 'internal' or 'external'");
        goto cleanup;
    }

    ok = true;

cleanup:
    if (!ok) {
        free_font_config(config);
    }
    json_free_value(root);
    free(json_text);
    return ok;
}
