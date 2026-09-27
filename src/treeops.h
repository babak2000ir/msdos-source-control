#ifndef TREEOPS_H
#define TREEOPS_H

int copy_directory_recursive(const char *source, const char *destination,
                             const char *excluded_root_directory,
                             const char *excluded_file_name);
int delete_directory_contents_recursive(const char *directory,
                                        const char *excluded_root_directory);
/* Returns 1 when a file exists, 0 when empty, or -1 on traversal error. */
int directory_has_any_file(const char *directory, int exclude_archive);

#endif
