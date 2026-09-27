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

/* ------------------------------------------------------------------- *
 * Generic tree walker
 *
 * Every operation this tool needs (copying, deleting, checking "is
 * there anything here", comparing two trees) boils down to the same
 * walk: list a directory, skip "." / "..", optionally skip a few
 * excluded names, recurse into subdirectories, and do *something*
 * different with each file. Previously that walk was duplicated five
 * times with a different body each time — any fix to the walking logic
 * (exclusion rules, path building, error handling) had to be made in
 * five places and was easy to get out of sync.
 *
 * walk_tree() below is that walk, written once. Each caller supplies a
 * small set of callbacks describing what it wants done at each file /
 * directory, plus which names to skip. The five original functions are
 * now thin wrappers that just plug their callbacks into walk_tree().
 * ------------------------------------------------------------------- */

/* Return values for walk_tree() and its callbacks. */
enum walk_result {
    WALK_OK    = 0,  /* success / keep walking */
    WALK_STOP  = 1,  /* not an error - stop the whole walk early (e.g. "found it") */
    WALK_ERROR = -1  /* something failed - abort the whole walk */
};

typedef int (*walk_file_callback)(const char *full_path, const char *relative_path,
                                   const struct find_t *entry, void *context);
typedef int (*walk_directory_callback)(const char *full_path, const char *relative_path,
                                        const struct find_t *entry, void *context);

/*
 * Describes one walk: what to skip, what to do, and where to stash
 * caller-specific state (the `context` pointer, passed back untouched
 * to every callback).
 *
 * excluded_at_root: NULL-terminated list of names to skip, but only at
 *   the walk's top level (e.g. don't recurse into the "dosgit" snapshot
 *   folder from the project root - but a folder that happens to be
 *   called "dosgit" nested three levels down is none of our business).
 * excluded_always: NULL-terminated list of names to skip at every
 *   depth (e.g. never touch "git.exe" itself, wherever it turns up).
 *
 * on_directory_enter fires before descending into a subdirectory;
 * on_directory_leave fires after its whole contents have been walked.
 * Either may be NULL to skip that hook - useful since some operations
 * only care about one side (e.g. copying creates directories on the
 * way in, deleting removes them on the way out).
 */
typedef struct {
    const char **excluded_at_root;
    const char **excluded_always;
    void *context;
    walk_file_callback on_file;
    walk_directory_callback on_directory_enter;
    walk_directory_callback on_directory_leave;
} walk_callbacks_t;

/* True if `name` (case-insensitive) appears in a NULL-terminated list. */
static int name_is_excluded(const char *name, const char **excluded_names)
{
    if (excluded_names == NULL) {
        return 0;
    }
    while (*excluded_names != NULL) {
        if (stricmp(name, *excluded_names) == 0) {
            return 1;
        }
        excluded_names++;
    }
    return 0;
}

/*
 * The exclusion lists themselves, as data rather than scattered
 * stricmp() checks. `git.exe` is excluded at every depth, everywhere -
 * this one list is shared by every wrapper below instead of each one
 * declaring its own copy of the same single-entry array.
 *
 * The "root" folder names (dosgit, archive) can't be a single shared
 * constant the same way, because which one applies depends on which
 * tree is being walked and whether a caller asked for it - so those are
 * still built per call, via single_name_exclude_list() below. But the
 * shape is the same: a plain data array walk_tree() consumes, not a
 * code path. Adding something like a user-editable .dosgitignore later
 * would mean changing how one of these arrays gets built, never
 * touching walk_tree() or any of the callbacks.
 */
static const char *excluded_executable[] = { EXECUTABLE_NAME, NULL };

/*
 * Builds a one-name (or, if `name` is NULL, empty) NULL-terminated
 * exclusion list into `storage` (which must have room for 2 entries)
 * and returns it. This is the single place that turns "one optional
 * name" into the array shape walk_tree() expects.
 */
static const char **single_name_exclude_list(const char *storage[2], const char *name)
{
    storage[0] = name;
    storage[1] = NULL;
    return storage;
}

