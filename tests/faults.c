#define _GNU_SOURCE
#include "ckvstore.h"
#include "ckv_index.h"
#include "ckv_io.h"
#include "ckv_log.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Compile private copies with syscall/allocation wrappers. Production has no
 * test hooks. The archive supplies only the remaining CRC implementation. */
static int io_at, io_count, io_hit, alloc_at, alloc_count, alloc_hit;
static int zero_write, interrupt_read, short_write;
static int io_fault(void) {
    if (io_at && ++io_count == io_at) { io_hit = 1; errno = EIO; return 1; }
    return 0;
}
static ssize_t test_pread(int fd, void *buf, size_t n, off_t off) {
    if (interrupt_read) { interrupt_read = 0; errno = EINTR; return -1; }
    if (io_fault()) return -1;
    return pread(fd, buf, n, off);
}
static ssize_t test_pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (zero_write) { zero_write = 0; return 0; }
    if (io_fault()) return -1;
    if (short_write && n > 1) n = 1;
    return pwrite(fd, buf, n, off);
}
static int test_fsync(int fd) { return io_fault() ? -1 : fsync(fd); }
static int test_truncate(int fd, off_t n) { return io_fault() ? -1 : ftruncate(fd, n); }
static int test_rename(const char *a, const char *b) { return io_fault() ? -1 : rename(a, b); }
static int alloc_fault(void) {
    if (alloc_at && ++alloc_count == alloc_at) { alloc_hit = 1; return 1; }
    return 0;
}
static void *test_malloc(size_t n) { return alloc_fault() ? NULL : malloc(n); }
static void *test_calloc(size_t n, size_t s) { return alloc_fault() ? NULL : calloc(n, s); }

#define pread test_pread
#define pwrite test_pwrite
#define fsync test_fsync
#define ftruncate test_truncate
#define rename test_rename
#define malloc test_malloc
#define calloc test_calloc
#include "../src/ckv_io.c"
#include "../src/ckv_index.c"
#include "../src/ckv_log.c"
#include "../src/ckv_store.c"
#undef pread
#undef pwrite
#undef fsync
#undef ftruncate
#undef rename
#undef malloc
#undef calloc

static struct ckv_config config(const char *path) {
    struct ckv_config c = {path, 64, 1024, 64, 16, CKV_FSYNC_ON_COMMIT, 4};
    return c;
}
static off_t file_size(const char *path) {
    struct stat sb;
    assert(stat(path, &sb) == 0);
    return sb.st_size;
}
static void expect(struct ckv_store *st, const char *key, const char *value) {
    char out[64]; size_t len;
    assert(ckv_get(st, key, strlen(key), out, sizeof(out), &len) == CKV_OK);
    assert(len == strlen(value) && memcmp(out, value, len) == 0);
}
static void reset_faults(void) {
    io_at = io_count = io_hit = alloc_at = alloc_count = alloc_hit = 0;
}
static void check_failed_handle(struct ckv_store *st) {
    char out[16]; size_t len;
    struct ckv_iter it = {0};
    assert(ckv_get(st, "a", 1, out, sizeof(out), &len) == CKV_EIO);
    assert(ckv_put(st, "z", 1, "z", 1) == CKV_EIO);
    assert(ckv_del(st, "a", 1) == CKV_EIO);
    assert(ckv_compact(st) == CKV_EIO);
    assert(ckv_iter_begin(st, &it) == CKV_EIO);
    assert(ckv_iter_next(st, &it, out, sizeof(out), &len) == CKV_EIO);
}

static void compaction_faults(const char *path, int allocations) {
    for (int n = 1; n < 100; n++) {
        reset_faults();
        (void)unlink(path);
        struct ckv_config c = config(path);
        struct ckv_store *st = NULL;
        assert(ckv_open(&c, &st) == CKV_OK);
        assert(ckv_put(st, "a", 1, "old", 3) == CKV_OK);
        assert(ckv_put(st, "a", 1, "new", 3) == CKV_OK);
        assert(ckv_put(st, "b", 1, "live", 4) == CKV_OK);
        if (allocations) alloc_at = n; else io_at = n;
        enum ckv_err rc = ckv_compact(st);
        int hit = allocations ? alloc_hit : io_hit;
        reset_faults();
        assert(rc == (hit ? (allocations ? CKV_ENOMEM : CKV_EIO) : CKV_OK));
        if (st->failed) check_failed_handle(st);
        else { expect(st, "a", "new"); expect(st, "b", "live"); }
        ckv_close(st);
        assert(ckv_open(&c, &st) == CKV_OK);
        expect(st, "a", "new"); expect(st, "b", "live");
        assert(ckv_put(st, "c", 1, "after", 5) == CKV_OK);
        ckv_close(st);
        assert(unlink(path) == 0);
        if (!hit) return; /* every wrapped failure point has been exercised */
    }
    assert(!"compaction fault loop did not terminate");
}

