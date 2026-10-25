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
    T_ASSERT(out_len == strlen(v2) && memcmp(out, v2, out_len) == 0);

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

    T_ASSERT(fsize(path) == 20); /* repaired to the end of record a */
    T_ASSERT(ckv_put(st, "c", 1, "3", 1) == CKV_OK);
    ckv_close(st);
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(ckv_get(st, "c", 1, out, sizeof(out), &out_len) == CKV_OK);
    T_ASSERT(out_len == 1 && out[0] == '3');
    ckv_close(st);
    rmrf(d);
    free(d);
}

static void test_capacity_and_reopen(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);
    struct ckv_config cfg = default_cfg(path);
    cfg.max_index_entries = 1;
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(ckv_put(st, "a", 1, "1", 1) == CKV_OK);
    T_ASSERT(ckv_put(st, "a", 1, "2", 1) == CKV_OK);
    off_t before = fsize(path);
    T_ASSERT(ckv_put(st, "b", 1, "3", 1) == CKV_EFULL);
    T_ASSERT(fsize(path) == before);
    ckv_close(st);
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    char out[8]; size_t len = 0;
    T_ASSERT(ckv_get(st, "a", 1, out, sizeof(out), &len) == CKV_OK);
    T_ASSERT(len == 1 && out[0] == '2');
    T_ASSERT(ckv_get(st, "b", 1, out, sizeof(out), &len) == CKV_ENOT_FOUND);
    T_ASSERT(ckv_del(st, "a", 1) == CKV_OK);
    T_ASSERT(ckv_put(st, "b", 1, NULL, 0) == CKV_OK);
    ckv_close(st);
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(ckv_get(st, "b", 1, out, sizeof(out), &len) == CKV_OK && len == 0);
    ckv_close(st);
    rmrf(d); free(d);
}

static void test_corruption_is_reported(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);
    struct ckv_config cfg = default_cfg(path);
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(ckv_put(st, "a", 1, "1", 1) == CKV_OK);
    T_ASSERT(ckv_put(st, "b", 1, "2", 1) == CKV_OK);
    int fd = open(path, O_RDWR);
    T_ASSERT(fd >= 0);
    T_ASSERT(pwrite(fd, "X", 1, 19) == 1); /* corrupt first value, not the tail */
    T_ASSERT(close(fd) == 0);
    char out[8]; size_t len;
    T_ASSERT(ckv_get(st, "a", 1, out, sizeof(out), &len) == CKV_ECORRUPT);
    T_ASSERT(ckv_compact(st) == CKV_ECORRUPT);
    T_ASSERT(fsize(path) == 40);
    T_ASSERT(ckv_get(st, "b", 1, out, sizeof(out), &len) == CKV_OK);
    T_ASSERT(out[0] == '2');
    ckv_close(st);
    T_ASSERT(ckv_open(&cfg, &st) == CKV_ECORRUPT && st == NULL);
    T_ASSERT(fsize(path) == 40);
    rmrf(d); free(d);
}

static void test_lower_limits_do_not_truncate(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);
    struct ckv_config cfg = default_cfg(path);
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(ckv_put(st, "key", 3, "value", 5) == CKV_OK);
    ckv_close(st);
    cfg.max_value_bytes = 4;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_EFULL);
    T_ASSERT(fsize(path) == 26);
    cfg.max_value_bytes = 1024;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    ckv_close(st);
    rmrf(d); free(d);
}

static void test_iterator_retry_and_permissions(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);
    struct ckv_config cfg = default_cfg(path);
    cfg.fsync_policy = CKV_FSYNC_ON_COMMIT;
    struct ckv_store *st = NULL;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(chmod(path, 0600) == 0);
    const unsigned char key[] = {0, 255, 3};
    T_ASSERT(ckv_put(st, key, sizeof(key), NULL, 0) == CKV_OK);
    struct ckv_iter it;
    unsigned char out[3]; size_t len;
    T_ASSERT(ckv_iter_begin(st, &it) == CKV_OK);
    T_ASSERT(ckv_iter_next(st, &it, NULL, 0, &len) == CKV_OK && len == 3);
    T_ASSERT(ckv_iter_next(st, &it, out, 2, &len) == CKV_EBUF_TOO_SMALL);
    T_ASSERT(ckv_iter_next(st, &it, out, 3, &len) == CKV_OK);
    T_ASSERT(memcmp(out, key, 3) == 0);
    T_ASSERT(ckv_iter_next(st, &it, out, 3, &len) == CKV_ENOT_FOUND);
    T_ASSERT(ckv_compact(st) == CKV_OK);
    struct stat sb;
    T_ASSERT(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600);
    ckv_close(st);
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    T_ASSERT(ckv_get(st, key, 3, out, sizeof(out), &len) == CKV_OK && len == 0);
    ckv_close(st);
    rmrf(d); free(d);
}

static void test_model_with_reopens(void) {
    char *d = mktempdir();
    T_ASSERT(d != NULL);
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/store.log", d);
    struct ckv_config cfg = default_cfg(path);
    cfg.initial_index_capacity = 16;
    struct ckv_store *st = NULL;
    unsigned char present[64] = {0}, values[64] = {0};
    uint32_t seed = 12345;
    T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
    for (unsigned step = 0; step < 2000; step++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned char key = (unsigned char)((seed >> 16) % 64);
        if ((seed >> 24) % 3 == 0) {
            T_ASSERT(ckv_del(st, &key, 1) == CKV_OK);
            present[key] = 0;
        } else {
            values[key] = (unsigned char)(seed >> 8);
            T_ASSERT(ckv_put(st, &key, 1, &values[key], 1) == CKV_OK);
            present[key] = 1;
        }
        if (step % 79 == 0) T_ASSERT(ckv_compact(st) == CKV_OK);
        if (step % 137 == 0) {
            ckv_close(st);
            T_ASSERT(ckv_open(&cfg, &st) == CKV_OK);
        }
        for (unsigned char k = 0; k < 64; k++) {
            unsigned char out; size_t len;
            enum ckv_err rc = ckv_get(st, &k, 1, &out, 1, &len);
            T_ASSERT(rc == (present[k] ? CKV_OK : CKV_ENOT_FOUND));
            if (present[k]) T_ASSERT(len == 1 && out == values[k]);
        }
    }
    ckv_close(st);
    rmrf(d); free(d);
}

int main(void) {
    test_basic_put_get_del();
    test_overwrite_semantics();
    test_recovery_after_reopen();
    test_compaction_removes_stale();
    test_corrupt_tail_tolerated();
    test_capacity_and_reopen();
    test_corruption_is_reported();
    test_lower_limits_do_not_truncate();
    test_iterator_retry_and_permissions();
    test_model_with_reopens();

    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
