#ifndef PATHS_H
#define PATHS_H

int build_child_path(char *result, const char *left, const char *right);
int path_is_directory(const char *path);
int create_directory_if_missing(const char *path);
int is_dot_entry(const char *name);

#endif
