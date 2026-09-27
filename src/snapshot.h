#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <time.h>

void make_archive_name(const struct tm *parts, char name[13]);
int commit_snapshot(const char *root, const char *dosgit);

#endif
