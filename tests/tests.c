#define _GNU_SOURCE
#include "ckvstore.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures = 0;

#define T_ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
        return; \
    } \
} while (0)

static char *mktempdir(void) {
    char tmpl[] = "/tmp/ckvstore_test.XXXXXX";
    char *d = strdup(tmpl);
    if (!d) return NULL;
    if (!mkdtemp(d)) { free(d); return NULL; }
    return d;
}

static void rmrf(const char *dir) {
    if (!dir) return;
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", dir);
    (void)unlink(path);
    (void)rmdir(dir);
}

static struct ckv_config default_cfg(const char *path) {
    struct ckv_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.path = path;
    cfg.max_key_bytes = 64;
    cfg.max_value_bytes = 1024;
    cfg.max_index_entries = 1024;
    cfg.initial_index_capacity = 64;
    cfg.fsync_policy = CKV_FSYNC_OFF;
    cfg.fsync_period_records = 16;
    return cfg;
}

static void test_basic_put_get_del(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);

    struct ckv_store *st = NULL;
    struct ckv_config cfg = default_cfg(path);

    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    const char *k = "k1";
    const char *v = "hello";
    T_ASSERT(ckv_put(st, k, strlen(k), v, strlen(v)) == CKV_OK);

    char out[64];
    size_t out_len = 0;
    T_ASSERT(ckv_get(st, k, strlen(k), out, sizeof(out), &out_len) == CKV_OK);
    T_ASSERT(out_len == strlen(v));
    T_ASSERT(memcmp(out, v, out_len) == 0);

    T_ASSERT(ckv_del(st, k, strlen(k)) == CKV_OK);
    T_ASSERT(ckv_get(st, k, strlen(k), out, sizeof(out), &out_len) == CKV_ENOT_FOUND);

    ckv_close(st);
    rmrf(d);
    free(d);
}

static void test_overwrite_semantics(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);

    struct ckv_store *st = NULL;
    struct ckv_config cfg = default_cfg(path);
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);

    const char *k = "key";
    const char *v1 = "one";
    const char *v2 = "two";
    T_ASSERT(ckv_put(st, k, strlen(k), v1, strlen(v1)) == CKV_OK);
    T_ASSERT(ckv_put(st, k, strlen(k), v2, strlen(v2)) == CKV_OK);

    char out[64];
    size_t out_len = 0;
    T_ASSERT(ckv_get(st, k, strlen(k), out, sizeof(out), &out_len) == CKV_OK);
    T_ASSERT(out_len == strlen(v2));
    T_ASSERT(memcmp(out, v2, out_len) == 0);

    ckv_close(st);
    rmrf(d);
    free(d);
}

static void test_recovery_after_reopen(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);

    struct ckv_config cfg = default_cfg(path);
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);

    const char *k = "k";
    const char *v = "persist";
    T_ASSERT(ckv_put(st, k, strlen(k), v, strlen(v)) == CKV_OK);
    ckv_close(st);

    st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);

    char out[64];
    size_t out_len = 0;
    T_ASSERT(ckv_get(st, k, strlen(k), out, sizeof(out), &out_len) == CKV_OK);
    T_ASSERT(out_len == strlen(v));
    T_ASSERT(memcmp(out, v, out_len) == 0);

    ckv_close(st);
    rmrf(d);
    free(d);
}

static off_t fsize(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return st.st_size;
}

static void test_compaction_removes_stale(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);

    struct ckv_config cfg = default_cfg(path);
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);

    const char *k = "k";
    const char *v1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *v2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    for (int i = 0; i < 50; i++) {
        const char *v = (i % 2 == 0) ? v1 : v2;
        T_ASSERT(ckv_put(st, k, strlen(k), v, strlen(v)) == CKV_OK);
    }
    T_ASSERT(ckv_put(st, "x", 1, "y", 1) == CKV_OK);
    T_ASSERT(ckv_del(st, "x", 1) == CKV_OK);

    off_t before = fsize(path);
    T_ASSERT(before > 0);

    T_ASSERT(ckv_compact(st) == CKV_OK);
    off_t after = fsize(path);
    T_ASSERT(after > 0);
    T_ASSERT(after < before);

    char out[128];
    size_t out_len = 0;
    T_ASSERT(ckv_get(st, k, strlen(k), out, sizeof(out), &out_len) == CKV_OK);

    ckv_close(st);
    rmrf(d);
    free(d);
}

static void test_corrupt_tail_tolerated(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);

    struct ckv_config cfg = default_cfg(path);
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);

    T_ASSERT(ckv_put(st, "a", 1, "1", 1) == CKV_OK);
    T_ASSERT(ckv_put(st, "b", 1, "2", 1) == CKV_OK);
    ckv_close(st);

    /* Corrupt tail by truncating a few bytes. */
    off_t sz = fsize(path);
    T_ASSERT(sz > 5);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    T_ASSERT(fd >= 0);
    T_ASSERT(ftruncate(fd, sz - 3) == 0);
    close(fd);

    st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);

    char out[8];
    size_t out_len = 0;
    T_ASSERT(ckv_get(st, "a", 1, out, sizeof(out), &out_len) == CKV_OK);
    T_ASSERT(out_len == 1 && out[0] == '1');

    /* "b" might be lost if truncation hit its record; but open must succeed. */
    ckv_close(st);
    rmrf(d);
    free(d);
}

int main(void) {
    test_basic_put_get_del();
    test_overwrite_semantics();
    test_recovery_after_reopen();
    test_compaction_removes_stale();
    test_corrupt_tail_tolerated();

    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
