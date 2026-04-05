#include "persistence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

static int write_uint64(FILE *f, uint64_t v) {
    return fwrite(&v, sizeof(uint64_t), 1, f) == 1 ? 0 : -1;
}

static int write_int64(FILE *f, int64_t v) {
    return fwrite(&v, sizeof(int64_t), 1, f) == 1 ? 0 : -1;
}

static int write_bytes(FILE *f, const void *buf, size_t len) {
    return fwrite(buf, 1, len, f) == len ? 0 : -1;
}

static int write_string(FILE *f, const char *s) {
    uint64_t len = s ? (uint64_t)strlen(s) : 0;
    if (write_uint64(f, len) != 0) return -1;
    if (len > 0 && write_bytes(f, s, (size_t)len) != 0) return -1;
    return 0;
}

static int read_uint64(FILE *f, uint64_t *out) {
    return fread(out, sizeof(uint64_t), 1, f) == 1 ? 0 : -1;
}

static int read_int64(FILE *f, int64_t *out) {
    return fread(out, sizeof(int64_t), 1, f) == 1 ? 0 : -1;
}

static int read_bytes(FILE *f, void *buf, size_t len) {
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

static char *read_string(FILE *f) {
    uint64_t len;
    if (read_uint64(f, &len) != 0) return NULL;
    if (len == 0) return g_strdup("");
    char *s = g_malloc0(len + 1);
    if (read_bytes(f, s, (size_t)len) != 0) {
        g_free(s);
        return NULL;
    }
    s[len] = '\0';
    return s;
}

typedef struct {
    FILE *f;
    int   error;
} SaveCtx;