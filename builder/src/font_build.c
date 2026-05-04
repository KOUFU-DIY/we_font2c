#include "font2c.h"

#include <ctype.h>
#include <dirent.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct NormalizedCharset {
    CodeRange *ranges;
    size_t range_count;
    uint32_t *sparse;
    size_t sparse_count;
    size_t total_range_codepoints;
    char *sparse_chars_literal;
} NormalizedCharset;

typedef struct GlyphRecord {
    uint16_t adv_w;
    uint16_t box_w;
    uint16_t box_h;
    int16_t x_ofs;
    int16_t y_ofs;
    uint32_t bitmap_offset;
} GlyphRecord;

typedef struct GlyphTable {
    GlyphRecord *items;
    size_t count;
    size_t capacity;
} GlyphTable;

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

static int compare_ranges(const void *lhs, const void *rhs)
{
    const CodeRange *a = (const CodeRange *)lhs;
    const CodeRange *b = (const CodeRange *)rhs;
    if (a->start < b->start) {
        return -1;
    }
    if (a->start > b->start) {
        return 1;
    }
    if (a->end < b->end) {
        return -1;
    }
    if (a->end > b->end) {
        return 1;
    }
    return 0;
}

static int compare_u32(const void *lhs, const void *rhs)
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

static bool append_search_dir(char ***items, size_t *count, size_t *capacity, const char *path, char *error, size_t error_cap)
{
    char **new_items;
    if (path == NULL || path[0] == '\0') {
        return true;
    }
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);
        new_items = (char **)realloc(*items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[*count] = dup_string(path);
    if ((*items)[*count] == NULL) {
        return failf(error, error_cap, "out of memory");
    }
    ++(*count);
    return true;
}

static void free_search_dirs(char **items, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static bool find_file_recursive(const char *directory, const char *target_name, int depth, char **out_path, char *error, size_t error_cap)
{
    DIR *dir;
    struct dirent *entry;

    if (depth > 24 || !is_directory_path(directory)) {
        return false;
    }

    dir = opendir(directory);
    if (dir == NULL) {
        return false;
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
            failf(error, error_cap, "out of memory");
            return false;
        }

        if (stat(joined, &st) != 0) {
            free(joined);
            continue;
        }

        if ((st.st_mode & S_IFDIR) != 0) {
            if (find_file_recursive(joined, target_name, depth + 1, out_path, error, error_cap)) {
                free(joined);
                closedir(dir);
                return true;
            }
            free(joined);
            if (error[0] != '\0') {
                closedir(dir);
                return false;
            }
            continue;
        }

        if ((st.st_mode & S_IFREG) != 0 && ascii_casecmp(entry->d_name, target_name) == 0) {
            *out_path = joined;
            closedir(dir);
            return true;
        }

        free(joined);
    }

    closedir(dir);
    return false;
}

