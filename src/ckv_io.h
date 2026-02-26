#ifndef CKV_IO_H
#define CKV_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int ckv_pread_full(int fd, void *buf, size_t n, off_t off);
int ckv_pwrite_full(int fd, const void *buf, size_t n, off_t off);
int ckv_write_full(int fd, const void *buf, size_t n);
int ckv_fsync_file(int fd);
int ckv_fsync_dir_of_path(const char *path);

#endif
