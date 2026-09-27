#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "dosgit.h"
#include "paths.h"
#include "report.h"
#include "treewalk.h"

/* Formats a time_t as "YYYY-MM-DD HH:MM:SS" for the status report columns. */
static void format_timestamp(time_t value, char *result)
{
    struct tm *parts;

    parts = localtime(&value);
    if (parts == NULL) {
        strcpy(result, "-------------------");
        return;
    }
    strftime(result, 20, "%Y-%m-%d %H:%M:%S", parts);
}

/*
 * Prints one row of the status report: a status word (NEW/CHANGED/
 * UNCHANGED/DELETED), the file's path (wrapped across lines if it's
 * longer than FILE_LINE_WIDTH), and the previous vs. current
 * timestamp/size. Either `previous` or `current` may be NULL (file
 * doesn't exist on that side), in which case dashes and -1 are shown.
 */
static void print_file_status_line(const char *status, const char *relative_path,
                             const struct stat *previous,
                             const struct stat *current)
{
    char previous_date[20];
    char current_date[20];
    long previous_size;
    long current_size;
    size_t path_length;
    int chunk_length;

    if (previous != NULL) {
        format_timestamp(previous->st_mtime, previous_date);
        previous_size = previous->st_size;
    } else {
        strcpy(previous_date, "-------------------");
        previous_size = -1L;
    }
    if (current != NULL) {
        format_timestamp(current->st_mtime, current_date);
        current_size = current->st_size;
    } else {
        strcpy(current_date, "-------------------");
        current_size = -1L;
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
    printf("          %s %10ld  %s %10ld\n", previous_date,
           previous_size, current_date, current_size);
}

/* ---- report_new_and_changed_files: callback + wrapper --------------- */

typedef struct {
    const char *snapshot_base;
} report_new_context_t;

static int report_new_or_changed_visitor(const char *full_path, const char *relative_path,
                                          void *context)
{
    report_new_context_t *report_context = (report_new_context_t *)context;
    struct stat current_information;
    struct stat snapshot_information;
    char snapshot_path[MAX_PATH_LENGTH];

    if (stat(full_path, &current_information) != 0) {
        fprintf(stderr, "Cannot inspect file: %s\n", full_path);
        return WALK_ERROR;
    }
    if (!build_child_path(snapshot_path, report_context->snapshot_base, relative_path)) {
        return WALK_ERROR;
    }

    if (stat(snapshot_path, &snapshot_information) != 0) {
        print_file_status_line("NEW", relative_path, NULL, &current_information);
    } else if (snapshot_information.st_size != current_information.st_size ||
               snapshot_information.st_mtime != current_information.st_mtime) {
        /* Key decision point: "changed" is decided purely by size and
         * mtime, never by actual content comparison. A file edited
         * and saved back with the exact same size/timestamp (or one
         * merely touched without content changes) can be misreported. */
        print_file_status_line("CHANGED", relative_path, &snapshot_information,
                         &current_information);
    } else {
        print_file_status_line("UNCHANGED", relative_path, &snapshot_information,
                         &current_information);
    }
    return WALK_OK;
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
                                int exclude_dosgit)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];
    report_new_context_t context;

    context.snapshot_base = snapshot_directory;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes,
                                     exclude_dosgit ? DOSGIT_DIRECTORY : NULL);
    callbacks.excluded_always = excluded_executable;
    callbacks.context = &context;
    callbacks.on_file = report_new_or_changed_visitor;

    return walk_tree(current_directory, "", 0, &callbacks) != WALK_ERROR;
}

/* ---- report_deleted_files: callback + wrapper ------------------------ */

typedef struct {
    const char *current_base;
} report_deleted_context_t;

static int report_deleted_visitor(const char *full_path, const char *relative_path,
                                   void *context)
{
    report_deleted_context_t *report_context = (report_deleted_context_t *)context;
    struct stat current_information;
    struct stat snapshot_information;
    char current_path[MAX_PATH_LENGTH];

    if (!build_child_path(current_path, report_context->current_base, relative_path)) {
        return WALK_ERROR;
    }
    if (stat(current_path, &current_information) == 0) {
        return WALK_OK; /* still exists on the current side - nothing to report */
    }
    if (stat(full_path, &snapshot_information) != 0) {
        return WALK_ERROR;
    }
    print_file_status_line("DELETED", relative_path, &snapshot_information, NULL);
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
                                int exclude_archive)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];
    report_deleted_context_t context;

    context.current_base = current_directory;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes,
                                     exclude_archive ? ARCHIVE_DIRECTORY : NULL);
    callbacks.excluded_always = excluded_executable;
    callbacks.context = &context;
    callbacks.on_file = report_deleted_visitor;

    return walk_tree(snapshot_directory, "", 0, &callbacks) != WALK_ERROR;
}

/* Prints the full status report of `root` against the snapshot in `dosgit`. */
int report_status(const char *root, const char *dosgit)
{
    printf("STATUS    FILE\n");
    printf("          %-19s %10s  %-19s %10s\n", "PREVIOUS DATE",
           "PREV SIZE", "NEW DATE", "NEW SIZE");
    /* Two separate passes: current tree (new/changed/unchanged), then
     * snapshot tree (deleted) — see the comments on each function. */
    if (!report_new_and_changed_files(root, dosgit, 1)) {
        return 0;
    }
    return report_deleted_files(dosgit, root, 1);
}
