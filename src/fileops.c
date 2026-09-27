#include <dos.h>
#include <stdio.h>

#include "dosgit.h"
#include "fileops.h"

/*
 * Copies a single file's bytes and preserves its DOS timestamp on the
 * destination. This timestamp preservation is what later lets the
 * comparison functions trust st_mtime as a proxy for "file changed".
 */
int copy_file_contents(const char *source, const char *destination, char *buffer)
{
    FILE *input;
    FILE *output;
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

    /* Copy the original mtime onto the new file so future comparisons
     * against this copy see it as identical rather than "changed". */
    if (succeeded && _dos_getftime(fileno(input), &file_date, &file_time) == 0) {
        fflush(output);
        _dos_setftime(fileno(output), file_date, file_time);
    }

    if (fclose(output) != 0) {
        succeeded = 0;
    }
    fclose(input);
    return succeeded;
}