static bool find_system_font_file(const char *font_file, char **out_path, char *error, size_t error_cap)
{
    const char *basename = path_filename(font_file);
    char **search_dirs = NULL;
    size_t count = 0;
    size_t capacity = 0;
    bool found = false;
    size_t i;

#ifdef _WIN32
    {
        const char *windir = getenv("WINDIR");
        const char *system_root = getenv("SystemRoot");
        char *windir_fonts = NULL;
        char *system_fonts = NULL;

        if (windir != NULL && windir[0] != '\0') {
            windir_fonts = path_join(windir, "Fonts");
            if (windir_fonts == NULL || !append_search_dir(&search_dirs, &count, &capacity, windir_fonts, error, error_cap)) {
                free(windir_fonts);
                free_search_dirs(search_dirs, count);
                return false;
            }
            free(windir_fonts);
        }
        if (system_root != NULL && system_root[0] != '\0' && (windir == NULL || ascii_casecmp(windir, system_root) != 0)) {
            system_fonts = path_join(system_root, "Fonts");
            if (system_fonts == NULL || !append_search_dir(&search_dirs, &count, &capacity, system_fonts, error, error_cap)) {
                free(system_fonts);
                free_search_dirs(search_dirs, count);
                return false;
            }
            free(system_fonts);
        }
        if (count == 0 && !append_search_dir(&search_dirs, &count, &capacity, "C:\\Windows\\Fonts", error, error_cap)) {
            free_search_dirs(search_dirs, count);
            return false;
        }
    }
#else
    {
        const char *home = getenv("HOME");
        char *user_library_fonts = NULL;
        char *user_dot_fonts = NULL;
        char *user_local_fonts = NULL;

        if (!append_search_dir(&search_dirs, &count, &capacity, "/System/Library/Fonts", error, error_cap) ||
            !append_search_dir(&search_dirs, &count, &capacity, "/Library/Fonts", error, error_cap) ||
            !append_search_dir(&search_dirs, &count, &capacity, "/usr/share/fonts", error, error_cap) ||
            !append_search_dir(&search_dirs, &count, &capacity, "/usr/local/share/fonts", error, error_cap)) {
            free_search_dirs(search_dirs, count);
            return false;
        }

        if (home != NULL && home[0] != '\0') {
            user_library_fonts = path_join(home, "Library/Fonts");
            user_dot_fonts = path_join(home, ".fonts");
            user_local_fonts = path_join(home, ".local/share/fonts");
            if ((user_library_fonts != NULL && !append_search_dir(&search_dirs, &count, &capacity, user_library_fonts, error, error_cap)) ||
                (user_dot_fonts != NULL && !append_search_dir(&search_dirs, &count, &capacity, user_dot_fonts, error, error_cap)) ||
                (user_local_fonts != NULL && !append_search_dir(&search_dirs, &count, &capacity, user_local_fonts, error, error_cap))) {
                free(user_library_fonts);
                free(user_dot_fonts);
                free(user_local_fonts);
                free_search_dirs(search_dirs, count);
                return false;
            }
            free(user_library_fonts);
            free(user_dot_fonts);
            free(user_local_fonts);
        }
    }
#endif

    for (i = 0; i < count; ++i) {
        if (find_file_recursive(search_dirs[i], basename, 0, out_path, error, error_cap)) {
            found = true;
            break;
        }
        if (error[0] != '\0') {
            break;
        }
    }

    free_search_dirs(search_dirs, count);
    return found;
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

static void free_normalized_charset(NormalizedCharset *charset)
{
    free(charset->ranges);
    free(charset->sparse);
    free(charset->sparse_chars_literal);
    memset(charset, 0, sizeof(*charset));
}

static bool normalize_charset(const FontConfig *config, NormalizedCharset *normalized, char *error, size_t error_cap)
{
    size_t i;
    size_t sparse_capacity = 0;
    StringBuilder sparse_chars = {0};

    memset(normalized, 0, sizeof(*normalized));

    if (config->range_count > 0) {
        normalized->ranges = (CodeRange *)malloc(config->range_count * sizeof(*normalized->ranges));
        if (normalized->ranges == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        memcpy(normalized->ranges, config->ranges, config->range_count * sizeof(*normalized->ranges));
        normalized->range_count = config->range_count;
        qsort(normalized->ranges, normalized->range_count, sizeof(*normalized->ranges), compare_ranges);

        {
            size_t write_index = 0;
            for (i = 0; i < normalized->range_count; ++i) {
                CodeRange current = normalized->ranges[i];
                if (write_index == 0) {
                    normalized->ranges[write_index++] = current;
                } else if (current.start <= normalized->ranges[write_index - 1].end + 1u) {
                    if (current.end > normalized->ranges[write_index - 1].end) {
                        normalized->ranges[write_index - 1].end = current.end;
                    }
                } else {
                    normalized->ranges[write_index++] = current;
                }
            }
            normalized->range_count = write_index;
        }

        for (i = 0; i < normalized->range_count; ++i) {
            normalized->total_range_codepoints += (size_t)(normalized->ranges[i].end - normalized->ranges[i].start) + 1u;
        }
    }

    if (config->char_count > 0) {
        uint32_t *sorted = (uint32_t *)malloc(config->char_count * sizeof(*sorted));
        if (sorted == NULL) {
            free_normalized_charset(normalized);
            return failf(error, error_cap, "out of memory");
        }
        memcpy(sorted, config->char_codepoints, config->char_count * sizeof(*sorted));
        qsort(sorted, config->char_count, sizeof(*sorted), compare_u32);

        for (i = 0; i < config->char_count; ++i) {
            uint32_t codepoint = sorted[i];
            bool covered = false;
            size_t r;

            if (i > 0 && codepoint == sorted[i - 1]) {
                continue;
            }

            for (r = 0; r < normalized->range_count; ++r) {
                if (codepoint < normalized->ranges[r].start) {
                    break;
                }
                if (codepoint <= normalized->ranges[r].end) {
                    covered = true;
                    break;
                }
            }
            if (covered) {
                continue;
            }
            if (!push_codepoint(&normalized->sparse, &normalized->sparse_count, &sparse_capacity, codepoint, error, error_cap) ||
                !sb_append_utf8(&sparse_chars, codepoint, error, error_cap)) {
                free(sorted);
                sb_free(&sparse_chars);
                free_normalized_charset(normalized);
                return false;
            }
        }
        free(sorted);
    }

    if (normalized->total_range_codepoints + normalized->sparse_count == 0) {
        sb_free(&sparse_chars);
        free_normalized_charset(normalized);
        return failf(error, error_cap, "normalized charset is empty");
    }
    if (normalized->range_count > 0xFFFFu || normalized->sparse_count > 0xFFFFu ||
        normalized->total_range_codepoints + normalized->sparse_count > 0xFFFFu) {
        sb_free(&sparse_chars);
        free_normalized_charset(normalized);
        return failf(error, error_cap, "v1 format supports at most 65535 ranges, sparse codepoints, and glyphs");
    }

    normalized->sparse_chars_literal = sparse_chars.data != NULL ? sparse_chars.data : dup_string("");
    if (normalized->sparse_chars_literal == NULL) {
        free_normalized_charset(normalized);
        return failf(error, error_cap, "out of memory");
    }
    return true;
}

static char *resolve_font_path(const FontConfig *config, const char *project_root)
{
    char *candidate;
    char *fonts_dir;
    char *fallback;
    char *system_match = NULL;
    char error[ERROR_CAP];

    error[0] = '\0';

    candidate = path_join(config->json_dir, config->font_file);
    if (candidate == NULL) {
        return NULL;
    }
    if (file_exists(candidate)) {
        return candidate;
    }
    free(candidate);

    fonts_dir = path_join(project_root, "fonts");
    if (fonts_dir == NULL) {
        return NULL;
    }
    fallback = path_join(fonts_dir, config->font_file);
    free(fonts_dir);
    if (fallback == NULL) {
        return NULL;
    }
    if (file_exists(fallback)) {
        return fallback;
    }

    if (find_system_font_file(config->font_file, &system_match, error, sizeof(error))) {
        free(fallback);
        return system_match;
    }
    if (error[0] != '\0') {
        free(fallback);
        return NULL;
    }
    return fallback;
}

static int round_26_6(FT_Pos value)
{
    if (value >= 0) {
        return (int)((value + 32) / 64);
    }
    return (int)((value - 32) / 64);
}

static bool glyph_table_push(GlyphTable *table, GlyphRecord glyph, char *error, size_t error_cap)
{
    if (table->count == table->capacity) {
        size_t new_capacity = table->capacity == 0 ? 128 : table->capacity * 2;
        GlyphRecord *new_items = (GlyphRecord *)realloc(table->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        table->items = new_items;
        table->capacity = new_capacity;
    }
    table->items[table->count++] = glyph;
    return true;
}

static void glyph_table_free(GlyphTable *table)
{
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

static bool quantize_gray_bitmap(
    const FT_Bitmap *bitmap,
    int bpp,
    unsigned char **out_bytes,
    size_t *out_size,
    char *error,
    size_t error_cap)
{
    size_t row_stride;
    size_t bitmap_size;
    unsigned char *packed;
    unsigned int row;

    if (bitmap->width == 0 || bitmap->rows == 0) {
        *out_bytes = NULL;
        *out_size = 0;
        return true;
    }
    if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) {
        return failf(error, error_cap, "only FT_PIXEL_MODE_GRAY glyph bitmaps are supported in v1");
    }

    row_stride = (((size_t)bitmap->width * (size_t)bpp) + 7u) / 8u;
    bitmap_size = row_stride * (size_t)bitmap->rows;
    packed = (unsigned char *)calloc(bitmap_size, 1);
    if (packed == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    for (row = 0; row < bitmap->rows; ++row) {
        const unsigned char *row_ptr;
        unsigned int col;
        if (bitmap->pitch >= 0) {
            row_ptr = bitmap->buffer + (size_t)row * (size_t)bitmap->pitch;
        } else {
            row_ptr = bitmap->buffer + (size_t)(bitmap->rows - 1 - row) * (size_t)(-bitmap->pitch);
        }
        for (col = 0; col < bitmap->width; ++col) {
            unsigned int raw = row_ptr[col];
            unsigned int gray = bitmap->num_grays > 1 ? (raw * 255u) / ((unsigned int)bitmap->num_grays - 1u) : raw;
            unsigned int max_value = (1u << bpp) - 1u;
            unsigned int level = (gray * max_value + 127u) / 255u;
            size_t byte_index;
            unsigned int shift;

            switch (bpp) {
            case 1:
                byte_index = (size_t)row * row_stride + (size_t)col / 8u;
                shift = 7u - ((unsigned int)col & 7u);
                packed[byte_index] |= (unsigned char)((level & 0x1u) << shift);
                break;
            case 2:
                byte_index = (size_t)row * row_stride + (size_t)col / 4u;
                shift = 6u - (unsigned int)(2 * (col & 3));
                packed[byte_index] |= (unsigned char)((level & 0x3u) << shift);
                break;
            case 4:
                byte_index = (size_t)row * row_stride + (size_t)col / 2u;
                shift = (col & 1) == 0 ? 4u : 0u;
                packed[byte_index] |= (unsigned char)((level & 0xFu) << shift);
                break;
            case 8:
                byte_index = (size_t)row * row_stride + (size_t)col;
                packed[byte_index] = (unsigned char)(level & 0xFFu);
                break;
            default:
                free(packed);
                return failf(error, error_cap, "unsupported bpp %d", bpp);
            }
        }
    }

    *out_bytes = packed;
    *out_size = bitmap_size;
    return true;
}

static bool render_one_glyph(
    FT_Face face,
    uint32_t codepoint,
    int bpp,
    int baseline,
    ByteBuffer *bitmap_buffer,
    GlyphRecord *out_glyph,
    char *error,
    size_t error_cap)
{
    FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
    unsigned char *packed = NULL;
    size_t packed_size = 0;
    int advance;
    int y_ofs;

    if (glyph_index == 0) {
        return failf(error, error_cap, "missing glyph U+%04X in source font", codepoint);
    }
    if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) != 0) {
        return failf(error, error_cap, "failed to load glyph U+%04X", codepoint);
    }
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return failf(error, error_cap, "failed to render glyph U+%04X", codepoint);
    }

    advance = round_26_6(face->glyph->advance.x);
    y_ofs = baseline - face->glyph->bitmap_top;

    if (advance < 0 || advance > 0xFFFF) {
        return failf(error, error_cap, "glyph advance out of range for U+%04X", codepoint);
    }
    if (face->glyph->bitmap.width > 0xFFFF || face->glyph->bitmap.rows > 0xFFFF) {
        return failf(error, error_cap, "glyph bitmap dimensions out of range for U+%04X", codepoint);
    }
    if (face->glyph->bitmap_left < INT16_MIN || face->glyph->bitmap_left > INT16_MAX ||
        y_ofs < INT16_MIN || y_ofs > INT16_MAX) {
        return failf(error, error_cap, "glyph offsets out of range for U+%04X", codepoint);
    }

    if (!quantize_gray_bitmap(&face->glyph->bitmap, bpp, &packed, &packed_size, error, error_cap)) {
        return false;
    }

    out_glyph->adv_w = (uint16_t)advance;
    out_glyph->box_w = (uint16_t)face->glyph->bitmap.width;
    out_glyph->box_h = (uint16_t)face->glyph->bitmap.rows;
    out_glyph->x_ofs = (int16_t)face->glyph->bitmap_left;
    out_glyph->y_ofs = (int16_t)y_ofs;
    out_glyph->bitmap_offset = 0;

    if (packed_size > 0) {
        if (bitmap_buffer->length > 0xFFFFFFFFu - packed_size) {
            free(packed);
            return failf(error, error_cap, "bitmap data exceeds v1 u32 offset range");
        }
        out_glyph->bitmap_offset = (uint32_t)bitmap_buffer->length;
        if (!bb_append(bitmap_buffer, packed, packed_size, error, error_cap)) {
            free(packed);
            return false;
        }
    }

    free(packed);
    return true;
}

static bool build_glyph_table(
    FT_Face face,
    int bpp,
    int baseline,
    const NormalizedCharset *charset,
    GlyphTable *glyphs,
    ByteBuffer *bitmap_buffer,
    char *error,
    size_t error_cap)
{
    size_t i;
    memset(glyphs, 0, sizeof(*glyphs));
    memset(bitmap_buffer, 0, sizeof(*bitmap_buffer));

    for (i = 0; i < charset->range_count; ++i) {
        uint32_t codepoint;
        for (codepoint = charset->ranges[i].start; codepoint <= charset->ranges[i].end; ++codepoint) {
            GlyphRecord glyph;
            if (!render_one_glyph(face, codepoint, bpp, baseline, bitmap_buffer, &glyph, error, error_cap) ||
                !glyph_table_push(glyphs, glyph, error, error_cap)) {
                return false;
            }
            if (codepoint == charset->ranges[i].end) {
                break;
            }
        }
    }

    for (i = 0; i < charset->sparse_count; ++i) {
        GlyphRecord glyph;
        if (!render_one_glyph(face, charset->sparse[i], bpp, baseline, bitmap_buffer, &glyph, error, error_cap) ||
            !glyph_table_push(glyphs, glyph, error, error_cap)) {
            return false;
        }
    }

    return true;
}

static char *make_header_guard(const char *identifier)
{
    StringBuilder sb = {0};
    char error[ERROR_CAP];
    size_t i;

    for (i = 0; identifier[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)identifier[i];
        if (isalnum(ch)) {
            if (!sb_append_char(&sb, (char)toupper(ch), error, sizeof(error))) {
                sb_free(&sb);
                return NULL;
            }
        } else if (!sb_append_char(&sb, '_', error, sizeof(error))) {
            sb_free(&sb);
            return NULL;
        }
    }
    if (!sb_append_cstr(&sb, "_H", error, sizeof(error))) {
        sb_free(&sb);
        return NULL;
    }
    return sb.data;
}

static bool append_comment_text(StringBuilder *sb, const char *text, char *error, size_t error_cap)
{
    size_t i;
    for (i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\r') {
            if (!sb_append_cstr(sb, "\\r", error, error_cap)) {
                return false;
            }
        } else if (text[i] == '\n') {
            if (!sb_append_cstr(sb, "\\n", error, error_cap)) {
                return false;
            }
        } else if (text[i] == '\t') {
            if (!sb_append_cstr(sb, "\\t", error, error_cap)) {
                return false;
            }
        } else if (text[i] == '*' && text[i + 1] == '/') {
            if (!sb_append_cstr(sb, "* /", error, error_cap)) {
                return false;
            }
            ++i;
        } else if (!sb_append_char(sb, text[i], error, error_cap)) {
            return false;
        }
    }
    return true;
}

