#ifndef TREEWALK_H
#define TREEWALK_H

/* Return values for walk_tree() and its callbacks. */
enum walk_result {
    WALK_OK    = 0,  /* success / keep walking */
    WALK_STOP  = 1,  /* not an error - stop the whole walk early (e.g. "found it") */
    WALK_ERROR = -1  /* something failed - abort the whole walk */
};

typedef int (*walk_file_callback)(const char *full_path, const char *relative_path,
                                   void *context);
typedef int (*walk_directory_callback)(const char *full_path, const char *relative_path,
                                        void *context);

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

const char **single_name_exclude_list(const char *storage[2], const char *name);
int walk_tree(const char *directory, const char *relative_directory,
              int depth, const walk_callbacks_t *callbacks);

#endif
