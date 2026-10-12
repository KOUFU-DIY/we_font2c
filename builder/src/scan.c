#include "font2c.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SCAN_MAX_DEPTH 24
#define SCAN_LIST_PREVIEW 20

typedef struct CodepointSet {
    uint32_t *items;
    size_t count;
    size_t capacity;
} CodepointSet;

typedef struct ExtensionList {
    char *storage;
    const char **items;
    size_t count;
} ExtensionList;

typedef struct SourceFileList {
    char **items;
    size_t count;
    size_t capacity;
} SourceFileList;

static int compare_u32_values(const void *lhs, const void *rhs)
{
    uint32_t a = *(const uint32_t *)lhs;
    uint32_t b = *(const uint32_t *)rhs;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static int compare_cstring_ptrs(const void *lhs, const void *rhs)
{
    const char *a = *(const char *const *)lhs;
    const char *b = *(const char *const *)rhs;
    return strcmp(a, b);
}

static bool set_push(CodepointSet *set, uint32_t codepoint, char *error, size_t error_cap)
{
    if (set->count == set->capacity) {
        size_t new_capacity = set->capacity == 0 ? 64 : (set->capacity * 2);
        uint32_t *new_items = (uint32_t *)realloc(set->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        set->items = new_items;
        set->capacity = new_capacity;
    }
    set->items[set->count++] = codepoint;
    return true;
}

static void set_sort_unique(CodepointSet *set)
{
    size_t read;
    size_t write = 0;
    if (set->count == 0) {
        return;
    }
    qsort(set->items, set->count, sizeof(*set->items), compare_u32_values);
    for (read = 0; read < set->count; ++read) {
        if (write == 0 || set->items[write - 1] != set->items[read]) {
            set->items[write++] = set->items[read];
        }
    }
    set->count = write;
}

/* requires a sorted set */
static bool set_contains(const CodepointSet *set, uint32_t codepoint)
{
    return bsearch(&codepoint, set->items, set->count, sizeof(*set->items), compare_u32_values) != NULL;
}

static void set_free(CodepointSet *set)
{
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void free_extension_list(ExtensionList *exts)
{
    free(exts->storage);
    free((void *)exts->items);
    memset(exts, 0, sizeof(*exts));
}

static bool parse_extension_list(const char *text, ExtensionList *exts, char *error, size_t error_cap)
{
    size_t i;
    size_t capacity = 0;
    char *cursor;

    memset(exts, 0, sizeof(*exts));
    exts->storage = dup_string(text);
    if (exts->storage == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    cursor = exts->storage;
    while (cursor != NULL) {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        if (cursor[0] == '.') {
            ++cursor;
        }
        if (cursor[0] == '\0') {
            free_extension_list(exts);
            return failf(error, error_cap, "--ext contains an empty extension");
        }
        for (i = 0; cursor[i] != '\0'; ++i) {
            if (cursor[i] == '.') {
                free_extension_list(exts);
                return failf(error, error_cap, "--ext entry '%s' must not contain '.'", cursor);
            }
        }
        if (exts->count == capacity) {
            size_t new_capacity = capacity == 0 ? 8 : (capacity * 2);
            const char **new_items = (const char **)realloc((void *)exts->items, new_capacity * sizeof(*new_items));
            if (new_items == NULL) {
                free_extension_list(exts);
                return failf(error, error_cap, "out of memory");
            }
            exts->items = new_items;
            capacity = new_capacity;
        }
        exts->items[exts->count++] = cursor;
        cursor = comma != NULL ? comma + 1 : NULL;
    }
    return true;
}

static bool has_listed_extension(const char *name, const ExtensionList *exts)
{
    const char *dot = strrchr(name, '.');
    size_t i;
    if (dot == NULL || dot == name || dot[1] == '\0') {
        return false;
    }
    for (i = 0; i < exts->count; ++i) {
        if (ascii_casecmp(dot + 1, exts->items[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void source_list_free(SourceFileList *list)
{
    size_t i;
    for (i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool source_list_push(SourceFileList *list, char *path, char *error, size_t error_cap)
{
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 32 : (list->capacity * 2);
        char **new_items = (char **)realloc(list->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = path;
    return true;
}

static bool collect_source_files(const char *directory, const ExtensionList *exts, int depth, SourceFileList *list, char *error, size_t error_cap)
{
    DIR *dir;
    struct dirent *entry;

    if (depth > SCAN_MAX_DEPTH) {
        return true;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        /* unreadable subdirectories are skipped; the top-level --src is checked by the caller */
        return true;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *joined;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        joined = path_join(directory, entry->d_name);
        if (joined == NULL) {
            closedir(dir);
            return failf(error, error_cap, "out of memory");
        }

        if (stat(joined, &st) != 0) {
            free(joined);
            continue;
        }

        if ((st.st_mode & S_IFDIR) != 0) {
            bool ok = collect_source_files(joined, exts, depth + 1, list, error, error_cap);
            free(joined);
            if (!ok) {
                closedir(dir);
                return false;
            }
            continue;
        }

        if ((st.st_mode & S_IFREG) != 0 && has_listed_extension(entry->d_name, exts)) {
            if (!source_list_push(list, joined, error, error_cap)) {
                free(joined);
                closedir(dir);
                return false;
            }
            continue;
        }

        free(joined);
    }

    closedir(dir);
    return true;
}

static void source_list_sort_unique(SourceFileList *list)
{
    size_t read;
    size_t write = 0;
    if (list->count == 0) {
        return;
    }
    qsort(list->items, list->count, sizeof(*list->items), compare_cstring_ptrs);
    for (read = 0; read < list->count; ++read) {
        if (write > 0 && strcmp(list->items[write - 1], list->items[read]) == 0) {
            free(list->items[read]);
        } else {
            list->items[write++] = list->items[read];
        }
    }
    list->count = write;
}

static size_t skip_utf8_bom(const char *text, size_t length)
{
    if (length >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        return 3;
    }
    return 0;
}

static bool parse_hex_codepoint(const char *text, size_t length, size_t offset, int digits, uint32_t *out_value)
{
    int i;
    uint32_t value = 0;
    if (offset + (size_t)digits > length) {
        return false;
    }
    for (i = 0; i < digits; ++i) {
        char ch = text[offset + (size_t)i];
        int digit;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10 + (ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10 + (ch - 'A');
        } else {
            return false;
        }
        value = (value << 4) | (uint32_t)digit;
    }
    *out_value = value;
    return true;
}

static bool collect_if_relevant(CodepointSet *set, uint32_t codepoint, char *error, size_t error_cap)
{
    if (codepoint < 0x80 || !is_scalar_value(codepoint)) {
        return true;
    }
    return set_push(set, codepoint, error, error_cap);
}

/* One-pass C lexer: comments and char literals are consumed but not collected,
 * so quotes inside them can never open a string. Raw newlines end string/char
 * literal states to keep a stray quote from swallowing the rest of the file. */
static bool scan_source_text(const char *path, const char *text, size_t length, CodepointSet *set, char *error, size_t error_cap)
{
    enum {
        ST_CODE,
        ST_LINE_COMMENT,
        ST_BLOCK_COMMENT,
        ST_STRING,
        ST_CHAR
    } state = ST_CODE;
    size_t i = skip_utf8_bom(text, length);
    size_t invalid_bytes = 0;
    size_t unterminated = 0;

    while (i < length) {
        char ch = text[i];
        switch (state) {
        case ST_CODE:
            if (ch == '/' && i + 1 < length && text[i + 1] == '/') {
                state = ST_LINE_COMMENT;
                i += 2;
            } else if (ch == '/' && i + 1 < length && text[i + 1] == '*') {
                state = ST_BLOCK_COMMENT;
                i += 2;
            } else if (ch == '"') {
                state = ST_STRING;
                ++i;
            } else if (ch == '\'') {
                state = ST_CHAR;
                ++i;
            } else {
                ++i;
            }
            break;
        case ST_LINE_COMMENT:
            if (ch == '\n') {
                state = ST_CODE;
            }
            ++i;
            break;
        case ST_BLOCK_COMMENT:
            if (ch == '*' && i + 1 < length && text[i + 1] == '/') {
                state = ST_CODE;
                i += 2;
            } else {
                ++i;
            }
            break;
        case ST_STRING:
            if (ch == '"') {
                state = ST_CODE;
                ++i;
            } else if (ch == '\n') {
                ++unterminated;
                state = ST_CODE;
                ++i;
            } else if (ch == '\\') {
                if (i + 1 >= length) {
                    ++unterminated;
                    i = length;
                    state = ST_CODE;
                    break;
                }
                if (text[i + 1] == 'u' || text[i + 1] == 'U') {
                    int digits = text[i + 1] == 'u' ? 4 : 8;
                    uint32_t codepoint;
                    if (parse_hex_codepoint(text, length, i + 2, digits, &codepoint)) {
                        if (!collect_if_relevant(set, codepoint, error, error_cap)) {
                            return false;
                        }
                        i += 2 + (size_t)digits;
                        break;
                    }
                }
                i += 2;
            } else if ((unsigned char)ch >= 0x80) {
                uint32_t codepoint = 0;
                size_t before = i;
                char discard[64];
                if (utf8_decode_next(text, length, &i, &codepoint, discard, sizeof(discard))) {
                    if (!collect_if_relevant(set, codepoint, error, error_cap)) {
                        return false;
                    }
                } else {
                    ++invalid_bytes;
                    i = before + 1;
                }
            } else {
                ++i;
            }
            break;
        case ST_CHAR:
            if (ch == '\'') {
                state = ST_CODE;
                ++i;
            } else if (ch == '\n') {
                ++unterminated;
                state = ST_CODE;
                ++i;
            } else if (ch == '\\') {
                i += (i + 1 < length) ? 2 : 1;
            } else {
                ++i;
            }
            break;
        }
    }

    if (state == ST_STRING || state == ST_CHAR) {
        ++unterminated;
    }
    if (invalid_bytes > 0) {
        fprintf(stderr, "warning: %s: skipped %zu invalid UTF-8 byte(s)\n", path, invalid_bytes);
    }
    if (unterminated > 0) {
        fprintf(stderr, "warning: %s: %zu unterminated string or char literal(s)\n", path, unterminated);
    }
    return true;
}

static bool scan_extra_text(const char *path, const char *text, size_t length, CodepointSet *set, char *error, size_t error_cap)
{
    size_t i = skip_utf8_bom(text, length);
    size_t invalid_bytes = 0;

    while (i < length) {
        if ((unsigned char)text[i] < 0x80) {
            ++i;
            continue;
        }
        {
            uint32_t codepoint = 0;
            size_t before = i;
            char discard[64];
            if (utf8_decode_next(text, length, &i, &codepoint, discard, sizeof(discard))) {
                if (!collect_if_relevant(set, codepoint, error, error_cap)) {
                    return false;
                }
            } else {
                ++invalid_bytes;
                i = before + 1;
            }
        }
    }

    if (invalid_bytes > 0) {
        fprintf(stderr, "warning: %s: skipped %zu invalid UTF-8 byte(s)\n", path, invalid_bytes);
    }
    return true;
}

static void fput_codepoint_utf8(uint32_t codepoint, FILE *stream)
{
    if (codepoint < 0x80) {
        fputc((int)codepoint, stream);
    } else if (codepoint < 0x800) {
        fputc(0xC0 | (int)(codepoint >> 6), stream);
        fputc(0x80 | (int)(codepoint & 0x3F), stream);
    } else if (codepoint < 0x10000) {
        fputc(0xE0 | (int)(codepoint >> 12), stream);
        fputc(0x80 | (int)((codepoint >> 6) & 0x3F), stream);
        fputc(0x80 | (int)(codepoint & 0x3F), stream);
    } else {
        fputc(0xF0 | (int)(codepoint >> 18), stream);
        fputc(0x80 | (int)((codepoint >> 12) & 0x3F), stream);
        fputc(0x80 | (int)((codepoint >> 6) & 0x3F), stream);
        fputc(0x80 | (int)(codepoint & 0x3F), stream);
    }
}

static void print_codepoint_list(const char *label, const CodepointSet *set)
{
    size_t limit = set->count < SCAN_LIST_PREVIEW ? set->count : SCAN_LIST_PREVIEW;
    size_t i;
    if (set->count == 0) {
        return;
    }
    printf("%s:", label);
    for (i = 0; i < limit; ++i) {
        printf(" U+%04X(", set->items[i]);
        fput_codepoint_utf8(set->items[i], stdout);
        printf(")");
    }
    if (limit < set->count) {
        printf(" ... (%zu more)", set->count - limit);
    }
    printf("\n");
}

static bool build_chars_utf8(const CodepointSet *set, StringBuilder *sb, char *error, size_t error_cap)
{
    size_t i;
    for (i = 0; i < set->count; ++i) {
        if (!sb_append_utf8(sb, set->items[i], error, error_cap)) {
            return false;
        }
    }
    return true;
}

static bool apply_json_update(const CliOptions *options, const CodepointSet *scanned, char *error, size_t error_cap)
{
    FontConfig config;
    CodepointSet existing = {0};
    CodepointSet added = {0};
    CodepointSet unseen = {0};
    CodepointSet result = {0};
    StringBuilder chars = {0};
    size_t i;
    bool changed;
    bool ok = false;

    memset(&config, 0, sizeof(config));
    if (!parse_font_config_file(options->scan_json, &config, error, error_cap)) {
        goto cleanup;
    }

    for (i = 0; i < config.char_count; ++i) {
        if (!set_push(&existing, config.char_codepoints[i], error, error_cap)) {
            goto cleanup;
        }
    }
    set_sort_unique(&existing);

    /* ASCII entries in charset.chars are outside the scanner's domain: they are
     * never reported and never pruned, mirroring the >= U+0080 collection rule. */
    for (i = 0; i < scanned->count; ++i) {
        if (!set_contains(&existing, scanned->items[i])) {
            if (!set_push(&added, scanned->items[i], error, error_cap)) {
                goto cleanup;
            }
        }
    }
    for (i = 0; i < existing.count; ++i) {
        uint32_t codepoint = existing.items[i];
        bool keep = true;
        if (codepoint >= 0x80 && !set_contains(scanned, codepoint)) {
            if (!set_push(&unseen, codepoint, error, error_cap)) {
                goto cleanup;
            }
            keep = !options->scan_prune;
        }
        if (keep) {
            if (!set_push(&result, codepoint, error, error_cap)) {
                goto cleanup;
            }
        }
    }
    for (i = 0; i < added.count; ++i) {
        if (!set_push(&result, added.items[i], error, error_cap)) {
            goto cleanup;
        }
    }
    set_sort_unique(&result);

    changed = result.count != existing.count ||
              (result.count > 0 && memcmp(result.items, existing.items, result.count * sizeof(*result.items)) != 0);

    printf("config %s: %zu -> %zu chars (+%zu new, %zu unseen %s)\n",
           options->scan_json,
           existing.count,
           result.count,
           added.count,
           unseen.count,
           options->scan_prune ? "pruned" : "kept");
    print_codepoint_list("new", &added);
    print_codepoint_list("unseen", &unseen);

    if (!changed) {
        printf("config %s unchanged\n", options->scan_json);
        ok = true;
        goto cleanup;
    }

    if (result.count == 0 && config.range_count == 0) {
        failf(error, error_cap, "refusing to update %s: charset would become empty (no ranges, no chars)", options->scan_json);
        goto cleanup;
    }

    if (options->scan_dry_run) {
        printf("dry-run: would update %s\n", options->scan_json);
        ok = true;
        goto cleanup;
    }

    if (result.count == 0) {
        if (!rewrite_config_chars_file(options->scan_json, NULL, error, error_cap)) {
            goto cleanup;
        }
    } else {
        if (!build_chars_utf8(&result, &chars, error, error_cap) ||
            !rewrite_config_chars_file(options->scan_json, chars.data != NULL ? chars.data : "", error, error_cap)) {
            goto cleanup;
        }
    }
    printf("updated %s\n", options->scan_json);
    ok = true;

cleanup:
    sb_free(&chars);
    set_free(&result);
    set_free(&unseen);
    set_free(&added);
    set_free(&existing);
    free_font_config(&config);
    return ok;
}

bool run_scan_command(const CliOptions *options, char *error, size_t error_cap)
{
    ExtensionList exts = {0};
    SourceFileList files = {0};
    CodepointSet scanned = {0};
    StringBuilder out_text = {0};
    size_t i;
    bool ok = false;

    if (!parse_extension_list(options->scan_ext_list, &exts, error, error_cap)) {
        goto cleanup;
    }

    for (i = 0; i < options->scan_src_count; ++i) {
        if (!is_directory_path(options->scan_srcs[i])) {
            failf(error, error_cap, "source directory not found: '%s'", options->scan_srcs[i]);
            goto cleanup;
        }
        if (!collect_source_files(options->scan_srcs[i], &exts, 0, &files, error, error_cap)) {
            goto cleanup;
        }
    }
    source_list_sort_unique(&files);

    for (i = 0; i < files.count; ++i) {
        char *text = NULL;
        size_t length = 0;
        if (!read_entire_file(files.items[i], &text, &length, error, error_cap)) {
            goto cleanup;
        }
        if (!scan_source_text(files.items[i], text, length, &scanned, error, error_cap)) {
            free(text);
            goto cleanup;
        }
        free(text);
    }

    for (i = 0; i < options->scan_extra_count; ++i) {
        char *text = NULL;
        size_t length = 0;
        if (!read_entire_file(options->scan_extras[i], &text, &length, error, error_cap)) {
            goto cleanup;
        }
        if (!scan_extra_text(options->scan_extras[i], text, length, &scanned, error, error_cap)) {
            free(text);
            goto cleanup;
        }
        free(text);
    }

    set_sort_unique(&scanned);

    printf("scanned %zu source file(s)", files.count);
    if (options->scan_extra_count > 0) {
        printf(" + %zu extra file(s)", options->scan_extra_count);
    }
    printf(", found %zu unique char(s) >= U+0080\n", scanned.count);

    if (options->scan_out != NULL) {
        if (options->scan_dry_run) {
            printf("dry-run: would write %s (%zu chars)\n", options->scan_out, scanned.count);
        } else {
            if (!build_chars_utf8(&scanned, &out_text, error, error_cap) ||
                !sb_append_char(&out_text, '\n', error, error_cap) ||
                !write_text_file(options->scan_out, out_text.data, out_text.length, error, error_cap)) {
                goto cleanup;
            }
            printf("wrote %s (%zu chars)\n", options->scan_out, scanned.count);
        }
    }

    if (options->scan_json != NULL) {
        if (!apply_json_update(options, &scanned, error, error_cap)) {
            goto cleanup;
        }
    }

    ok = true;

cleanup:
    sb_free(&out_text);
    set_free(&scanned);
    source_list_free(&files);
    free_extension_list(&exts);
    return ok;
}
