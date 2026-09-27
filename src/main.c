#include <direct.h>
#include <stdio.h>
#include <string.h>

#include "dosgit.h"
#include "paths.h"
#include "report.h"
#include "snapshot.h"
#include "treeops.h"

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
    int has_snapshot_files;

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
    has_snapshot_files = 0;
    if (path_is_directory(dosgit)) {
        has_snapshot_files = directory_has_any_file(dosgit, 1);
        if (has_snapshot_files < 0) {
            fprintf(stderr, "Cannot inspect snapshot directory: %s\n", dosgit);
            return 1;
        }
    }
    if (!has_snapshot_files) {
        printf("No committed files were found. Commit first? [Y/N] ");
        fflush(stdout);
        answer = getchar();
        if (answer == 'Y' || answer == 'y') {
            return commit_snapshot(root, dosgit) ? 0 : 1;
        }
        printf("No comparison was made.\n");
        return 1;
    }

    return report_status(root, dosgit) ? 0 : 1;
}
