#include "ckvstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <file> get <key>\n"
        "  %s <file> put <key> <value>\n"
        "  %s <file> del <key>\n"
        "  %s <file> compact\n",
        argv0, argv0, argv0, argv0);
}

static int hexdump(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    const char *path = argv[1];
    const char *cmd = argv[2];

    struct ckv_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.path = path;
    cfg.max_key_bytes = 1024;
    cfg.max_value_bytes = 1024 * 1024;
    cfg.max_index_entries = 100000;
    cfg.initial_index_capacity = 1024;
    cfg.fsync_policy = CKV_FSYNC_OFF;
    cfg.fsync_period_records = 1024;

    struct ckv_store *st = NULL;
    enum ckv_err e = ckv_open(&cfg, &st);
    if (e != CKV_OK) {
        fprintf(stderr, "open failed: %s\n", ckv_strerror(e));
        return 1;
    }

    int rc = 0;
    if (strcmp(cmd, "get") == 0) {
        if (argc != 4) { usage(argv[0]); rc = 2; goto out; }
        const char *key = argv[3];
        unsigned char buf[1024 * 1024];
        size_t out_len = 0;
        e = ckv_get(st, key, strlen(key), buf, sizeof(buf), &out_len);
        if (e == CKV_OK) {
            fwrite(buf, 1, out_len, stdout);
            fputc('\n', stdout);
        } else if (e == CKV_EBUF_TOO_SMALL) {
            fprintf(stderr, "buffer too small; need %zu bytes\n", out_len);
            rc = 1;
        } else {
            fprintf(stderr, "get failed: %s\n", ckv_strerror(e));
            rc = 1;
        }
    } else if (strcmp(cmd, "put") == 0) {
        if (argc != 5) { usage(argv[0]); rc = 2; goto out; }
        const char *key = argv[3];
        const char *val = argv[4];
        e = ckv_put(st, key, strlen(key), val, strlen(val));
        if (e != CKV_OK) { fprintf(stderr, "put failed: %s\n", ckv_strerror(e)); rc = 1; }
    } else if (strcmp(cmd, "del") == 0) {
        if (argc != 4) { usage(argv[0]); rc = 2; goto out; }
        const char *key = argv[3];
        e = ckv_del(st, key, strlen(key));
        if (e != CKV_OK) { fprintf(stderr, "del failed: %s\n", ckv_strerror(e)); rc = 1; }
    } else if (strcmp(cmd, "compact") == 0) {
        e = ckv_compact(st);
        if (e != CKV_OK) { fprintf(stderr, "compact failed: %s\n", ckv_strerror(e)); rc = 1; }
    } else if (strcmp(cmd, "iter") == 0) {
        struct ckv_iter it;
        e = ckv_iter_begin(st, &it);
        if (e != CKV_OK) { fprintf(stderr, "iter_begin: %s\n", ckv_strerror(e)); rc = 1; goto out; }
        unsigned char kbuf[1024];
        size_t klen = 0;
        while ((e = ckv_iter_next(st, &it, kbuf, sizeof(kbuf), &klen)) == CKV_OK) {
            printf("key[%zu]=", klen);
            hexdump(kbuf, klen);
            printf("\n");
        }
        if (e != CKV_ENOT_FOUND) { fprintf(stderr, "iter_next: %s\n", ckv_strerror(e)); rc = 1; }
        ckv_iter_end(st, &it);
    } else {
        usage(argv[0]);
        rc = 2;
    }

out:
    ckv_close(st);
    return rc;
}
