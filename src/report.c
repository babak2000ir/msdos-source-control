#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "crc32.h"
#include "dosgit.h"
#include "paths.h"
#include "report.h"
#include "treewalk.h"

static int hash_manifest_lookup(FILE *manifest, const char *relative_path,
                                uint32 *result)
{
    char line[MAX_PATH_LENGTH + 16];
    char *tab;
    char *hash_text;
    char *suffix;
    unsigned long hash;
    int index;
    int digit;

    if (fseek(manifest, 0L, SEEK_SET) != 0) {
        return -1;
    }

    while (fgets(line, sizeof(line), manifest) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(manifest)) {
            return -1;
        }
        tab = strchr(line, '\t');
        if (tab == NULL || tab == line) {
            return -1;
        }
        *tab = '\0';
        hash_text = tab + 1;
        hash = 0;
        for (index = 0; index < 8; ++index) {
            if (hash_text[index] == '\0' || hash_text[index] == '\r' ||
                hash_text[index] == '\n') {
                return -1;
            }
            if (hash_text[index] >= '0' && hash_text[index] <= '9') {
                digit = hash_text[index] - '0';
            } else if (hash_text[index] >= 'A' && hash_text[index] <= 'F') {
                digit = hash_text[index] - 'A' + 10;
            } else if (hash_text[index] >= 'a' && hash_text[index] <= 'f') {
                digit = hash_text[index] - 'a' + 10;
            } else {
                return -1;
            }
            hash = (hash << 4) | (unsigned long)digit;
        }
        suffix = hash_text + 8;
        if (*suffix == '\r') {
            ++suffix;
        }
        if (*suffix == '\n') {
            ++suffix;
        }
        if (*suffix != '\0') {
            return -1;
        }
        if (stricmp(line, relative_path) == 0) {
            *result = (uint32)hash;
            return 1;
        }
    }

    return ferror(manifest) ? -1 : 0;
}

/* Prints a status row and its previous/current hash values. */
static void print_file_status_line(const char *status, const char *relative_path,
                                   const uint32 *previous,
                                   const uint32 *current)
{
    char previous_hash[9];
    char current_hash[9];
    size_t path_length;
    int chunk_length;

    if (previous != NULL) {
        sprintf(previous_hash, "%08lX", *previous);
    } else {
        strcpy(previous_hash, "--------");
    }
    if (current != NULL) {
        sprintf(current_hash, "%08lX", *current);
    } else {
        strcpy(current_hash, "--------");
    }

    path_length = strlen(relative_path);
    chunk_length = path_length > FILE_LINE_WIDTH ? FILE_LINE_WIDTH :
                   (int)path_length;
    printf("%-9s %.*s\n", status, chunk_length, relative_path);
    relative_path += chunk_length;
    while (*relative_path != '\0') {
        path_length = strlen(relative_path);
        chunk_length = path_length > FILE_LINE_WIDTH ? FILE_LINE_WIDTH :
                       (int)path_length;
        printf("          %.*s\n", chunk_length, relative_path);
        relative_path += chunk_length;
    }
    printf("          %14s  %14s\n", previous_hash, current_hash);
}

/* ---- report_new_and_changed_files: callback + wrapper --------------- */

typedef struct {
    const char *snapshot_base;
    FILE *manifest;
} report_new_context_t;

static int report_new_or_changed_visitor(const char *full_path, const char *relative_path,
                                          void *context)
{
    report_new_context_t *report_context = (report_new_context_t *)context;
    struct stat snapshot_information;
    char snapshot_path[MAX_PATH_LENGTH];
    uint32 current_hash;
    uint32 previous_hash;
    int has_previous_hash;

    if (stricmp(relative_path, HASH_MANIFEST_NAME) == 0) {
        return WALK_OK;
    }

    if (!crc32_file(full_path, &current_hash)) {
        fprintf(stderr, "Cannot hash file: %s\n", full_path);
        return WALK_ERROR;
    }
    if (!build_child_path(snapshot_path, report_context->snapshot_base, relative_path)) {
        return WALK_ERROR;
    }

    if (stat(snapshot_path, &snapshot_information) != 0) {
        print_file_status_line("NEW", relative_path, NULL, &current_hash);
    } else {
        has_previous_hash = hash_manifest_lookup(report_context->manifest,
                                                 relative_path, &previous_hash);
        if (has_previous_hash < 0) {
            fprintf(stderr, "Invalid hash manifest.\n");
            return WALK_ERROR;
        }
        if (has_previous_hash == 0) {
            fprintf(stderr, "No hash found for snapshot file: %s\n", relative_path);
            return WALK_ERROR;
        }
        if (previous_hash != current_hash) {
            print_file_status_line("CHANGED", relative_path, &previous_hash,
                                   &current_hash);
        } else {
            print_file_status_line("UNCHANGED", relative_path, &previous_hash,
                                   &current_hash);
        }
    }
    return WALK_OK;
}

