#ifndef FONT2C_H
#define FONT2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define FONT2C_VERSION "0.3.0"
#define ERROR_CAP 1024

typedef struct StringBuilder {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

typedef struct ByteBuffer {
    unsigned char *data;
    size_t length;
    size_t capacity;
} ByteBuffer;

typedef struct CodeRange {
    uint32_t start;
    uint32_t end;
} CodeRange;

typedef enum DeployMode {
    DEPLOY_INTERNAL,
    DEPLOY_EXTERNAL
} DeployMode;

typedef struct FontConfig {
    char *json_path;
    char *json_dir;
    char *json_name;
    char *base_name;
    char *symbol;
    char *font_file;
    int font_size;
    int face_index;
    int bpp;
    DeployMode deploy_mode;
    CodeRange *ranges;
    size_t range_count;
    char *chars_literal;
    uint32_t *char_codepoints;
    size_t char_count;
} FontConfig;

typedef enum CliMode {
    CLI_MODE_HELP,
    CLI_MODE_VERSION,
    CLI_MODE_BUILD_ONE,
    CLI_MODE_BUILD_ALL
} CliMode;

typedef struct CliOptions {
    CliMode mode;
    const char *input_path;
    const char *output_dir;
} CliOptions;

bool failf(char *error, size_t error_cap, const char *fmt, ...);
char *dup_string(const char *text);

bool sb_append_bytes(StringBuilder *sb, const char *bytes, size_t length, char *error, size_t error_cap);
bool sb_append_cstr(StringBuilder *sb, const char *text, char *error, size_t error_cap);
bool sb_append_char(StringBuilder *sb, char ch, char *error, size_t error_cap);
bool sb_append_format(StringBuilder *sb, char *error, size_t error_cap, const char *fmt, ...);
bool sb_append_utf8(StringBuilder *sb, uint32_t codepoint, char *error, size_t error_cap);
void sb_free(StringBuilder *sb);

bool bb_append(ByteBuffer *buffer, const unsigned char *bytes, size_t length, char *error, size_t error_cap);
bool bb_put_u8(ByteBuffer *buffer, uint8_t value, char *error, size_t error_cap);
bool bb_put_u16be(ByteBuffer *buffer, uint16_t value, char *error, size_t error_cap);
bool bb_put_u32be(ByteBuffer *buffer, uint32_t value, char *error, size_t error_cap);
void bb_free(ByteBuffer *buffer);

int ascii_casecmp(const char *lhs, const char *rhs);
bool is_scalar_value(uint32_t codepoint);

bool parse_cli(int argc, char **argv, CliOptions *options, char *error, size_t error_cap);
void print_usage(FILE *stream);

bool file_exists(const char *path);
bool is_directory_path(const char *path);
char *path_join(const char *left, const char *right);
char *path_dirname(const char *path);
char *path_stem(const char *filename);
const char *path_filename(const char *path);
bool ensure_directory_recursive(const char *path, char *error, size_t error_cap);
bool read_entire_file(const char *path, char **out_text, size_t *out_size, char *error, size_t error_cap);
bool write_binary_file(const char *path, const unsigned char *data, size_t size, char *error, size_t error_cap);
bool write_text_file(const char *path, const char *text, size_t size, char *error, size_t error_cap);
bool list_json_files(const char *directory, char ***out_files, size_t *out_count, char *error, size_t error_cap);
char *get_current_directory(char *error, size_t error_cap);

bool parse_font_config_file(const char *json_path, FontConfig *config, char *error, size_t error_cap);
void free_font_config(FontConfig *config);

bool build_one_config(const char *project_root, const char *json_path, const char *output_dir, char *error, size_t error_cap);

#endif
