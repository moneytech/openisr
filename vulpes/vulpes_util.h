#ifndef VULPES_UTIL_H_
#define VULPES_UTIL_H_

#include "vulpes.h"

int is_dir(const char *name);
int is_file(const char *name);
off_t get_filesize(int fd);
vulpes_err_t read_file(int fd, char *buf, int *bufsize);
char *vulpes_strerror(vulpes_err_t err);

#endif
