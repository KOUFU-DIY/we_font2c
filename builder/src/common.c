#include "font2c.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define PATH_SEP '\\'
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

static bool sb_reserve(StringBuilder *sb, size_t extra, char *error, size_t error_cap)
{
    size_t required = sb->length + extra + 1;
    size_t new_capacity;
    char *new_data;

    if (required <= sb->capacity) {
        return true;
    }

    new_capacity = sb->capacity == 0 ? 128 : sb->capacity;
    while (new_capacity < required) {
        if (new_capacity > (size_t)-1 / 2) {
            return failf(error, error_cap, "string builder capacity overflow");
        }
        new_capacity *= 2;
    }

    new_data = (char *)realloc(sb->data, new_capacity);
    if (new_data == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    sb->data = new_data;
    sb->capacity = new_capacity;
    return true;
}

static bool bb_reserve(ByteBuffer *buffer, size_t extra, char *error, size_t error_cap)
{
    size_t required = buffer->length + extra;
    size_t new_capacity;
    unsigned char *new_data;

    if (required <= buffer->capacity) {
        return true;
    }

    new_capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (new_capacity < required) {
        if (new_capacity > (size_t)-1 / 2) {
            return failf(error, error_cap, "byte buffer capacity overflow");
        }
        new_capacity *= 2;
    }

    new_data = (unsigned char *)realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return true;
}

static bool path_is_sep(char ch)
{
    return ch == '/' || ch == '\\';
}

static bool path_is_absolute(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (path_is_sep(path[0])) {
        return true;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static int compare_cstrings(const void *lhs, const void *rhs)
{
    const char *const *a = (const char *const *)lhs;
    const char *const *b = (const char *const *)rhs;
    return strcmp(*a, *b);
}

static int make_directory_once(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

static bool has_json_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && ascii_casecmp(dot, ".json") == 0;
}

static bool append_json_file(char ***items, size_t *count, size_t *capacity, char *path, char *error, size_t error_cap)
{
    char **new_items;
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);
        new_items = (char **)realloc(*items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[(*count)++] = path;
    return true;
}

bool failf(char *error, size_t error_cap, const char *fmt, ...)
{
    va_list args;
    if (error_cap == 0) {
        return false;
    }
    va_start(args, fmt);
    vsnprintf(error, error_cap, fmt, args);
    va_end(args);
    error[error_cap - 1] = '\0';
    return false;
}

char *dup_string(const char *text)
{
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length + 1);
    return copy;
}

bool sb_append_bytes(StringBuilder *sb, const char *bytes, size_t length, char *error, size_t error_cap)
{
    if (!sb_reserve(sb, length, error, error_cap)) {
        return false;
    }
    memcpy(sb->data + sb->length, bytes, length);
    sb->length += length;
    sb->data[sb->length] = '\0';
    return true;
}

bool sb_append_cstr(StringBuilder *sb, const char *text, char *error, size_t error_cap)
{
    return sb_append_bytes(sb, text, strlen(text), error, error_cap);
}

bool sb_append_char(StringBuilder *sb, char ch, char *error, size_t error_cap)
{
    return sb_append_bytes(sb, &ch, 1, error, error_cap);
}

bool sb_append_format(StringBuilder *sb, char *error, size_t error_cap, const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;

    if (!sb_reserve(sb, 64, error, error_cap)) {
        return false;
    }

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(sb->data + sb->length, sb->capacity - sb->length, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return failf(error, error_cap, "formatting failure");
    }
    if ((size_t)needed >= sb->capacity - sb->length) {
        if (!sb_reserve(sb, (size_t)needed, error, error_cap)) {
            va_end(args);
            return false;
        }
        needed = vsnprintf(sb->data + sb->length, sb->capacity - sb->length, fmt, args);
        if (needed < 0) {
            va_end(args);
            return failf(error, error_cap, "formatting failure");
        }
    }
    sb->length += (size_t)needed;
    va_end(args);
    return true;
}

bool sb_append_utf8(StringBuilder *sb, uint32_t codepoint, char *error, size_t error_cap)
{
    char bytes[4];
    size_t length = 0;

    if (codepoint <= 0x7F) {
        bytes[0] = (char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7FF) {
        bytes[0] = (char)(0xC0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3F));
        length = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = (char)(0xE0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (codepoint & 0x3F));
        length = 3;
    } else if (codepoint <= 0x10FFFF) {
        bytes[0] = (char)(0xF0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (codepoint & 0x3F));
        length = 4;
    } else {
        return failf(error, error_cap, "invalid Unicode codepoint U+%X", codepoint);
    }

    return sb_append_bytes(sb, bytes, length, error, error_cap);
}

void sb_free(StringBuilder *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->length = 0;
    sb->capacity = 0;
}

bool bb_append(ByteBuffer *buffer, const unsigned char *bytes, size_t length, char *error, size_t error_cap)
{
    if (length == 0) {
        return true;
    }
    if (!bb_reserve(buffer, length, error, error_cap)) {
        return false;
    }
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    return true;
}

bool bb_put_u8(ByteBuffer *buffer, uint8_t value, char *error, size_t error_cap)
{
    return bb_append(buffer, &value, 1, error, error_cap);
}

bool bb_put_u16be(ByteBuffer *buffer, uint16_t value, char *error, size_t error_cap)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value >> 8);
    bytes[1] = (unsigned char)(value & 0xFF);
    return bb_append(buffer, bytes, 2, error, error_cap);
}

