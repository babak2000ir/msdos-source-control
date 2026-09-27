#include <dos.h>
#include <string.h>

#include "dosgit.h"
#include "paths.h"
#include "treewalk.h"

#define DOS_ERROR_NO_MORE_FILES 18

/* ------------------------------------------------------------------- *
 * Generic tree walker
 *
 * Every operation this tool needs (copying, deleting, checking "is
 * there anything here", comparing two trees) boils down to the same
 * walk: list a directory, skip "." / "..", optionally skip a few
 * excluded names, recurse into subdirectories, and do *something*
 * different with each file.
 *
 * walk_tree() below is the walk, written once. Each caller supplies a
 * small set of callbacks describing what it wants done at each file /
 * directory, plus which names to skip. The five original functions are
 * now thin wrappers that just plug their callbacks into walk_tree().
 * ------------------------------------------------------------------- */

/*
 * Starts a directory listing of `directory` into `entry` (DOS-style
 * find-first/find-next). Matches all attribute types (normal, read-only,
 * hidden, system, subdirectory) so nothing is silently skipped.
 */
static unsigned find_first_entry(const char *directory, struct find_t *entry)
{
    char pattern[MAX_PATH_LENGTH];

    if (!build_child_path(pattern, directory, "*.*")) {
        return 1;
    }
    return _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_HIDDEN |
                          _A_SYSTEM | _A_SUBDIR, entry);
}

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
 * Builds a one-name (or, if `name` is NULL, empty) NULL-terminated
 * exclusion list into `storage` (which must have room for 2 entries)
 * and returns it. This is the single place that turns "one optional
 * name" into the array shape walk_tree() expects.
 */
const char **single_name_exclude_list(const char *storage[2], const char *name)
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
int walk_tree(const char *directory, const char *relative_directory,
              int depth, const walk_callbacks_t *callbacks)
{
    struct find_t entry;
    char full_path[MAX_PATH_LENGTH];
    char relative_path[MAX_PATH_LENGTH];
    int outcome;
    unsigned search_error;

    search_error = find_first_entry(directory, &entry);
    if (search_error == DOS_ERROR_NO_MORE_FILES) {
        return WALK_OK;
    }
    if (search_error != 0) {
        return WALK_ERROR;
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
                                                               callbacks->context);
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
                                                                   callbacks->context);
                if (leave_outcome != WALK_OK) {
                    outcome = leave_outcome;
                    break;
                }
            }
        } else if (callbacks->on_file != NULL) {
            int file_outcome = callbacks->on_file(full_path, relative_path,
                                                   callbacks->context);
            if (file_outcome != WALK_OK) {
                outcome = file_outcome;
                break;
            }
        }
    } while ((search_error = _dos_findnext(&entry)) == 0);

    if (outcome == WALK_OK && search_error != DOS_ERROR_NO_MORE_FILES) {
        outcome = WALK_ERROR;
    }

    return outcome;
}
