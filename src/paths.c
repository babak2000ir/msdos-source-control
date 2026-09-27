#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dosgit.h"
#include "paths.h"

/*
 * Joins `right` onto `left` with a single backslash separator, writing
 * the result into `result`. Used everywhere a child path needs to be
 * built from a directory + entry name.
 */
int build_child_path(char *result, const char *left, const char *right)
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
int path_is_directory(const char *path)
{
    struct stat information;

    return stat(path, &information) == 0 &&
           (information.st_mode & S_IFDIR) != 0;
}

/* Creates `path` as a directory if it doesn't already exist as one. */
int create_directory_if_missing(const char *path)
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
int is_dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}
