#define _GNU_SOURCE
#include "ckv_io.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int ckv_pread_full(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        if (r == 0) return -1; /* EOF */
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int ckv_pwrite_full(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t put = 0;
    while (put < n) {
        ssize_t r = pwrite(fd, p + put, n - put, off + (off_t)put);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EIO; return -1; }
        put += (size_t)r;
    }
    return 0;
}

int ckv_write_full(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, p + put, n - put);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EIO; return -1; }
        put += (size_t)r;
    }
    return 0;
}

int ckv_fsync_file(int fd) {
    for (;;) {
        if (fsync(fd) == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

int ckv_fsync_dir_of_path(const char *path) {
    /* Best-effort durability for rename: fsync the containing directory. */
    const char *slash = strrchr(path, '/');
    char dirbuf[4096];
    const char *dir = ".";
    if (slash) {
        size_t len = slash == path ? 1 : (size_t)(slash - path);
        if (len >= sizeof(dirbuf)) return -1;
        memcpy(dirbuf, path, len);
        dirbuf[len] = '\0';
        dir = dirbuf;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return -1;
    int rc = ckv_fsync_file(dfd);
    close(dfd);
    return rc;
}