/*
 * Walks `directory` depth-first, invoking `callbacks` for every entry
 * found. `relative_directory` is the path built up so far, relative to
 * the walk's root (empty string at the top) - this is what callbacks
 * receive so they can locate the "same" file in another tree (e.g. the
 * snapshot, or a copy destination) without needing their own recursion.
 * `depth` is 0 at the root, incrementing with each subdirectory, and is
 * what makes excluded_at_root apply only at the very top.
 */
static int walk_tree(const char *directory, const char *relative_directory,
                      int depth, const walk_callbacks_t *callbacks)
{
    struct find_t entry;
    char full_path[MAX_PATH_LENGTH];
    char relative_path[MAX_PATH_LENGTH];
    int outcome;

    if (!find_first_entry(directory, &entry)) {
        return WALK_OK;
    }

    outcome = WALK_OK;
    do {
        if (is_dot_entry(entry.name)) {
            continue;
        }
        if (depth == 0 && name_is_excluded(entry.name, callbacks->excluded_at_root)) {
            continue;
        }
        if (name_is_excluded(entry.name, callbacks->excluded_always)) {
            continue;
        }
        if (!build_child_path(full_path, directory, entry.name)) {
            outcome = WALK_ERROR;
            break;
        }
        if (relative_directory[0] == '\0') {
            strcpy(relative_path, entry.name);
        } else if (!build_child_path(relative_path, relative_directory, entry.name)) {
            outcome = WALK_ERROR;
            break;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            int enter_outcome;
            int sub_outcome;

            enter_outcome = WALK_OK;
            if (callbacks->on_directory_enter != NULL) {
                enter_outcome = callbacks->on_directory_enter(full_path, relative_path,
                                                               &entry, callbacks->context);
            }
            if (enter_outcome != WALK_OK) {
                outcome = enter_outcome;
                break;
            }

            /* Recursion happens here, once, regardless of which
             * operation is being performed - callers never write their
             * own recursive call anymore. */
            sub_outcome = walk_tree(full_path, relative_path, depth + 1, callbacks);
            if (sub_outcome != WALK_OK) {
                outcome = sub_outcome;
                break;
            }

            if (callbacks->on_directory_leave != NULL) {
                int leave_outcome = callbacks->on_directory_leave(full_path, relative_path,
                                                                   &entry, callbacks->context);
                if (leave_outcome != WALK_OK) {
                    outcome = leave_outcome;
                    break;
                }
            }
        } else if (callbacks->on_file != NULL) {
            int file_outcome = callbacks->on_file(full_path, relative_path, &entry,
                                                   callbacks->context);
            if (file_outcome != WALK_OK) {
                outcome = file_outcome;
                break;
            }
        }
    } while (_dos_findnext(&entry) == 0);

    return outcome;
}

/* ---- copy_directory_recursive: walk_tree callbacks + wrapper -------- */

typedef struct {
    const char *destination_base;
} copy_context_t;

static int copy_directory_visitor(const char *full_path, const char *relative_path,
                                   const struct find_t *entry, void *context)
{
    copy_context_t *copy_context = (copy_context_t *)context;
    char destination_path[MAX_PATH_LENGTH];

    (void)full_path;
    (void)entry;
    if (!build_child_path(destination_path, copy_context->destination_base, relative_path)) {
        return WALK_ERROR;
    }
    return create_directory_if_missing(destination_path) ? WALK_OK : WALK_ERROR;
}

static int copy_file_visitor(const char *full_path, const char *relative_path,
                              const struct find_t *entry, void *context)
{
    copy_context_t *copy_context = (copy_context_t *)context;
    char destination_path[MAX_PATH_LENGTH];

    (void)entry;
    if (!build_child_path(destination_path, copy_context->destination_base, relative_path)) {
        return WALK_ERROR;
    }
    return copy_file_contents(full_path, destination_path) ? WALK_OK : WALK_ERROR;
}

/*
 * Recursively copies every file and subdirectory from `source` into
 * `destination`.
 *
 * `excluded_root_directory`: a name to skip, but only at the top level
 * of `source` (e.g. don't copy the "dosgit" snapshot folder into a
 * fresh snapshot of itself). `excluded_file_name`: a name to skip at
 * every level (e.g. never copy "git.exe" itself).
 */
