#include <dos.h>
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_PATH_LENGTH 260
#define COPY_BUFFER_SIZE 16384
#define DOSGIT_DIRECTORY "dosgit"
#define ARCHIVE_DIRECTORY "archive"
#define EXECUTABLE_NAME "git.exe"
#define FILE_LINE_WIDTH 69

/*
 * Joins `right` onto `left` with a single backslash separator, writing
 * the result into `result`. Used everywhere a child path needs to be
 * built from a directory + entry name.
 */
static int build_child_path(char *result, const char *left, const char *right)
{
    size_t left_length;

    left_length = strlen(left);
    if (left_length + strlen(right) + 2 > MAX_PATH_LENGTH) {
        fprintf(stderr, "Path is too long: %s\\%s\n", left, right);
        return 0;
    }

    strcpy(result, left);
    if (left_length > 0 && left[left_length - 1] != '\\' &&
        left[left_length - 1] != '/') {
        strcat(result, "\\");
    }
    strcat(result, right);
    return 1;
}

/* True if `path` exists and is a directory. */
static int path_is_directory(const char *path)
{
    struct stat information;

    return stat(path, &information) == 0 &&
           (information.st_mode & S_IFDIR) != 0;
}

/* Creates `path` as a directory if it doesn't already exist as one. */
static int create_directory_if_missing(const char *path)
{
    if (path_is_directory(path)) {
        return 1;
    }
    if (mkdir(path) == 0) {
        return 1;
    }
    fprintf(stderr, "Cannot create directory: %s\n", path);
    return 0;
}

/* True for the "." and ".." pseudo-entries every directory listing returns. */
static int is_dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/*
 * Starts a directory listing of `directory` into `entry` (DOS-style
 * find-first/find-next). Matches all attribute types (normal, read-only,
 * hidden, system, subdirectory) so nothing is silently skipped.
 */
static int find_first_entry(const char *directory, struct find_t *entry)
{
    char pattern[MAX_PATH_LENGTH];

    if (!build_child_path(pattern, directory, "*.*")) {
        return 0;
    }
    return _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_HIDDEN |
                          _A_SYSTEM | _A_SUBDIR, entry) == 0;
}

/*
 * Copies a single file's bytes and preserves its DOS timestamp on the
 * destination. This timestamp preservation is what later lets the
 * comparison functions trust st_mtime as a proxy for "file changed".
 */
static int copy_file_contents(const char *source, const char *destination)
{
    FILE *input;
    FILE *output;
    char *buffer;
    size_t bytes_read;
    int succeeded;
    unsigned file_date;
    unsigned file_time;

    input = fopen(source, "rb");
    if (input == NULL) {
        fprintf(stderr, "Cannot read file: %s\n", source);
        return 0;
    }

    output = fopen(destination, "wb");
    if (output == NULL) {
        fprintf(stderr, "Cannot write file: %s\n", destination);
        fclose(input);
        return 0;
    }

    buffer = (char *)malloc(COPY_BUFFER_SIZE);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to copy: %s\n", source);
        fclose(output);
        fclose(input);
        return 0;
    }

    succeeded = 1;
    while ((bytes_read = fread(buffer, 1, COPY_BUFFER_SIZE, input)) != 0) {
        if (fwrite(buffer, 1, bytes_read, output) != bytes_read) {
            fprintf(stderr, "Write failed: %s\n", destination);
            succeeded = 0;
            break;
        }
    }
    if (ferror(input)) {
        fprintf(stderr, "Read failed: %s\n", source);
        succeeded = 0;
    }

    /* Copy the original mtime onto the new file so future comparisons
     * against this copy see it as identical rather than "changed". */
    if (succeeded && _dos_getftime(fileno(input), &file_date, &file_time) == 0) {
        fflush(output);
        _dos_setftime(fileno(output), file_date, file_time);
    }

    free(buffer);
    if (fclose(output) != 0) {
        succeeded = 0;
    }
    fclose(input);
    return succeeded;
}

/*
 * Recursively copies every file and subdirectory from `source` into
 * `destination`.
 *
 * `excluded_root_directory` / `excluded_file_name`: names to skip at this
 * level only (e.g. don't copy the "dosgit" snapshot folder or the
 * "git.exe" binary into a fresh snapshot). The exclusion is passed as
 * NULL on recursive calls so it only applies at the top level of the
 * tree being copied, not to every same-named entry found deeper down.
 */