static bool append_comment_codepoint(StringBuilder *sb, uint32_t codepoint, char *error, size_t error_cap);

static bool append_ranges_comment(StringBuilder *sb, const NormalizedCharset *charset, char *error, size_t error_cap)
{
    size_t i;
    if (!sb_append_cstr(sb, " * ranges (normalized):\n", error, error_cap)) {
        return false;
    }
    if (charset->range_count == 0) {
        return sb_append_cstr(sb, " *   (none)\n", error, error_cap);
    }
    for (i = 0; i < charset->range_count; ++i) {
        if (!sb_append_format(sb, error, error_cap, " *   U+%04X(", charset->ranges[i].start) ||
            !append_comment_codepoint(sb, charset->ranges[i].start, error, error_cap) ||
            !sb_append_format(sb, error, error_cap, ") - U+%04X(", charset->ranges[i].end) ||
            !append_comment_codepoint(sb, charset->ranges[i].end, error, error_cap) ||
            !sb_append_cstr(sb, ")\n", error, error_cap)) {
            return false;
        }
    }
    return true;
}

static void collect_max_box_size(const GlyphTable *glyphs, uint16_t *max_box_width, uint16_t *max_box_height)
{
    size_t i;
    *max_box_width = 0;
    *max_box_height = 0;
    for (i = 0; i < glyphs->count; ++i) {
        if (glyphs->items[i].box_w > *max_box_width) {
            *max_box_width = glyphs->items[i].box_w;
        }
        if (glyphs->items[i].box_h > *max_box_height) {
            *max_box_height = glyphs->items[i].box_h;
        }
    }
}

