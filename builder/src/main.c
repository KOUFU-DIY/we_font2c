#include "font2c.h"

#include <stdio.h>
#include <stdlib.h>

static bool ensure_workspace_layout(const char *project_root, char *error, size_t error_cap)
{
    char *fonts_dir = path_join(project_root, "fonts");
    char *input_dir = path_join(project_root, "input");
    char *output_dir = path_join(project_root, "output");
    bool ok = false;

    if (fonts_dir == NULL || input_dir == NULL || output_dir == NULL) {
        failf(error, error_cap, "out of memory");
        goto cleanup;
    }

    if (!ensure_directory_recursive(fonts_dir, error, error_cap) ||
        !ensure_directory_recursive(input_dir, error, error_cap) ||
        !ensure_directory_recursive(output_dir, error, error_cap)) {
        goto cleanup;
    }

    ok = true;

cleanup:
    free(fonts_dir);
    free(input_dir);
    free(output_dir);
    return ok;
}

int main(int argc, char **argv)
{
    CliOptions options;
    char error[ERROR_CAP];
    char *project_root = NULL;
    int exit_code = 1;

    if (!parse_cli(argc, argv, &options, error, sizeof(error))) {
        fprintf(stderr, "error: %s\n\n", error);
        print_usage(stderr);
        return 1;
    }

    if (options.mode == CLI_MODE_HELP) {
        print_usage(stdout);
        return 0;
    }
    if (options.mode == CLI_MODE_VERSION) {
        printf("%s\n", FONT2C_VERSION);
        return 0;
    }

    project_root = get_current_directory(error, sizeof(error));
    if (project_root == NULL) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    if (!ensure_workspace_layout(project_root, error, sizeof(error))) {
        fprintf(stderr, "error: %s\n", error);
        goto cleanup;
    }

    if (options.mode == CLI_MODE_BUILD_ONE) {
        if (!build_one_config(project_root, options.input_path, options.output_dir, error, sizeof(error))) {
            fprintf(stderr, "error: %s\n", error);
            goto cleanup;
        }
        printf("built %s -> %s\n", options.input_path, options.output_dir);
    } else {
        char **files = NULL;
        size_t count = 0;
        size_t i;

        if (!list_json_files(options.input_path, &files, &count, error, sizeof(error))) {
            fprintf(stderr, "error: %s\n", error);
            goto cleanup;
        }
        if (count == 0) {
            fprintf(stderr, "error: no .json files found in %s\n", options.input_path);
            free(files);
            goto cleanup;
        }

        for (i = 0; i < count; ++i) {
            if (!build_one_config(project_root, files[i], options.output_dir, error, sizeof(error))) {
                fprintf(stderr, "error: %s\n", error);
                while (i < count) {
                    free(files[i]);
                    ++i;
                }
                free(files);
                goto cleanup;
            }
            printf("built %s -> %s\n", files[i], options.output_dir);
            free(files[i]);
        }
        free(files);
    }

    exit_code = 0;

cleanup:
    free(project_root);
    return exit_code;
}