static int copy_directory_recursive(const char *source, const char *destination,
                     const char *excluded_root_directory,
                     const char *excluded_file_name)
{
    struct find_t entry;
    char source_path[MAX_PATH_LENGTH];
    char destination_path[MAX_PATH_LENGTH];
    int result;

    if (!create_directory_if_missing(destination)) {
        return 0;
    }
    if (!find_first_entry(source, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_entry(entry.name) ||
            (excluded_root_directory != NULL &&
             stricmp(entry.name, excluded_root_directory) == 0) ||
            (excluded_file_name != NULL &&
             stricmp(entry.name, excluded_file_name) == 0)) {
            continue;
        }
        if (!build_child_path(source_path, source, entry.name) ||
            !build_child_path(destination_path, destination, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            /* Note: excluded_root_directory is deliberately NOT passed
             * down, so a same-named directory nested deeper is copied
             * normally; only the top-level exclusion is meant to apply. */
            if (!copy_directory_recursive(source_path, destination_path, NULL,
                           excluded_file_name)) {
                result = 0;
            }
        } else if (!copy_file_contents(source_path, destination_path)) {
            result = 0;
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

/*
 * Recursively deletes everything inside `directory` (but not the
 * directory itself), optionally leaving one top-level subdirectory
 * (`excluded_root_directory`) untouched. Used to clear out the old
 * snapshot while keeping its "archive" folder intact.
 */
static int delete_directory_contents_recursive(const char *directory,
                                const char *excluded_root_directory)
{
    struct find_t entry;
    char path[MAX_PATH_LENGTH];
    int result;

    if (!find_first_entry(directory, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_entry(entry.name) ||
            (excluded_root_directory != NULL &&
             stricmp(entry.name, excluded_root_directory) == 0)) {
            continue;
        }
        if (!build_child_path(path, directory, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!delete_directory_contents_recursive(path, NULL) || rmdir(path) != 0) {
                fprintf(stderr, "Cannot remove directory: %s\n", path);
                result = 0;
            }
        } else {
            /* Clear read-only attribute first; otherwise remove() on a
             * read-only file (e.g. one copied with its original
             * attributes) would fail. */
            chmod(path, S_IWRITE | S_IREAD);
            if (remove(path) != 0) {
                fprintf(stderr, "Cannot remove file: %s\n", path);
                result = 0;
            }
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

/*
 * True if `directory` contains at least one real file anywhere below it.
 * Used to decide whether there's anything worth archiving/comparing yet
 * (an empty or brand-new snapshot folder doesn't need either).
 * `exclude_archive`, like the exclusion parameters above, only applies
 * at the top level of the recursion (passed as 0 on recursive calls).
 */
static int directory_has_any_file(const char *directory, int exclude_archive)
{
    struct find_t entry;
    char path[MAX_PATH_LENGTH];

    if (!find_first_entry(directory, &entry)) {
        return 0;
    }

    do {
        if (is_dot_entry(entry.name) ||
            (exclude_archive && stricmp(entry.name, ARCHIVE_DIRECTORY) == 0) ||
            stricmp(entry.name, EXECUTABLE_NAME) == 0) {
            continue;
        }
        if ((entry.attrib & _A_SUBDIR) == 0) {
            return 1;
        }
        if (build_child_path(path, directory, entry.name) &&
            directory_has_any_file(path, 0)) {
            return 1;
        }
    } while (_dos_findnext(&entry) == 0);

    return 0;
}

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

/*
 * Walks the CURRENT (working) tree and reports every file as NEW (not
 * present in the snapshot) or CHANGED/UNCHANGED (present in the
 * snapshot too). Deleted files are NOT detected here — that's handled
 * separately by report_deleted_files(), which walks the snapshot
 * instead so it can see files that no longer exist on the current side.
 */
static int report_new_and_changed_files(const char *current_directory,
                                const char *snapshot_directory,
                                const char *relative_directory,
                                int exclude_dosgit)
{
    struct find_t entry;
    struct stat current_information;
    struct stat snapshot_information;
    char current_path[MAX_PATH_LENGTH];
    char snapshot_path[MAX_PATH_LENGTH];
    char relative_path[MAX_PATH_LENGTH];
    int result;

    if (!find_first_entry(current_directory, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_entry(entry.name) ||
            (exclude_dosgit && stricmp(entry.name, DOSGIT_DIRECTORY) == 0) ||
            stricmp(entry.name, EXECUTABLE_NAME) == 0) {
            continue;
        }
        if (!build_child_path(current_path, current_directory, entry.name) ||
            !build_child_path(snapshot_path, snapshot_directory, entry.name)) {
            result = 0;
            continue;
        }
        if (relative_directory[0] == '\0') {
            strcpy(relative_path, entry.name);
        } else if (!build_child_path(relative_path, relative_directory, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!report_new_and_changed_files(current_path, snapshot_path,
                                      relative_path, 0)) {
                result = 0;
            }
        } else if (stat(current_path, &current_information) != 0) {
            fprintf(stderr, "Cannot inspect file: %s\n", current_path);
            result = 0;
        } else if (stat(snapshot_path, &snapshot_information) != 0) {
            print_file_status_line("NEW", relative_path, NULL,
                             &current_information);
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
    } while (_dos_findnext(&entry) == 0);

    return result;
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
                                const char *relative_directory,
                                int exclude_archive)
{
    struct find_t entry;
    struct stat snapshot_information;
    char snapshot_path[MAX_PATH_LENGTH];
    char current_path[MAX_PATH_LENGTH];
    char relative_path[MAX_PATH_LENGTH];
    int result;

    if (!find_first_entry(snapshot_directory, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_entry(entry.name) ||
            (exclude_archive && stricmp(entry.name, ARCHIVE_DIRECTORY) == 0) ||
            stricmp(entry.name, EXECUTABLE_NAME) == 0) {
            continue;
        }
        if (!build_child_path(snapshot_path, snapshot_directory, entry.name) ||
            !build_child_path(current_path, current_directory, entry.name)) {
            result = 0;
            continue;
        }
        if (relative_directory[0] == '\0') {
            strcpy(relative_path, entry.name);
        } else if (!build_child_path(relative_path, relative_directory, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!report_deleted_files(snapshot_path, current_path,
                                      relative_path, 0)) {
                result = 0;
            }
        } else if (stat(current_path, &snapshot_information) != 0) {
            /* current_path doesn't exist -> re-stat the snapshot copy
             * (reusing snapshot_information) so its date/size can be shown. */
            if (stat(snapshot_path, &snapshot_information) == 0) {
                print_file_status_line("DELETED", relative_path,
                                 &snapshot_information, NULL);
            } else {
                result = 0;
            }
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

/*
 * Performs a "commit": archives the existing snapshot (if any), then
 * replaces the snapshot with a fresh copy of the current working tree.
 */
static int commit_snapshot(const char *root, const char *dosgit)
{
    char archive[MAX_PATH_LENGTH];
    char archive_name[13];
    char archive_path[MAX_PATH_LENGTH];
    time_t now;
    struct tm *parts;

    if (!create_directory_if_missing(dosgit) ||
        !build_child_path(archive, dosgit, ARCHIVE_DIRECTORY) ||
        !create_directory_if_missing(archive)) {
        return 0;
    }

    /* Decision point: only archive the old snapshot if it actually held
     * any files. On the very first commit there's nothing to preserve. */
    if (directory_has_any_file(dosgit, 1)) {
        now = time(NULL);
        parts = localtime(&now);
        if (parts == NULL) {
            fprintf(stderr, "Cannot determine the archive time.\n");
            return 0;
        }
        /* Archive folder name is HHMMSS.DDD (hour/min/sec + day-of-year),
         * giving each commit within the same day a distinct, sortable
         * name without needing the full date. */
        sprintf(archive_name, "%02d%02d%02d.%03d", parts->tm_hour,
                parts->tm_min, parts->tm_sec, parts->tm_yday + 1);
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

static void print_usage(const char *program_name)
{
    printf("Usage: %s [-commit] [path]\n", program_name);
}

int main(int argc, char **argv)
{
    char root[MAX_PATH_LENGTH];
    char dosgit[MAX_PATH_LENGTH];
    const char *requested_path;
    int commit_requested;
    int index;
    int answer;

    requested_path = NULL;
    commit_requested = 0;
    for (index = 1; index < argc; ++index) {
        if (stricmp(argv[index], "-commit") == 0) {
            commit_requested = 1;
        } else if (argv[index][0] == '-') {
            fprintf(stderr, "Unknown switch: %s\n", argv[index]);
            print_usage(argv[0]);
            return 1;
        } else if (requested_path == NULL) {
            requested_path = argv[index];
        } else {
            fprintf(stderr, "Only one path may be provided.\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    if (requested_path == NULL) {
        if (getcwd(root, sizeof(root)) == NULL) {
            fprintf(stderr, "Cannot determine the current directory.\n");
            return 1;
        }
    } else {
        if (strlen(requested_path) >= sizeof(root)) {
            fprintf(stderr, "Path is too long: %s\n", requested_path);
            return 1;
        }
        strcpy(root, requested_path);
    }

    if (!path_is_directory(root)) {
        fprintf(stderr, "Directory does not exist: %s\n", root);
        return 1;
    }
    if (!build_child_path(dosgit, root, DOSGIT_DIRECTORY)) {
        return 1;
    }

    if (commit_requested) {
        return commit_snapshot(root, dosgit) ? 0 : 1;
    }

    /* If there's no usable snapshot yet, offer to create one before
     * doing any comparison (there'd be nothing meaningful to compare). */
    if (!path_is_directory(dosgit) || !directory_has_any_file(dosgit, 1)) {
        printf("No committed files were found. Commit first? [Y/N] ");
        fflush(stdout);
        answer = getchar();
        if (answer == 'Y' || answer == 'y') {
            return commit_snapshot(root, dosgit) ? 0 : 1;
        }
        printf("No comparison was made.\n");
        return 1;
    }

    printf("STATUS    FILE\n");
    printf("          %-19s %10s  %-19s %10s\n", "PREVIOUS DATE",
           "PREV SIZE", "NEW DATE", "NEW SIZE");
    /* Two separate passes: current tree (new/changed/unchanged), then
     * snapshot tree (deleted) — see the comments on each function. */
    if (!report_new_and_changed_files(root, dosgit, "", 1)) {
        return 1;
    }
    return report_deleted_files(dosgit, root, "", 1) ? 0 : 1;
}