bool bb_put_u32be(ByteBuffer *buffer, uint32_t value, char *error, size_t error_cap)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)((value >> 16) & 0xFF);
    bytes[2] = (unsigned char)((value >> 8) & 0xFF);
    bytes[3] = (unsigned char)(value & 0xFF);
    return bb_append(buffer, bytes, 4, error, error_cap);
}

void bb_free(ByteBuffer *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

int ascii_casecmp(const char *lhs, const char *rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        unsigned char a = (unsigned char)tolower((unsigned char)*lhs);
        unsigned char b = (unsigned char)tolower((unsigned char)*rhs);
        if (a != b) {
            return (int)a - (int)b;
        }
        ++lhs;
        ++rhs;
    }
    return (int)(unsigned char)tolower((unsigned char)*lhs) - (int)(unsigned char)tolower((unsigned char)*rhs);
}

bool is_scalar_value(uint32_t codepoint)
{
    if (codepoint > 0x10FFFF) {
        return false;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        return false;
    }
    return true;
}

bool utf8_decode_next(const char *text, size_t length, size_t *offset, uint32_t *out_codepoint, char *error, size_t error_cap)
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

const char *path_filename(const char *path)
{
    const char *cursor = path;
    const char *result = path;
    while (*cursor != '\0') {
        if (path_is_sep(*cursor)) {
            result = cursor + 1;
        }
        ++cursor;
    }
    return result;
}

char *path_dirname(const char *path)
{
    const char *last_sep = NULL;
    const char *cursor = path;
    size_t length;
    char *result;

    while (*cursor != '\0') {
        if (path_is_sep(*cursor)) {
            last_sep = cursor;
        }
        ++cursor;
    }
    if (last_sep == NULL) {
        return dup_string(".");
    }

    length = (size_t)(last_sep - path);
    if (length == 0) {
        return dup_string("/");
    }
    if (length == 2 && path[1] == ':') {
        length = 3;
    }

    result = (char *)malloc(length + 1);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, path, length);
    result[length] = '\0';
    return result;
}

char *path_stem(const char *filename)
{
    const char *base = path_filename(filename);
    const char *last_dot = strrchr(base, '.');
    size_t length;
    char *result;

    if (last_dot == NULL || last_dot == base) {
        return dup_string(base);
    }

    length = (size_t)(last_dot - base);
    result = (char *)malloc(length + 1);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, base, length);
    result[length] = '\0';
    return result;
}

char *path_join(const char *left, const char *right)
{
    size_t left_length;
    size_t right_length;
    bool add_sep;
    char *result;

    if (path_is_absolute(right)) {
        return dup_string(right);
    }

    left_length = strlen(left);
    right_length = strlen(right);
    add_sep = left_length > 0 && !path_is_sep(left[left_length - 1]);

    result = (char *)malloc(left_length + (add_sep ? 1 : 0) + right_length + 1);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, left, left_length);
    if (add_sep) {
        result[left_length] = PATH_SEP;
        memcpy(result + left_length + 1, right, right_length);
        result[left_length + 1 + right_length] = '\0';
    } else {
        memcpy(result + left_length, right, right_length);
        result[left_length + right_length] = '\0';
    }
    return result;
}

bool file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

bool is_directory_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return (st.st_mode & S_IFDIR) != 0;
}

