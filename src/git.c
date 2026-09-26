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

static int path_join(char *result, const char *left, const char *right)
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

static int is_directory(const char *path)
{
    struct stat information;

    return stat(path, &information) == 0 &&
           (information.st_mode & S_IFDIR) != 0;
}

static int ensure_directory(const char *path)
{
    if (is_directory(path)) {
        return 1;
    }
    if (mkdir(path) == 0) {
        return 1;
    }
    fprintf(stderr, "Cannot create directory: %s\n", path);
    return 0;
}

static int is_dot_directory(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int find_first(const char *directory, struct find_t *entry)
{
    char pattern[MAX_PATH_LENGTH];

    if (!path_join(pattern, directory, "*.*")) {
        return 0;
    }
    return _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_HIDDEN |
                          _A_SYSTEM | _A_SUBDIR, entry) == 0;
}

static int copy_file(const char *source, const char *destination)
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

static int copy_tree(const char *source, const char *destination,
                     const char *excluded_root_directory,
                     const char *excluded_file_name)
{
    struct find_t entry;
    char source_path[MAX_PATH_LENGTH];
    char destination_path[MAX_PATH_LENGTH];
    int result;

    if (!ensure_directory(destination)) {
        return 0;
    }
    if (!find_first(source, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_directory(entry.name) ||
            (excluded_root_directory != NULL &&
             stricmp(entry.name, excluded_root_directory) == 0) ||
            (excluded_file_name != NULL &&
             stricmp(entry.name, excluded_file_name) == 0)) {
            continue;
        }
        if (!path_join(source_path, source, entry.name) ||
            !path_join(destination_path, destination, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!copy_tree(source_path, destination_path, NULL,
                           excluded_file_name)) {
                result = 0;
            }
        } else if (!copy_file(source_path, destination_path)) {
            result = 0;
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

static int delete_tree_contents(const char *directory,
                                const char *excluded_root_directory)
{
    struct find_t entry;
    char path[MAX_PATH_LENGTH];
    int result;

    if (!find_first(directory, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_directory(entry.name) ||
            (excluded_root_directory != NULL &&
             stricmp(entry.name, excluded_root_directory) == 0)) {
            continue;
        }
        if (!path_join(path, directory, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!delete_tree_contents(path, NULL) || rmdir(path) != 0) {
                fprintf(stderr, "Cannot remove directory: %s\n", path);
                result = 0;
            }
        } else {
            chmod(path, S_IWRITE | S_IREAD);
            if (remove(path) != 0) {
                fprintf(stderr, "Cannot remove file: %s\n", path);
                result = 0;
            }
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

static int tree_contains_file(const char *directory, int exclude_archive)
{
    struct find_t entry;
    char path[MAX_PATH_LENGTH];

    if (!find_first(directory, &entry)) {
        return 0;
    }

    do {
        if (is_dot_directory(entry.name) ||
            (exclude_archive && stricmp(entry.name, ARCHIVE_DIRECTORY) == 0) ||
            stricmp(entry.name, EXECUTABLE_NAME) == 0) {
            continue;
        }
        if ((entry.attrib & _A_SUBDIR) == 0) {
            return 1;
        }
        if (path_join(path, directory, entry.name) &&
            tree_contains_file(path, 0)) {
            return 1;
        }
    } while (_dos_findnext(&entry) == 0);

    return 0;
}

static void format_file_time(time_t value, char *result)
{
    struct tm *parts;

    parts = localtime(&value);
    if (parts == NULL) {
        strcpy(result, "-------------------");
        return;
    }
    strftime(result, 20, "%Y-%m-%d %H:%M:%S", parts);
}

static void print_comparison(const char *status, const char *relative_path,
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
        format_file_time(previous->st_mtime, previous_date);
        previous_size = previous->st_size;
    } else {
        strcpy(previous_date, "-------------------");
        previous_size = -1L;
    }
    if (current != NULL) {
        format_file_time(current->st_mtime, current_date);
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

static int compare_current_tree(const char *current_directory,
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

    if (!find_first(current_directory, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_directory(entry.name) ||
            (exclude_dosgit && stricmp(entry.name, DOSGIT_DIRECTORY) == 0) ||
            stricmp(entry.name, EXECUTABLE_NAME) == 0) {
            continue;
        }
        if (!path_join(current_path, current_directory, entry.name) ||
            !path_join(snapshot_path, snapshot_directory, entry.name)) {
            result = 0;
            continue;
        }
        if (relative_directory[0] == '\0') {
            strcpy(relative_path, entry.name);
        } else if (!path_join(relative_path, relative_directory, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!compare_current_tree(current_path, snapshot_path,
                                      relative_path, 0)) {
                result = 0;
            }
        } else if (stat(current_path, &current_information) != 0) {
            fprintf(stderr, "Cannot inspect file: %s\n", current_path);
            result = 0;
        } else if (stat(snapshot_path, &snapshot_information) != 0) {
            print_comparison("NEW", relative_path, NULL,
                             &current_information);
        } else if (snapshot_information.st_size != current_information.st_size ||
                   snapshot_information.st_mtime != current_information.st_mtime) {
            print_comparison("CHANGED", relative_path, &snapshot_information,
                             &current_information);
        } else {
            print_comparison("UNCHANGED", relative_path, &snapshot_information,
                             &current_information);
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

static int compare_deleted_tree(const char *snapshot_directory,
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

    if (!find_first(snapshot_directory, &entry)) {
        return 1;
    }

    result = 1;
    do {
        if (is_dot_directory(entry.name) ||
            (exclude_archive && stricmp(entry.name, ARCHIVE_DIRECTORY) == 0) ||
            stricmp(entry.name, EXECUTABLE_NAME) == 0) {
            continue;
        }
        if (!path_join(snapshot_path, snapshot_directory, entry.name) ||
            !path_join(current_path, current_directory, entry.name)) {
            result = 0;
            continue;
        }
        if (relative_directory[0] == '\0') {
            strcpy(relative_path, entry.name);
        } else if (!path_join(relative_path, relative_directory, entry.name)) {
            result = 0;
            continue;
        }

        if ((entry.attrib & _A_SUBDIR) != 0) {
            if (!compare_deleted_tree(snapshot_path, current_path,
                                      relative_path, 0)) {
                result = 0;
            }
        } else if (stat(current_path, &snapshot_information) != 0) {
            if (stat(snapshot_path, &snapshot_information) == 0) {
                print_comparison("DELETED", relative_path,
                                 &snapshot_information, NULL);
            } else {
                result = 0;
            }
        }
    } while (_dos_findnext(&entry) == 0);

    return result;
}

static int commit(const char *root, const char *dosgit)
{
    char archive[MAX_PATH_LENGTH];
    char archive_name[13];
    char archive_path[MAX_PATH_LENGTH];
    time_t now;
    struct tm *parts;

    if (!ensure_directory(dosgit) ||
        !path_join(archive, dosgit, ARCHIVE_DIRECTORY) ||
        !ensure_directory(archive)) {
        return 0;
    }

    if (tree_contains_file(dosgit, 1)) {
        now = time(NULL);
        parts = localtime(&now);
        if (parts == NULL) {
            fprintf(stderr, "Cannot determine the archive time.\n");
            return 0;
        }
        sprintf(archive_name, "%02d%02d%02d.%03d", parts->tm_hour,
                parts->tm_min, parts->tm_sec, parts->tm_yday + 1);
        if (!path_join(archive_path, archive, archive_name) ||
            is_directory(archive_path)) {
            fprintf(stderr, "Archive already exists: %s\n", archive_path);
            return 0;
        }
        printf("Archiving previous commit to %s\n", archive_path);
        if (!copy_tree(dosgit, archive_path, ARCHIVE_DIRECTORY,
                   EXECUTABLE_NAME)) {
            return 0;
        }
    }

    if (!delete_tree_contents(dosgit, ARCHIVE_DIRECTORY)) {
        return 0;
    }
    printf("Copying files into %s\n", dosgit);
    if (!copy_tree(root, dosgit, DOSGIT_DIRECTORY, EXECUTABLE_NAME)) {
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

    if (!is_directory(root)) {
        fprintf(stderr, "Directory does not exist: %s\n", root);
        return 1;
    }
    if (!path_join(dosgit, root, DOSGIT_DIRECTORY)) {
        return 1;
    }

    if (commit_requested) {
        return commit(root, dosgit) ? 0 : 1;
    }

    if (!is_directory(dosgit) || !tree_contains_file(dosgit, 1)) {
        printf("No committed files were found. Commit first? [Y/N] ");
        fflush(stdout);
        answer = getchar();
        if (answer == 'Y' || answer == 'y') {
            return commit(root, dosgit) ? 0 : 1;
        }
        printf("No comparison was made.\n");
        return 1;
    }

    printf("STATUS    FILE\n");
    printf("          %-19s %10s  %-19s %10s\n", "PREVIOUS DATE",
           "PREV SIZE", "NEW DATE", "NEW SIZE");
    if (!compare_current_tree(root, dosgit, "", 1)) {
        return 1;
    }
    return compare_deleted_tree(dosgit, root, "", 1) ? 0 : 1;
}
