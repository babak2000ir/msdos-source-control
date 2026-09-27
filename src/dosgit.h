#ifndef DOSGIT_H
#define DOSGIT_H

#define MAX_PATH_LENGTH 260
#define COPY_BUFFER_SIZE 16384
#define DOSGIT_DIRECTORY "dosgit"
#define ARCHIVE_DIRECTORY "archive"
#define HASH_MANIFEST_NAME "HASH"
#define EXECUTABLE_NAME "git.exe"
#define FILE_LINE_WIDTH 69

/* NULL-terminated exclusion list containing EXECUTABLE_NAME; defined in treeops.c. */
extern const char *excluded_executable[];

#endif
