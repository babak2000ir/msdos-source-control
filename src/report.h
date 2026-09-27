#ifndef REPORT_H
#define REPORT_H

int report_status(const char *root, const char *dosgit);
int report_directories(const char *current_directory,
					   const char *snapshot_directory,
					   const char *excluded_current_root,
					   const char *excluded_snapshot_root,
					   int include_unchanged);

#endif