static int walk_report_files(const char *directory, const char *excluded_root,
                             walk_file_callback on_file, void *context)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes, excluded_root);
    callbacks.excluded_always = excluded_executable;
    callbacks.context = context;
    callbacks.on_file = on_file;

    return walk_tree(directory, "", 0, &callbacks) != WALK_ERROR;
}

/*
 * Walks the CURRENT (working) tree and reports every file as NEW (not
 * present in the snapshot) or CHANGED/UNCHANGED (present in the
 * snapshot too). Deleted files are NOT detected here — that's handled
 * separately by report_deleted_files(), which walks the snapshot
 * instead so it can see files that no longer exist on the current side.
 */
static int report_new_and_changed_files(const char *current_directory,
                                const char *snapshot_directory,
                                FILE *manifest,
                                int exclude_dosgit)
{
    report_new_context_t context;

    context.snapshot_base = snapshot_directory;
    context.manifest = manifest;
    return walk_report_files(current_directory,
                             exclude_dosgit ? DOSGIT_DIRECTORY : NULL,
                             report_new_or_changed_visitor, &context);
}

/* ---- report_deleted_files: callback + wrapper ------------------------ */

typedef struct {
    const char *current_base;
    FILE *manifest;
} report_deleted_context_t;

static int report_deleted_visitor(const char *full_path, const char *relative_path,
                                   void *context)
{
    report_deleted_context_t *report_context = (report_deleted_context_t *)context;
    struct stat current_information;
    struct stat snapshot_information;
    char current_path[MAX_PATH_LENGTH];
    uint32 previous_hash;
    int has_previous_hash;

    if (stricmp(relative_path, HASH_MANIFEST_NAME) == 0) {
        return WALK_OK;
    }

    if (!build_child_path(current_path, report_context->current_base, relative_path)) {
        return WALK_ERROR;
    }
    if (stat(current_path, &current_information) == 0) {
        return WALK_OK; /* still exists on the current side - nothing to report */
    }
    if (stat(full_path, &snapshot_information) != 0) {
        return WALK_ERROR;
    }
    has_previous_hash = hash_manifest_lookup(report_context->manifest,
                                             relative_path, &previous_hash);
    if (has_previous_hash < 0) {
        fprintf(stderr, "Invalid hash manifest.\n");
        return WALK_ERROR;
    }
    if (has_previous_hash == 0) {
        fprintf(stderr, "No hash found for snapshot file: %s\n", relative_path);
        return WALK_ERROR;
    }
    print_file_status_line("DELETED", relative_path, &previous_hash, NULL);
    return WALK_OK;
}

/*
 * Walks the SNAPSHOT tree and reports any file that no longer exists in
 * the current working tree as DELETED. This is the mirror image of
 * report_new_and_changed_files(): that function can't see deletions
 * because it only iterates the current tree, so a separate pass over
 * the snapshot is needed.
 */
static int report_deleted_files(const char *snapshot_directory,
                                const char *current_directory,
                                FILE *manifest,
                                int exclude_archive)
{
    report_deleted_context_t context;

    context.current_base = current_directory;
    context.manifest = manifest;
    return walk_report_files(snapshot_directory,
                             exclude_archive ? ARCHIVE_DIRECTORY : NULL,
                             report_deleted_visitor, &context);
}

/* Prints the full status report of `root` against the snapshot in `dosgit`. */
int report_status(const char *root, const char *dosgit)
{
    char manifest_path[MAX_PATH_LENGTH];
    FILE *manifest;
    int succeeded;

    if (!build_child_path(manifest_path, dosgit, HASH_MANIFEST_NAME)) {
        return 0;
    }
    manifest = fopen(manifest_path, "rb");
    if (manifest == NULL) {
        fprintf(stderr, "Cannot read hash manifest: %s. Commit again to create it.\n",
                manifest_path);
        return 0;
    }

    printf("STATUS    FILE\n");
    printf("          %-14s  %-14s\n", "PREVIOUS HASH", "CURRENT HASH");
    /* Two separate passes: current tree (new/changed/unchanged), then
     * snapshot tree (deleted) — see the comments on each function. */
    succeeded = report_new_and_changed_files(root, dosgit, manifest, 1);
    if (succeeded) {
        succeeded = report_deleted_files(dosgit, root, manifest, 1);
    }
    if (fclose(manifest) != 0) {
        succeeded = 0;
    }
    return succeeded;
}
