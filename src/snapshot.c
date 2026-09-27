#include <stdio.h>
#include <time.h>

#include "dosgit.h"
#include "paths.h"
#include "snapshot.h"
#include "treeops.h"

/*
 * Archive folder name is HHMMSS.DDD (hour/min/sec + day-of-year),
 * giving each commit within the same day a distinct, sortable
 * name without needing the full date. Fits DOS 8.3 naming.
 */
void make_archive_name(const struct tm *parts, char name[13])
{
    sprintf(name, "%02d%02d%02d.%03d", parts->tm_hour,
            parts->tm_min, parts->tm_sec, parts->tm_yday + 1);
}

/*
 * Performs a "commit": archives the existing snapshot (if any), then
 * replaces the snapshot with a fresh copy of the current working tree.
 */
int commit_snapshot(const char *root, const char *dosgit)
{
    char archive[MAX_PATH_LENGTH];
    char archive_name[13];
    char archive_path[MAX_PATH_LENGTH];
    time_t now;
    struct tm *parts;
    int has_previous_files;

    if (!create_directory_if_missing(dosgit) ||
        !build_child_path(archive, dosgit, ARCHIVE_DIRECTORY) ||
        !create_directory_if_missing(archive)) {
        return 0;
    }

    /* Decision point: only archive the old snapshot if it actually held
     * any files. On the very first commit there's nothing to preserve. */
    has_previous_files = directory_has_any_file(dosgit, 1);
    if (has_previous_files < 0) {
        fprintf(stderr, "Cannot inspect previous snapshot: %s\n", dosgit);
        return 0;
    }
    if (has_previous_files > 0) {
        now = time(NULL);
        parts = localtime(&now);
        if (parts == NULL) {
            fprintf(stderr, "Cannot determine the archive time.\n");
            return 0;
        }
        make_archive_name(parts, archive_name);
        if (!build_child_path(archive_path, archive, archive_name) ||
            path_is_directory(archive_path)) {
            fprintf(stderr, "Archive already exists: %s\n", archive_path);
            return 0;
        }
        printf("Archiving previous commit to %s\n", archive_path);
        /* The ENTIRE old snapshot is copied into the archive (a full
         * copy, not a diff), excluding the archive folder itself and the
         * executable so the archive never nests inside itself. */
        if (!copy_directory_recursive(dosgit, archive_path, ARCHIVE_DIRECTORY,
                   EXECUTABLE_NAME)) {
            return 0;
        }
    }

    /* Wipe the old snapshot contents (keeping the archive folder), then
     * repopulate it from the current working tree so it becomes the
     * new baseline for future comparisons. */
    if (!delete_directory_contents_recursive(dosgit, ARCHIVE_DIRECTORY)) {
        return 0;
    }
    printf("Copying files into %s\n", dosgit);
    if (!copy_directory_recursive(root, dosgit, DOSGIT_DIRECTORY, EXECUTABLE_NAME)) {
        return 0;
    }
    printf("Commit complete.\n");
    return 1;
}
