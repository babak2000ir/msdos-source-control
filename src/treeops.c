#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dosgit.h"
#include "fileops.h"
#include "paths.h"
#include "treeops.h"
#include "treewalk.h"

/*
 * The exclusion lists themselves, as data rather than scattered
 * stricmp() checks. `git.exe` is excluded at every depth, everywhere -
 * this one list is shared by every wrapper instead of each one
 * declaring its own copy of the same single-entry array.
 *
 * The "root" folder names (dosgit, archive) can't be a single shared
 * constant the same way, because which one applies depends on which
 * tree is being walked and whether a caller asked for it - so those are
 * still built per call, via single_name_exclude_list(). But the
 * shape is the same: a plain data array walk_tree() consumes, not a
 * code path. Adding something like a user-editable .dosgitignore later
 * would mean changing how one of these arrays gets built, never
 * touching walk_tree() or any of the callbacks.
 */
const char *excluded_executable[] = { EXECUTABLE_NAME, NULL };

/* ---- copy_directory_recursive: walk_tree callbacks + wrapper -------- */

typedef struct {
    const char *destination_base;
    char *buffer;
} copy_context_t;

static int copy_directory_visitor(const char *full_path, const char *relative_path,
                                   void *context)
{
    copy_context_t *copy_context = (copy_context_t *)context;
    char destination_path[MAX_PATH_LENGTH];

    (void)full_path;
    if (!build_child_path(destination_path, copy_context->destination_base, relative_path)) {
        return WALK_ERROR;
    }
    return create_directory_if_missing(destination_path) ? WALK_OK : WALK_ERROR;
}

static int copy_file_visitor(const char *full_path, const char *relative_path,
                              void *context)
{
    copy_context_t *copy_context = (copy_context_t *)context;
    char destination_path[MAX_PATH_LENGTH];

    if (!build_child_path(destination_path, copy_context->destination_base, relative_path)) {
        return WALK_ERROR;
    }
        return copy_file_contents(full_path, destination_path, copy_context->buffer) ?
            WALK_OK : WALK_ERROR;
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
int copy_directory_recursive(const char *source, const char *destination,
                             const char *excluded_root_directory,
                             const char *excluded_file_name)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];
    const char *always_excludes[2];
    copy_context_t context;
    int outcome;

    if (!create_directory_if_missing(destination)) {
        return 0;
    }

    context.destination_base = destination;
    context.buffer = (char *)malloc(COPY_BUFFER_SIZE);
    if (context.buffer == NULL) {
        fprintf(stderr, "Not enough memory to copy files.\n");
        return 0;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes, excluded_root_directory);
    callbacks.excluded_always = single_name_exclude_list(always_excludes, excluded_file_name);
    callbacks.context = &context;
    callbacks.on_file = copy_file_visitor;
    callbacks.on_directory_enter = copy_directory_visitor;

    outcome = walk_tree(source, "", 0, &callbacks);
    free(context.buffer);
    return outcome != WALK_ERROR;
}

/* ---- delete_directory_contents_recursive: callbacks + wrapper ------ */

static int delete_file_visitor(const char *full_path, const char *relative_path,
                                void *context)
{
    (void)relative_path;
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
                                     void *context)
{
    (void)relative_path;
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
int delete_directory_contents_recursive(const char *directory,
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
                                 void *context)
{
    has_any_file_context_t *found_context = (has_any_file_context_t *)context;

    (void)full_path;
    (void)relative_path;

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
 * the top level - see excluded_at_root in treewalk.h.
 */
int directory_has_any_file(const char *directory, int exclude_archive)
{
    walk_callbacks_t callbacks;
    const char *root_excludes[2];
    has_any_file_context_t context;
    int outcome;

    context.found = 0;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.excluded_at_root = single_name_exclude_list(root_excludes,
                                     exclude_archive ? ARCHIVE_DIRECTORY : NULL);
    callbacks.excluded_always = excluded_executable;
    callbacks.context = &context;
    callbacks.on_file = has_any_file_visitor;

    outcome = walk_tree(directory, "", 0, &callbacks);
    if (outcome == WALK_ERROR) {
        return -1;
    }
    return context.found;
}