static uint32_t crc32_ieee(const unsigned char *data, size_t length)
{
    static uint32_t table[256];
    static bool initialized = false;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    if (!initialized) {
        uint32_t index;
        for (index = 0; index < 256u; ++index) {
            uint32_t value = index;
            unsigned int bit;
            for (bit = 0; bit < 8u; ++bit) {
                if ((value & 1u) != 0u) {
                    value = 0xEDB88320u ^ (value >> 1);
                } else {
                    value >>= 1;
                }
            }
            table[index] = value;
        }
        initialized = true;
    }

    for (i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool build_external_blob(
    const GlyphTable *glyphs,
    const ByteBuffer *bitmap_buffer,
    ByteBuffer *blob,
    uint32_t *out_crc32,
    char *error,
    size_t error_cap)
{
    ByteBuffer payload = {0};
    size_t i;
    bool ok = false;

    for (i = 0; i < glyphs->count; ++i) {
        if (!bb_put_u16be(&payload, glyphs->items[i].adv_w, error, error_cap) ||
            !bb_put_u16be(&payload, glyphs->items[i].box_w, error, error_cap) ||
            !bb_put_u16be(&payload, glyphs->items[i].box_h, error, error_cap) ||
            !bb_put_u16be(&payload, (uint16_t)glyphs->items[i].x_ofs, error, error_cap) ||
            !bb_put_u16be(&payload, (uint16_t)glyphs->items[i].y_ofs, error, error_cap) ||
            !bb_put_u32be(&payload, glyphs->items[i].bitmap_offset, error, error_cap)) {
            goto cleanup;
        }
    }
    if (!bb_append(&payload, bitmap_buffer->data, bitmap_buffer->length, error, error_cap)) {
        goto cleanup;
    }

    memset(blob, 0, sizeof(*blob));
    *out_crc32 = crc32_ieee(payload.data, payload.length);
    if (!bb_put_u8(blob, 0x02u, error, error_cap) ||
        !bb_put_u8(blob, 0x00u, error, error_cap) ||
        !bb_put_u32be(blob, *out_crc32, error, error_cap) ||
        !bb_append(blob, payload.data, payload.length, error, error_cap)) {
        goto cleanup;
    }

    ok = true;

cleanup:
    bb_free(&payload);
    return ok;
}

static bool append_comment_codepoint(StringBuilder *sb, uint32_t codepoint, char *error, size_t error_cap)
{
    if (codepoint >= 0x20u && codepoint != 0x7Fu && is_scalar_value(codepoint)) {
        return sb_append_utf8(sb, codepoint, error, error_cap);
    }
    return sb_append_char(sb, '?', error, error_cap);
}

static bool append_chars_comment(StringBuilder *sb, const char *label, const char *text, char *error, size_t error_cap)
{
    if (!sb_append_format(sb, error, error_cap, " * %s:\n *   ", label)) {
        return false;
    }
    if (text == NULL || text[0] == '\0') {
        return sb_append_cstr(sb, "(none)\n", error, error_cap);
    }
    if (!append_comment_text(sb, text, error, error_cap) ||
        !sb_append_cstr(sb, "\n", error, error_cap)) {
        return false;
    }
    return true;
}

static bool append_runtime_type_block(StringBuilder *header, char *error, size_t error_cap)
{
    return sb_append_cstr(
        header,
        "#ifndef FONT2C_RUNTIME_TYPES_H\n"
        "#define FONT2C_RUNTIME_TYPES_H\n"
        "\n"
        "typedef struct font_range_t {\n"
        "    uint32_t start;\n"
        "    uint32_t end;\n"
        "} font_range_t;\n"
        "\n"
        "typedef struct font_glyph_desc_t {\n"
        "    uint16_t adv_w;\n"
        "    uint16_t box_w;\n"
        "    uint16_t box_h;\n"
        "    int16_t x_ofs;\n"
        "    int16_t y_ofs;\n"
        "    uint32_t bitmap_offset;\n"
        "} font_glyph_desc_t;\n"
        "\n"
        "typedef struct font_internal_t {\n"
        "    uint8_t bpp;\n"
        "    uint16_t line_height;\n"
        "    uint16_t baseline;\n"
        "    uint16_t range_count;\n"
        "    uint16_t sparse_count;\n"
        "    uint16_t glyph_count;\n"
        "    uint16_t max_box_width;\n"
        "    uint16_t max_box_height;\n"
        "    const font_range_t *ranges;\n"
        "    const uint32_t *sparse;\n"
        "    const font_glyph_desc_t *glyph_desc;\n"
        "    const uint8_t *bitmap_data;\n"
        "} font_internal_t;\n"
        "\n"
        "typedef struct font_external_t {\n"
        "    uint8_t bpp;\n"
        "    uint16_t line_height;\n"
        "    uint16_t baseline;\n"
        "    uint16_t range_count;\n"
        "    uint16_t sparse_count;\n"
        "    uint16_t glyph_count;\n"
        "    uint16_t max_box_width;\n"
        "    uint16_t max_box_height;\n"
        "    const font_range_t *ranges;\n"
        "    const uint32_t *sparse;\n"
        "    uint32_t blob_size;\n"
        "    uint32_t blob_crc32;\n"
        "} font_external_t;\n"
        "\n"
        "typedef struct font_external_handle_t {\n"
        "    const font_external_t *font;\n"
        "    uintptr_t blob_addr;\n"
        "} font_external_handle_t;\n"
        "\n"
        "#endif\n"
        "\n",
        error,
        error_cap);
}

static bool write_generated_header(
    const char *symbol,
    const char *header_path,
    bool external_mode,
    char *error,
    size_t error_cap)
{
    StringBuilder header = {0};
    char *guard = make_header_guard(symbol);
    bool ok = false;

    if (guard == NULL) {
        return failf(error, error_cap, "out of memory");
    }
    if (!sb_append_format(&header, error, error_cap, "#ifndef %s\n#define %s\n\n", guard, guard) ||
        !sb_append_cstr(&header, "#include <stddef.h>\n#include <stdint.h>\n\n", error, error_cap) ||
        !append_runtime_type_block(&header, error, error_cap) ||
        !sb_append_format(&header,
                          error,
                          error_cap,
                          "extern const %s %s;\n\n",
                          external_mode ? "font_external_t" : "font_internal_t",
                          symbol) ||
        !sb_append_cstr(&header, "#endif\n", error, error_cap)) {
        goto cleanup;
    }

    ok = write_text_file(header_path, header.data, header.length, error, error_cap);

cleanup:
    free(guard);
    sb_free(&header);
    return ok;
}

static bool append_source_preamble(
    StringBuilder *source,
    const FontConfig *config,
    const NormalizedCharset *charset,
    const char *header_name,
    const char *deploy_label,
    int line_height,
    int baseline,
    uint16_t max_box_width,
    uint16_t max_box_height,
    size_t glyph_count,
    size_t blob_size,
    uint32_t blob_crc32,
    char *error,
    size_t error_cap)
{
    if (!sb_append_cstr(source, "/*\n", error, error_cap) ||
        !sb_append_cstr(source, " * Generated by font2c.\n", error, error_cap) ||
        !sb_append_format(source, error, error_cap, " * config: %s\n", config->json_name) ||
        !sb_append_format(source, error, error_cap, " * symbol: %s\n", config->symbol) ||
        !sb_append_format(source, error, error_cap, " * deploy mode: %s\n", deploy_label) ||
        !sb_append_format(source, error, error_cap, " * source font: %s\n", config->font_file) ||
        !sb_append_format(source, error, error_cap, " * face index: %d\n", config->face_index) ||
        !sb_append_format(source, error, error_cap, " * font size: %d px\n", config->font_size) ||
        !sb_append_format(source, error, error_cap, " * bpp: %d\n", config->bpp) ||
        !sb_append_format(source, error, error_cap, " * line_height: %d\n", line_height) ||
        !sb_append_format(source, error, error_cap, " * baseline: %d\n", baseline) ||
        !sb_append_format(source, error, error_cap, " * max_box_width: %u\n", (unsigned)max_box_width) ||
        !sb_append_format(source, error, error_cap, " * max_box_height: %u\n", (unsigned)max_box_height) ||
        !sb_append_format(source, error, error_cap, " * glyph_count: %zu\n", glyph_count) ||
        !append_ranges_comment(source, charset, error, error_cap) ||
        !append_chars_comment(source, "direct chars (input)", config->chars_literal, error, error_cap) ||
        !append_chars_comment(source, "direct chars (normalized sparse)", charset->sparse_chars_literal, error, error_cap)) {
        return false;
    }

    if (strcmp(deploy_label, "external") == 0) {
        if (!sb_append_format(source, error, error_cap, " * blob_size: %zu\n", blob_size) ||
            !sb_append_format(source, error, error_cap, " * blob_crc32: 0x%08X\n", blob_crc32) ||
            !sb_append_cstr(source, " * external blob layout: [0]=0x02 [1]=0x00 [2:5]=crc32_be [6:...]=glyph_desc+bitmap\n", error, error_cap) ||
            !sb_append_cstr(source, " * user binds blob_addr at runtime through font_external_handle_t.\n", error, error_cap)) {
            return false;
        }
    }

    return sb_append_format(source,
                            error,
                            error_cap,
                            " */\n#include <stddef.h>\n#include <stdint.h>\n#include \"%s\"\n\n",
                            header_name);
}

static bool append_static_ranges(StringBuilder *source, const char *symbol, const NormalizedCharset *charset, char *error, size_t error_cap)
{
    size_t i;
    if (charset->range_count == 0) {
        return true;
    }
    if (!sb_append_format(source, error, error_cap, "static const font_range_t %s_ranges[] = {\n", symbol)) {
        return false;
    }
    for (i = 0; i < charset->range_count; ++i) {
        if (!sb_append_format(source,
                              error,
                              error_cap,
                              "    { .start = 0x%08Xu, .end = 0x%08Xu },\n",
                              charset->ranges[i].start,
                              charset->ranges[i].end)) {
            return false;
        }
    }
    return sb_append_cstr(source, "};\n\n", error, error_cap);
}

static bool append_static_sparse(StringBuilder *source, const char *symbol, const NormalizedCharset *charset, char *error, size_t error_cap)
{
    size_t i;
    if (charset->sparse_count == 0) {
        return true;
    }
    if (!sb_append_format(source, error, error_cap, "static const uint32_t %s_sparse[] = {\n", symbol)) {
        return false;
    }
    for (i = 0; i < charset->sparse_count; ++i) {
        if (!sb_append_format(source, error, error_cap, "    0x%08Xu,\n", charset->sparse[i])) {
            return false;
        }
    }
    return sb_append_cstr(source, "};\n\n", error, error_cap);
}

static bool append_static_glyph_desc(StringBuilder *source, const char *symbol, const GlyphTable *glyphs, char *error, size_t error_cap)
{
    size_t i;
    if (!sb_append_format(source, error, error_cap, "static const font_glyph_desc_t %s_glyph_desc[] = {\n", symbol)) {
        return false;
    }
    for (i = 0; i < glyphs->count; ++i) {
        if (!sb_append_format(source,
                              error,
                              error_cap,
                              "    { .adv_w = %uu, .box_w = %uu, .box_h = %uu, .x_ofs = %d, .y_ofs = %d, .bitmap_offset = %uu },\n",
                              (unsigned)glyphs->items[i].adv_w,
                              (unsigned)glyphs->items[i].box_w,
                              (unsigned)glyphs->items[i].box_h,
                              (int)glyphs->items[i].x_ofs,
                              (int)glyphs->items[i].y_ofs,
                              (unsigned)glyphs->items[i].bitmap_offset)) {
            return false;
        }
    }
    return sb_append_cstr(source, "};\n\n", error, error_cap);
}

static bool append_static_bitmap(StringBuilder *source, const char *symbol, const ByteBuffer *bitmap_buffer, char *error, size_t error_cap)
{
    size_t i;
    if (!sb_append_format(source, error, error_cap, "static const uint8_t %s_bitmap_data[] = {\n", symbol)) {
        return false;
    }
    if (bitmap_buffer->length == 0) {
        return sb_append_cstr(source, "    0x00\n};\n\n", error, error_cap);
    }
    for (i = 0; i < bitmap_buffer->length; ++i) {
        if (i % 12u == 0u && !sb_append_cstr(source, "    ", error, error_cap)) {
            return false;
        }
        if (!sb_append_format(source, error, error_cap, "0x%02X", bitmap_buffer->data[i])) {
            return false;
        }
        if (i + 1u != bitmap_buffer->length) {
            if (!sb_append_cstr(source, ", ", error, error_cap)) {
                return false;
            }
        }
        if (i % 12u == 11u || i + 1u == bitmap_buffer->length) {
            if (!sb_append_cstr(source, "\n", error, error_cap)) {
                return false;
            }
        }
    }
    return sb_append_cstr(source, "};\n\n", error, error_cap);
}

static bool emit_internal_mode(
    const FontConfig *config,
    const NormalizedCharset *charset,
    const GlyphTable *glyphs,
    const ByteBuffer *bitmap_buffer,
    int line_height,
    int baseline,
    uint16_t max_box_width,
    uint16_t max_box_height,
    const char *output_dir,
    char *error,
    size_t error_cap)
{
    char *header_name = NULL;
    char *source_name = NULL;
    char *header_path = NULL;
    char *source_path = NULL;
    StringBuilder source = {0};
    bool ok = false;

    header_name = (char *)malloc(strlen(config->base_name) + 3u);
    source_name = (char *)malloc(strlen(config->base_name) + 3u);
    if (header_name == NULL || source_name == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }
    snprintf(header_name, strlen(config->base_name) + 3u, "%s.h", config->base_name);
    snprintf(source_name, strlen(config->base_name) + 3u, "%s.c", config->base_name);
    header_path = path_join(output_dir, header_name);
    source_path = path_join(output_dir, source_name);
    if (header_path == NULL || source_path == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }

    if (!write_generated_header(config->symbol, header_path, false, error, error_cap) ||
        !append_source_preamble(&source,
                                config,
                                charset,
                                header_name,
                                "internal",
                                line_height,
                                baseline,
                                max_box_width,
                                max_box_height,
                                glyphs->count,
                                0u,
                                0u,
                                error,
                                error_cap) ||
        !append_static_ranges(&source, config->symbol, charset, error, error_cap) ||
        !append_static_sparse(&source, config->symbol, charset, error, error_cap) ||
        !append_static_glyph_desc(&source, config->symbol, glyphs, error, error_cap) ||
        !append_static_bitmap(&source, config->symbol, bitmap_buffer, error, error_cap) ||
        !sb_append_format(&source, error, error_cap, "const font_internal_t %s = {\n", config->symbol) ||
        !sb_append_format(&source, error, error_cap, "    .bpp = %uu,\n", (unsigned)config->bpp) ||
        !sb_append_format(&source, error, error_cap, "    .line_height = %uu,\n", (unsigned)line_height) ||
        !sb_append_format(&source, error, error_cap, "    .baseline = %uu,\n", (unsigned)baseline) ||
        !sb_append_format(&source, error, error_cap, "    .range_count = %uu,\n", (unsigned)charset->range_count) ||
        !sb_append_format(&source, error, error_cap, "    .sparse_count = %uu,\n", (unsigned)charset->sparse_count) ||
        !sb_append_format(&source, error, error_cap, "    .glyph_count = %uu,\n", (unsigned)glyphs->count) ||
        !sb_append_format(&source, error, error_cap, "    .max_box_width = %uu,\n", (unsigned)max_box_width) ||
        !sb_append_format(&source, error, error_cap, "    .max_box_height = %uu,\n", (unsigned)max_box_height)) {
        goto cleanup;
    }
    if (charset->range_count > 0) {
        if (!sb_append_format(&source, error, error_cap, "    .ranges = %s_ranges,\n", config->symbol)) {
            goto cleanup;
        }
    } else if (!sb_append_cstr(&source, "    .ranges = NULL,\n", error, error_cap)) {
        goto cleanup;
    }
    if (charset->sparse_count > 0) {
        if (!sb_append_format(&source, error, error_cap, "    .sparse = %s_sparse,\n", config->symbol)) {
            goto cleanup;
        }
    } else if (!sb_append_cstr(&source, "    .sparse = NULL,\n", error, error_cap)) {
        goto cleanup;
    }
    if (!sb_append_format(&source, error, error_cap, "    .glyph_desc = %s_glyph_desc,\n", config->symbol) ||
        !sb_append_format(&source, error, error_cap, "    .bitmap_data = %s_bitmap_data\n", config->symbol) ||
        !sb_append_cstr(&source, "};\n", error, error_cap) ||
        !write_text_file(source_path, source.data, source.length, error, error_cap)) {
        goto cleanup;
    }

    ok = true;

cleanup:
    free(header_name);
    free(source_name);
    free(header_path);
    free(source_path);
    sb_free(&source);
    return ok;
}

static bool emit_external_mode(
    const FontConfig *config,
    const NormalizedCharset *charset,
    int line_height,
    int baseline,
    uint16_t max_box_width,
    uint16_t max_box_height,
    const ByteBuffer *blob,
    uint32_t blob_crc32,
    size_t glyph_count,
    const char *output_dir,
    char *error,
    size_t error_cap)
{
    char *header_name = NULL;
    char *source_name = NULL;
    char *bin_name = NULL;
    char *header_path = NULL;
    char *source_path = NULL;
    char *bin_path = NULL;
    StringBuilder source = {0};
    bool ok = false;

    header_name = (char *)malloc(strlen(config->base_name) + 3u);
    source_name = (char *)malloc(strlen(config->base_name) + 3u);
    bin_name = (char *)malloc(strlen(config->base_name) + 5u);
    if (header_name == NULL || source_name == NULL || bin_name == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }
    snprintf(header_name, strlen(config->base_name) + 3u, "%s.h", config->base_name);
    snprintf(source_name, strlen(config->base_name) + 3u, "%s.c", config->base_name);
    snprintf(bin_name, strlen(config->base_name) + 5u, "%s.bin", config->base_name);
    header_path = path_join(output_dir, header_name);
    source_path = path_join(output_dir, source_name);
    bin_path = path_join(output_dir, bin_name);
    if (header_path == NULL || source_path == NULL || bin_path == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }

    if (!write_generated_header(config->symbol, header_path, true, error, error_cap) ||
        !append_source_preamble(&source,
                                config,
                                charset,
                                header_name,
                                "external",
                                line_height,
                                baseline,
                                max_box_width,
                                max_box_height,
                                glyph_count,
                                blob->length,
                                blob_crc32,
                                error,
                                error_cap) ||
        !append_static_ranges(&source, config->symbol, charset, error, error_cap) ||
        !append_static_sparse(&source, config->symbol, charset, error, error_cap) ||
        !sb_append_format(&source, error, error_cap, "const font_external_t %s = {\n", config->symbol) ||
        !sb_append_format(&source, error, error_cap, "    .bpp = %uu,\n", (unsigned)config->bpp) ||
        !sb_append_format(&source, error, error_cap, "    .line_height = %uu,\n", (unsigned)line_height) ||
        !sb_append_format(&source, error, error_cap, "    .baseline = %uu,\n", (unsigned)baseline) ||
        !sb_append_format(&source, error, error_cap, "    .range_count = %uu,\n", (unsigned)charset->range_count) ||
        !sb_append_format(&source, error, error_cap, "    .sparse_count = %uu,\n", (unsigned)charset->sparse_count) ||
        !sb_append_format(&source, error, error_cap, "    .glyph_count = %uu,\n", (unsigned)glyph_count) ||
        !sb_append_format(&source, error, error_cap, "    .max_box_width = %uu,\n", (unsigned)max_box_width) ||
        !sb_append_format(&source, error, error_cap, "    .max_box_height = %uu,\n", (unsigned)max_box_height)) {
        goto cleanup;
    }
    if (charset->range_count > 0) {
        if (!sb_append_format(&source, error, error_cap, "    .ranges = %s_ranges,\n", config->symbol)) {
            goto cleanup;
        }
    } else if (!sb_append_cstr(&source, "    .ranges = NULL,\n", error, error_cap)) {
        goto cleanup;
    }
    if (charset->sparse_count > 0) {
        if (!sb_append_format(&source, error, error_cap, "    .sparse = %s_sparse,\n", config->symbol)) {
            goto cleanup;
        }
    } else if (!sb_append_cstr(&source, "    .sparse = NULL,\n", error, error_cap)) {
        goto cleanup;
    }
    if (!sb_append_format(&source, error, error_cap, "    .blob_size = %uu,\n", (unsigned)blob->length) ||
        !sb_append_format(&source, error, error_cap, "    .blob_crc32 = 0x%08Xu\n", blob_crc32) ||
        !sb_append_cstr(&source, "};\n", error, error_cap) ||
        !write_text_file(source_path, source.data, source.length, error, error_cap) ||
        !write_binary_file(bin_path, blob->data, blob->length, error, error_cap)) {
        goto cleanup;
    }

    ok = true;

cleanup:
    free(header_name);
    free(source_name);
    free(bin_name);
    free(header_path);
    free(source_path);
    free(bin_path);
    sb_free(&source);
    return ok;
}

bool build_one_config(const char *project_root, const char *json_path, const char *output_dir, char *error, size_t error_cap)
{
    FontConfig config;
    NormalizedCharset normalized;
    char *font_path = NULL;
    FT_Library library = NULL;
    FT_Face face = NULL;
    int line_height;
    int baseline;
    uint16_t max_box_width = 0;
    uint16_t max_box_height = 0;
    GlyphTable glyphs;
    ByteBuffer bitmaps;
    ByteBuffer blob;
    uint32_t blob_crc32 = 0;
    bool ok = false;

    memset(&config, 0, sizeof(config));
    memset(&normalized, 0, sizeof(normalized));
    memset(&glyphs, 0, sizeof(glyphs));
    memset(&bitmaps, 0, sizeof(bitmaps));
    memset(&blob, 0, sizeof(blob));
    error[0] = '\0';

    if (!ensure_directory_recursive(output_dir, error, error_cap) ||
        !parse_font_config_file(json_path, &config, error, error_cap)) {
        goto cleanup;
    }
    if (!normalize_charset(&config, &normalized, error, error_cap)) {
        goto cleanup;
    }

    font_path = resolve_font_path(&config, project_root);
    if (font_path == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }
    if (!file_exists(font_path)) {
        failf(error, error_cap, "cannot find font file '%s' for config '%s'", config.font_file, config.json_name);
        goto cleanup;
    }

    if (FT_Init_FreeType(&library) != 0) {
        failf(error, error_cap, "cannot initialize FreeType");
        goto cleanup;
    }
    if (FT_New_Face(library, font_path, config.face_index, &face) != 0) {
        failf(error, error_cap, "cannot open font '%s' (face_index=%d)", font_path, config.face_index);
        goto cleanup;
    }
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)config.font_size) != 0) {
        failf(error, error_cap, "cannot set pixel size %d for '%s'", config.font_size, font_path);
        goto cleanup;
    }

    line_height = round_26_6(face->size->metrics.height);
    baseline = round_26_6(face->size->metrics.ascender);
    if (line_height <= 0) {
        line_height = config.font_size;
    }
    if (baseline < 0) {
        baseline = 0;
    }

    if (!build_glyph_table(face, config.bpp, baseline, &normalized, &glyphs, &bitmaps, error, error_cap)) {
        goto cleanup;
    }
    collect_max_box_size(&glyphs, &max_box_width, &max_box_height);

    if (config.deploy_mode == DEPLOY_INTERNAL) {
        if (!emit_internal_mode(&config,
                                &normalized,
                                &glyphs,
                                &bitmaps,
                                line_height,
                                baseline,
                                max_box_width,
                                max_box_height,
                                output_dir,
                                error,
                                error_cap)) {
            goto cleanup;
        }
    } else {
        if (!build_external_blob(&glyphs, &bitmaps, &blob, &blob_crc32, error, error_cap) ||
            !emit_external_mode(&config,
                                &normalized,
                                line_height,
                                baseline,
                                max_box_width,
                                max_box_height,
                                &blob,
                                blob_crc32,
                                glyphs.count,
                                output_dir,
                                error,
                                error_cap)) {
            goto cleanup;
        }
    }

    ok = true;

cleanup:
    bb_free(&blob);
    bb_free(&bitmaps);
    glyph_table_free(&glyphs);
    if (face != NULL) {
        FT_Done_Face(face);
    }
    if (library != NULL) {
        FT_Done_FreeType(library);
    }
    free(font_path);
    free_normalized_charset(&normalized);
    free_font_config(&config);
    return ok;
}
