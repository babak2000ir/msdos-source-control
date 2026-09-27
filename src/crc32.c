#include <stdio.h>
#include <string.h>

#include "crc32.h"

static uint32 crc_table[256];
static int crc_table_initialized = 0;


/*
 * Build the CRC-32 lookup table.
 *
 * Polynomial: 0xEDB88320
 * This is the reversed representation of
 * the standard CRC-32 polynomial 0x04C11DB7.
 */
static void crc32_init(void)
{
    uint32 crc;
    int i;
    int j;

    if (crc_table_initialized)
        return;

    for (i = 0; i < 256; i++) {
        crc = (uint32)i;

        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320UL;
            else
                crc >>= 1;
        }

        crc_table[i] = crc;
    }

    crc_table_initialized = 1;
}


/*
 * Add data to an existing CRC-32.
 */
static uint32 crc32_update(
    uint32 crc,
    const unsigned char *data,
    unsigned int len
)
{
    while (len--) {
        crc = (crc >> 8) ^
              crc_table[(crc ^ *data++) & 0xFF];
    }

    return crc;
}


/*
 * Calculate CRC-32 of:
 *
 *     <file size as decimal ASCII>\0<file contents>
 *
 * Returns 1 on success, 0 on failure.
 *
 * The resulting CRC is written to *result.
 */
int crc32_file(const char *source, uint32 *result)
{
    FILE *fp;
    unsigned char buffer[4096];
    unsigned int n;

    char size_string[32];
    long file_size;

    uint32 crc;
    unsigned char zero;


    crc32_init();


    /* Open in binary mode. */
    fp = fopen(source, "rb");

    if (!fp)
        return 0;


    /*
     * Determine file size.
     */
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    file_size = ftell(fp);

    if (file_size < 0) {
        fclose(fp);
        return 0;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }


    /*
     * CRC-32 starts with all bits set.
     */
    crc = 0xFFFFFFFFUL;


    /*
     * Hash the decimal file size.
     *
     * Example:
     *
     *     1234
     *
     * followed by a NUL byte.
     */
    sprintf(size_string, "%ld", file_size);

    crc = crc32_update(
        crc,
        (const unsigned char *)size_string,
        strlen(size_string)
    );


    /*
     * Hash the NUL separator.
     */
    zero = 0;

    crc = crc32_update(
        crc,
        &zero,
        1
    );


    /*
     * Hash the actual file contents.
     */
    while ((n = fread(buffer, 1, sizeof(buffer), fp)) != 0) {
        crc = crc32_update(crc, buffer, n);
    }

    if (ferror(fp)) {
        fclose(fp);
        return 0;
    }
    if (fclose(fp) != 0) {
        return 0;
    }


    /*
     * Final CRC-32.
     */
    *result = crc ^ 0xFFFFFFFFUL;

    return 1;
}