bool ensure_directory_recursive(const char *path, char *error, size_t error_cap)
{
    char *mutable_path;
    size_t i;
    size_t start = 0;

    if (path == NULL || path[0] == '\0') {
        return failf(error, error_cap, "output directory is empty");
    }
    if (is_directory_path(path)) {
        return true;
    }

    mutable_path = dup_string(path);
    if (mutable_path == NULL) {
        return failf(error, error_cap, "out of memory");
    }

    for (i = 0; mutable_path[i] != '\0'; ++i) {
        if (path_is_sep(mutable_path[i])) {
            mutable_path[i] = PATH_SEP;
        }
    }

    if (isalpha((unsigned char)mutable_path[0]) && mutable_path[1] == ':') {
        start = 2;
        if (mutable_path[2] == PATH_SEP) {
            start = 3;
        }
    } else if (mutable_path[0] == PATH_SEP) {
        start = 1;
    }

    for (i = start; mutable_path[i] != '\0'; ++i) {
        if (mutable_path[i] == PATH_SEP) {
            char saved = mutable_path[i];
            mutable_path[i] = '\0';
            if (mutable_path[0] != '\0' && !is_directory_path(mutable_path)) {
                if (make_directory_once(mutable_path) != 0 && errno != EEXIST) {
                    int saved_errno = errno;
                    free(mutable_path);
                    return failf(error, error_cap, "cannot create directory '%s': %s", path, strerror(saved_errno));
                }
            }
            mutable_path[i] = saved;
        }
    }

    if (!is_directory_path(mutable_path)) {
        if (make_directory_once(mutable_path) != 0 && errno != EEXIST) {
            int saved_errno = errno;
            free(mutable_path);
            return failf(error, error_cap, "cannot create directory '%s': %s", path, strerror(saved_errno));
        }
    }

    free(mutable_path);
    return true;
}

bool read_entire_file(const char *path, char **out_text, size_t *out_size, char *error, size_t error_cap)
{
    FILE *fp;
    long size;
    char *data;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return failf(error, error_cap, "cannot open '%s': %s", path, strerror(errno));
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return failf(error, error_cap, "cannot seek '%s'", path);
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return failf(error, error_cap, "cannot determine size of '%s'", path);
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return failf(error, error_cap, "cannot seek '%s'", path);
    }

    data = (char *)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(fp);
        return failf(error, error_cap, "out of memory");
    }

    if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return failf(error, error_cap, "cannot read '%s'", path);
    }

    data[size] = '\0';
    fclose(fp);

    *out_text = data;
    if (out_size != NULL) {
        *out_size = (size_t)size;
    }
    return true;
}

bool write_binary_file(const char *path, const unsigned char *data, size_t size, char *error, size_t error_cap)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return failf(error, error_cap, "cannot write '%s': %s", path, strerror(errno));
    }
    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return failf(error, error_cap, "cannot write '%s'", path);
    }
    if (fclose(fp) != 0) {
        return failf(error, error_cap, "cannot close '%s'", path);
    }
    return true;
}

bool write_text_file(const char *path, const char *text, size_t size, char *error, size_t error_cap)
{
    return write_binary_file(path, (const unsigned char *)text, size, error, error_cap);
}

bool list_json_files(const char *directory, char ***out_files, size_t *out_count, char *error, size_t error_cap)
{
    DIR *dir;
    struct dirent *entry;
    char **items = NULL;
    size_t count = 0;
    size_t capacity = 0;

    dir = opendir(directory);
    if (dir == NULL) {
        return failf(error, error_cap, "cannot open directory '%s': %s", directory, strerror(errno));
    }

    while ((entry = readdir(dir)) != NULL) {
        char *joined;
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!has_json_extension(entry->d_name)) {
            continue;
        }

        joined = path_join(directory, entry->d_name);
        if (joined == NULL) {
            closedir(dir);
            return failf(error, error_cap, "out of memory");
        }

        if (stat(joined, &st) == 0 && (st.st_mode & S_IFREG) != 0) {
            if (!append_json_file(&items, &count, &capacity, joined, error, error_cap)) {
                free(joined);
                closedir(dir);
                return false;
            }
        } else {
            free(joined);
        }
    }

    closedir(dir);
    qsort(items, count, sizeof(*items), compare_cstrings);
    *out_files = items;
    *out_count = count;
    return true;
}

