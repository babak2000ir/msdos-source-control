#ifndef CRC32_H
#define CRC32_H

typedef unsigned long uint32;

int crc32_file(const char *source, uint32 *result);

#endif
