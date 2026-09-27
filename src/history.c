#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dosgit.h"
#include "history.h"
#include "paths.h"
#include "report.h"

#define DOS_ERROR_NO_MORE_FILES 18

typedef struct {
    char name[13];
    unsigned short write_date;
    unsigned short write_time;
} archive_entry_t;

static int compare_archive_entries(const void *left, const void *right)
{
    const archive_entry_t *left_entry = (const archive_entry_t *)left;
    const archive_entry_t *right_entry = (const archive_entry_t *)right;

    if (left_entry->write_date != right_entry->write_date) {
        return left_entry->write_date < right_entry->write_date ? -1 : 1;
    }
    if (left_entry->write_time != right_entry->write_time) {
        return left_entry->write_time < right_entry->write_time ? -1 : 1;
    }
    return stricmp(left_entry->name, right_entry->name);
}

/* Lists archived snapshots oldest-first using their DOS directory timestamps. */
int show_history(const char *dosgit)
{
    char archive_path[MAX_PATH_LENGTH];
    char search_path[MAX_PATH_LENGTH];
    char previous_path[MAX_PATH_LENGTH];
    char current_path[MAX_PATH_LENGTH];
    struct find_t entry;
    archive_entry_t *archives;
    archive_entry_t *resized_archives;
    size_t archive_count;
    size_t archive_capacity;
    size_t new_capacity;
    size_t index;
    unsigned long display_index;
    unsigned long display_count;
    unsigned search_error;
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    int allocation_failed;

    if (!build_child_path(archive_path, dosgit, ARCHIVE_DIRECTORY)) {
        return 0;
    }
    if (!path_is_directory(archive_path)) {
        printf("No archived commits found.\n");
        return 1;
    }
    if (!build_child_path(search_path, archive_path, "*.*")) {
        return 0;
    }

    archives = NULL;
    archive_count = 0;
    archive_capacity = 0;
    allocation_failed = 0;
    search_error = _dos_findfirst(search_path, _A_NORMAL | _A_RDONLY |
                                  _A_HIDDEN | _A_SYSTEM | _A_SUBDIR, &entry);
    if (search_error == DOS_ERROR_NO_MORE_FILES) {
        printf("No archived commits found.\n");
        return 1;
    }
    if (search_error != 0) {
        fprintf(stderr, "Cannot inspect archive directory: %s\n", archive_path);
        return 0;
    }

    do {
        if (is_dot_entry(entry.name) || (entry.attrib & _A_SUBDIR) == 0) {
            continue;
        }
        if (archive_count == archive_capacity) {
            new_capacity = archive_capacity == 0 ? 8 : archive_capacity * 2;
            if (new_capacity < archive_capacity ||
                new_capacity > (size_t)-1 / sizeof(*archives)) {
                allocation_failed = 1;
                break;
            }
            resized_archives = (archive_entry_t *)realloc(
                archives, new_capacity * sizeof(*archives));
            if (resized_archives == NULL) {
                allocation_failed = 1;
                break;
            }
            archives = resized_archives;
            archive_capacity = new_capacity;
        }

        strcpy(archives[archive_count].name, entry.name);
        archives[archive_count].write_date = entry.wr_date;
        archives[archive_count].write_time = entry.wr_time;
        ++archive_count;
    } while ((search_error = _dos_findnext(&entry)) == 0);

    _dos_findclose(&entry);
    if (allocation_failed) {
        fprintf(stderr, "Not enough memory to list archive history.\n");
        free(archives);
        return 0;
    }
    if (search_error != DOS_ERROR_NO_MORE_FILES) {
        fprintf(stderr, "Cannot finish reading archive directory: %s\n",
                archive_path);
        free(archives);
        return 0;
    }

    if (archive_count == 0) {
        printf("No archived commits found.\n");
        free(archives);
        return 1;
    }

    qsort(archives, archive_count, sizeof(*archives), compare_archive_entries);
    display_count = (unsigned long)archive_count;
    printf("ARCHIVE HISTORY\n===============\n");
    for (index = 0; index < archive_count; ++index) {
        year = ((archives[index].write_date >> 9) & 0x7F) + 1980;
        month = (archives[index].write_date >> 5) & 0x0F;
        day = archives[index].write_date & 0x1F;
        hour = (archives[index].write_time >> 11) & 0x1F;
        minute = (archives[index].write_time >> 5) & 0x3F;
        second = (archives[index].write_time & 0x1F) * 2;
          display_index = (unsigned long)index + 1;
          printf("\n[%lu/%lu] %s  %04u-%02u-%02u %02u:%02u:%02u\n",
                    display_index, display_count, archives[index].name,
                    year, month, day, hour, minute, second);
        if (index == 0) {
                printf("  initial archive (no previous archive)\n");
            continue;
        }
        if (!build_child_path(previous_path, archive_path,
                              archives[index - 1].name) ||
            !build_child_path(current_path, archive_path, archives[index].name)) {
            free(archives);
            return 0;
        }
        printf("  changes from %s\n", archives[index - 1].name);
        if (!report_directories(current_path, previous_path, NULL, NULL, 0)) {
            free(archives);
            return 0;
        }
    }

    if (!build_child_path(previous_path, archive_path,
                          archives[archive_count - 1].name)) {
        free(archives);
        return 0;
    }
    printf("\n[CURRENT] DOSGIT snapshot\n");
    printf("  changes from %s\n", archives[archive_count - 1].name);
    if (!report_directories(dosgit, previous_path, ARCHIVE_DIRECTORY,
                            NULL, 0)) {
        free(archives);
        return 0;
    }

    free(archives);
    return 1;
}