char *get_current_directory(char *error, size_t error_cap)
{
    size_t size = 1024;
    while (size <= 32768) {
        char *buffer = (char *)malloc(size);
        if (buffer == NULL) {
            return NULL;
        }
#ifdef _WIN32
        if (_getcwd(buffer, (int)size) != NULL) {
#else
        if (getcwd(buffer, size) != NULL) {
#endif
            return buffer;
        }
        free(buffer);
        size *= 2;
    }
    failf(error, error_cap, "cannot determine current directory");
    return NULL;
}

void print_usage(FILE *stream)
{
    fprintf(stream,
            "font2c %s\n"
            "Usage:\n"
            "  font2c build <config.json> [-o <output_dir>]\n"
            "  font2c build-all <input_dir> [-o <output_dir>]\n"
            "  font2c scan --src <dir> [--src <dir> ...] [scan options]\n"
            "  font2c --help\n"
            "  font2c --version\n"
            "\n"
            "Scan options:\n"
            "  --ext <list>     comma-separated source extensions (default: c,h)\n"
            "  --extra <file>   UTF-8 text file with extra chars, repeatable\n"
            "  --json <config>  merge scanned chars into the config's charset.chars\n"
            "  --prune          with --json, drop non-ASCII chars not seen in the scan\n"
            "  --out <file>     write scanned chars to a UTF-8 text file\n"
            "  --dry-run        report only, write nothing\n"
            "\n"
            "With no arguments, font2c runs:\n"
            "  font2c build-all input -o output\n",
            FONT2C_VERSION);
}

static bool append_cli_string(const char ***items, size_t *count, size_t *capacity, const char *value, char *error, size_t error_cap)
{
    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);
        const char **new_items = (const char **)realloc((void *)*items, new_capacity * sizeof(*new_items));
        if (new_items == NULL) {
            return failf(error, error_cap, "out of memory");
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[(*count)++] = value;
    return true;
}

void free_cli_options(CliOptions *options)
{
    free((void *)options->scan_srcs);
    free((void *)options->scan_extras);
    options->scan_srcs = NULL;
    options->scan_src_count = 0;
    options->scan_extras = NULL;
    options->scan_extra_count = 0;
}

static bool parse_scan_cli(int argc, char **argv, CliOptions *options, char *error, size_t error_cap)
{
    int argi = 2;
    size_t src_capacity = 0;
    size_t extra_capacity = 0;

    options->mode = CLI_MODE_SCAN;
    options->scan_ext_list = "c,h";

    while (argi < argc) {
        const char *arg = argv[argi];
        if (strcmp(arg, "--src") == 0) {
            if (argi + 1 >= argc) {
                return failf(error, error_cap, "missing value after --src");
            }
            if (!append_cli_string(&options->scan_srcs, &options->scan_src_count, &src_capacity, argv[argi + 1], error, error_cap)) {
                return false;
            }
            argi += 2;
        } else if (strcmp(arg, "--extra") == 0) {
            if (argi + 1 >= argc) {
                return failf(error, error_cap, "missing value after --extra");
            }
            if (!append_cli_string(&options->scan_extras, &options->scan_extra_count, &extra_capacity, argv[argi + 1], error, error_cap)) {
                return false;
            }
            argi += 2;
        } else if (strcmp(arg, "--ext") == 0) {
            if (argi + 1 >= argc) {
                return failf(error, error_cap, "missing value after --ext");
            }
            options->scan_ext_list = argv[argi + 1];
            argi += 2;
        } else if (strcmp(arg, "--json") == 0) {
            if (argi + 1 >= argc) {
                return failf(error, error_cap, "missing value after --json");
            }
            options->scan_json = argv[argi + 1];
            argi += 2;
        } else if (strcmp(arg, "--out") == 0) {
            if (argi + 1 >= argc) {
                return failf(error, error_cap, "missing value after --out");
            }
            options->scan_out = argv[argi + 1];
            argi += 2;
        } else if (strcmp(arg, "--prune") == 0) {
            options->scan_prune = true;
            ++argi;
        } else if (strcmp(arg, "--dry-run") == 0) {
            options->scan_dry_run = true;
            ++argi;
        } else {
            return failf(error, error_cap, "unexpected argument '%s'", arg);
        }
    }

    if (options->scan_src_count == 0) {
        return failf(error, error_cap, "scan requires at least one --src <dir>");
    }
    if (options->scan_prune && options->scan_json == NULL) {
        return failf(error, error_cap, "--prune requires --json");
    }
    return true;
}

bool parse_cli(int argc, char **argv, CliOptions *options, char *error, size_t error_cap)
{
    int argi = 1;
    memset(options, 0, sizeof(*options));

    if (argc == 1) {
        options->mode = CLI_MODE_BUILD_ALL;
        options->input_path = "input";
        options->output_dir = "output";
        return true;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
        options->mode = CLI_MODE_HELP;
        return true;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        options->mode = CLI_MODE_VERSION;
        return true;
    }

    if (strcmp(argv[1], "scan") == 0) {
        return parse_scan_cli(argc, argv, options, error, error_cap);
    }

    if (strcmp(argv[1], "build") == 0) {
        options->mode = CLI_MODE_BUILD_ONE;
    } else if (strcmp(argv[1], "build-all") == 0) {
        options->mode = CLI_MODE_BUILD_ALL;
    } else {
        return failf(error, error_cap, "unknown command '%s'", argv[1]);
    }

    ++argi;
    if (argi >= argc) {
        return failf(error, error_cap, "missing input path");
    }
    options->input_path = argv[argi++];
    options->output_dir = "output";

    while (argi < argc) {
        if (strcmp(argv[argi], "-o") == 0 || strcmp(argv[argi], "--output") == 0) {
            ++argi;
            if (argi >= argc) {
                return failf(error, error_cap, "missing value after -o");
            }
            options->output_dir = argv[argi++];
        } else {
            return failf(error, error_cap, "unexpected argument '%s'", argv[argi]);
        }
    }

    return true;
}