static int copy_directory_recursive(const char *source, const char *destination,
                     const char *excluded_root_directory,
                     const char *excluded_file_name)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];
    const char *always_excludes[2];
    copy_context_t context;

    if (!create_directory_if_missing(destination)) {
        return 0;
    }

    context.destination_base = destination;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes, excluded_root_directory);
    callbacks.excluded_always = single_name_exclude_list(always_excludes, excluded_file_name);
    callbacks.context = &context;
    callbacks.on_file = copy_file_visitor;
    callbacks.on_directory_enter = copy_directory_visitor;

    return walk_tree(source, "", 0, &callbacks) != WALK_ERROR;
}

/* ---- delete_directory_contents_recursive: callbacks + wrapper ------ */

static int delete_file_visitor(const char *full_path, const char *relative_path,
                                const struct find_t *entry, void *context)
{
    (void)relative_path;
    (void)entry;
    (void)context;

    /* Clear read-only attribute first; otherwise remove() on a
     * read-only file (e.g. one copied with its original attributes)
     * would fail. */
    chmod(full_path, S_IWRITE | S_IREAD);
    if (remove(full_path) != 0) {
        fprintf(stderr, "Cannot remove file: %s\n", full_path);
        return WALK_ERROR;
    }
    return WALK_OK;
}

static int delete_directory_visitor(const char *full_path, const char *relative_path,
                                     const struct find_t *entry, void *context)
{
    (void)relative_path;
    (void)entry;
    (void)context;

    /* This runs as on_directory_leave, i.e. after the subdirectory's
     * own contents have already been walked (and deleted) - the
     * directory is only rmdir'd once it's empty. */
    if (rmdir(full_path) != 0) {
        fprintf(stderr, "Cannot remove directory: %s\n", full_path);
        return WALK_ERROR;
    }
    return WALK_OK;
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
    walk_callbacks_t callbacks;
    const char *root_excludes[2];

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes, excluded_root_directory);
    callbacks.on_file = delete_file_visitor;
    callbacks.on_directory_leave = delete_directory_visitor;

    return walk_tree(directory, "", 0, &callbacks) != WALK_ERROR;
}

/* ---- directory_has_any_file: callback + wrapper --------------------- */

typedef struct {
    int found;
} has_any_file_context_t;

static int has_any_file_visitor(const char *full_path, const char *relative_path,
                                 const struct find_t *entry, void *context)
{
    has_any_file_context_t *found_context = (has_any_file_context_t *)context;

    (void)full_path;
    (void)relative_path;
    (void)entry;

    found_context->found = 1;
    /* One file is enough to answer the question - stop walking instead
     * of scanning the rest of a possibly large tree. */
    return WALK_STOP;
}

/*
 * True if `directory` contains at least one real file anywhere below
 * it. Used to decide whether there's anything worth archiving/
 * comparing yet (an empty or brand-new snapshot folder doesn't need
 * either). `exclude_archive` skips the "archive" folder, but only at
 * the top level - see excluded_at_root above.
 */
static int directory_has_any_file(const char *directory, int exclude_archive)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];
    has_any_file_context_t context;

    context.found = 0;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes,
                                     exclude_archive ? ARCHIVE_DIRECTORY : NULL);
    callbacks.excluded_always = excluded_executable;
    callbacks.context = &context;
    callbacks.on_file = has_any_file_visitor;

    walk_tree(directory, "", 0, &callbacks);
    return context.found;
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

/* ---- report_new_and_changed_files: callback + wrapper --------------- */

typedef struct {
    const char *snapshot_base;
} report_new_context_t;

static int report_new_or_changed_visitor(const char *full_path, const char *relative_path,
                                          const struct find_t *entry, void *context)
{
    report_new_context_t *report_context = (report_new_context_t *)context;
    struct stat current_information;
    struct stat snapshot_information;
    char snapshot_path[MAX_PATH_LENGTH];

    (void)entry;

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
                                   const struct find_t *entry, void *context)
{
    report_deleted_context_t *report_context = (report_deleted_context_t *)context;
    struct stat current_information;
    struct stat snapshot_information;
    char current_path[MAX_PATH_LENGTH];

    (void)entry;

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
    if (!report_new_and_changed_files(root, dosgit, 1)) {
        return 1;
    }
    return report_deleted_files(dosgit, root, 1) ? 0 : 1;
}