static void write_faults(const char *path) {
    for (int n = 1; n < 20; n++) {
        reset_faults();
        struct ckv_config c = config(path);
        struct ckv_store *st = NULL;
        assert(ckv_open(&c, &st) == CKV_OK);
        assert(ckv_put(st, "a", 1, "old", 3) == CKV_OK);
        io_at = n;
        enum ckv_err rc = ckv_put(st, "b", 1, "new", 3);
        int hit = io_hit;
        reset_faults();
        assert(rc == (hit ? CKV_EIO : CKV_OK));
        if (hit) check_failed_handle(st);
        ckv_close(st);
        assert(ckv_open(&c, &st) == CKV_OK);
        expect(st, "a", "old");
        char out[16]; size_t len;
        rc = ckv_get(st, "b", 1, out, sizeof(out), &len);
        assert(rc == CKV_OK || (hit && rc == CKV_ENOT_FOUND));
        if (rc == CKV_OK) assert(len == 3 && memcmp(out, "new", 3) == 0);
        assert(ckv_put(st, "c", 1, "after", 5) == CKV_OK);
        ckv_close(st);
        assert(ckv_open(&c, &st) == CKV_OK);
        expect(st, "c", "after");
        ckv_close(st);
        assert(unlink(path) == 0);
        if (!hit) return;
    }
    assert(!"write fault loop did not terminate");
}

static void allocation_before_append(const char *path) {
    /* The 12th key grows the initial 16-slot index, then allocates its key. */
    for (int n = 1; n <= 3; n++) {
        reset_faults();
        struct ckv_config c = config(path);
        struct ckv_store *st = NULL;
        assert(ckv_open(&c, &st) == CKV_OK);
        for (unsigned char k = 0; k < 11; k++)
            assert(ckv_put(st, &k, 1, "x", 1) == CKV_OK);
        off_t before = file_size(path);
        alloc_at = n;
        enum ckv_err rc = ckv_put(st, "z", 1, "new", 3);
        int hit = alloc_hit;
        reset_faults();
        assert(rc == (hit ? CKV_ENOMEM : CKV_OK));
        if (hit) {
            assert(file_size(path) == before);
            assert(ckv_put(st, "z", 1, "retry", 5) == CKV_OK);
            expect(st, "z", "retry");
        }
        ckv_close(st);
        assert(ckv_open(&c, &st) == CKV_OK);
        expect(st, "z", hit ? "retry" : "new");
        ckv_close(st);
        assert(unlink(path) == 0);
    }
}

static void recovery_faults(const char *path) {
    for (int n = 1; n < 20; n++) {
        reset_faults();
        struct ckv_config c = config(path);
        c.fsync_policy = CKV_FSYNC_OFF;
        struct ckv_store *st = NULL;
        assert(ckv_open(&c, &st) == CKV_OK);
        assert(ckv_put(st, "a", 1, "old", 3) == CKV_OK);
        assert(ckv_put(st, "b", 1, "new", 3) == CKV_OK);
        ckv_close(st);
        assert(truncate(path, 43) == 0); /* last byte missing */
        io_at = n;
        enum ckv_err rc = ckv_open(&c, &st);
        int hit = io_hit;
        reset_faults();
        assert(rc == (hit ? CKV_EIO : CKV_OK));
        if (hit) assert(st == NULL);
        ckv_close(st);
        assert(ckv_open(&c, &st) == CKV_OK);
        expect(st, "a", "old");
        assert(file_size(path) == 22);
        ckv_close(st);
        assert(unlink(path) == 0);
        if (!hit) return;
    }
    assert(!"recovery fault loop did not terminate");
}

int main(void) {
    char dir[] = "/tmp/ckvstore_faults.XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", dir);
    compaction_faults(path, 0);
    compaction_faults(path, 1);
    write_faults(path);
    allocation_before_append(path);
    recovery_faults(path);

    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    zero_write = 1;
    assert(ckv_pwrite_full(fd, "abc", 3, 0) == -1 && errno == EIO);
    short_write = 1;
    assert(ckv_pwrite_full(fd, "abc", 3, 0) == 0);
    short_write = 0;
    char buf[3];
    interrupt_read = 1;
    assert(ckv_pread_full(fd, buf, 3, 0) == 0 && memcmp(buf, "abc", 3) == 0);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0); /* failed compaction must not leave temporary files */
    puts("ALL FAULT TESTS PASSED");
    return 0;
}
