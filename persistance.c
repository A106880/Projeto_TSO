#include "persistance.h"
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

static void save_file_entry(gpointer key, gpointer value, gpointer user_data) {
    (void)key;
    SaveCtx *ctx = (SaveCtx *)user_data;
    if (ctx->error) return;

    filemeta *fm = (filemeta *)value;

    if (write_string(ctx->f, fm->id) != 0)         { ctx->error = 1; return; }
    if (write_int64(ctx->f, (int64_t)fm->realSize) != 0)    { ctx->error = 1; return; }
    if (write_int64(ctx->f, (int64_t)fm->logicalSize) != 0) { ctx->error = 1; return; }

    uint64_t nblocks = (uint64_t)g_queue_get_length(fm->blockList);
    if (write_uint64(ctx->f, nblocks) != 0) { ctx->error = 1; return; }

    GList *node = g_queue_peek_head_link(fm->blockList);
    while (node) {
        blockmeta *bm = (blockmeta *)node->data;
        if (write_bytes(ctx->f, bm->id, 64) != 0)              { ctx->error = 1; return; }
        if (write_uint64(ctx->f, (uint64_t)bm->size) != 0)     { ctx->error = 1; return; }
        node = node->next;
    }
}

static void save_block_entry(gpointer key, gpointer value, gpointer user_data) {
    SaveCtx *ctx = (SaveCtx *)user_data;
    if (ctx->error) return;

    if (write_bytes(ctx->f, key, 64) != 0)                          { ctx->error = 1; return; }
    if (write_uint64(ctx->f, (uint64_t)GPOINTER_TO_INT(value)) != 0) { ctx->error = 1; return; }
}

void save_metadata(GHashTable *fileIndex, GHashTable *blockCounter) {
    FILE *f = fopen(PERSISTENCE_FILE, "wb");
    if (!f) return;

    SaveCtx ctx = { f, 0 };

    uint64_t nfiles = (uint64_t)g_hash_table_size(fileIndex);
    if (write_uint64(f, nfiles) != 0) { fclose(f); return; }
    g_hash_table_foreach(fileIndex, save_file_entry, &ctx);

    uint64_t nblocks = (uint64_t)g_hash_table_size(blockCounter);
    if (!ctx.error && write_uint64(f, nblocks) != 0) ctx.error = 1;
    if (!ctx.error) g_hash_table_foreach(blockCounter, save_block_entry, &ctx);

    fclose(f);
}

void load_metadata(GHashTable *fileIndex, GHashTable *blockCounter) {
    FILE *f = fopen(PERSISTENCE_FILE, "rb");
    if (!f) return;

    uint64_t nfiles;
    if (read_uint64(f, &nfiles) != 0) { fclose(f); return; }

    for (uint64_t i = 0; i < nfiles; i++) {
        char *id = read_string(f);
        if (!id) goto err;

        int64_t realSize, logicalSize;
        if (read_int64(f, &realSize) != 0)    { g_free(id); goto err; }
        if (read_int64(f, &logicalSize) != 0) { g_free(id); goto err; }

        uint64_t nblocks;
        if (read_uint64(f, &nblocks) != 0) { g_free(id); goto err; }

        filemeta *fm = g_malloc0(sizeof(filemeta));
        fm->id          = id;
        fm->realSize    = (off_t)realSize;
        fm->logicalSize = (off_t)logicalSize;
        fm->blockList   = g_queue_new();

        for (uint64_t j = 0; j < nblocks; j++) {
            blockmeta *bm = g_malloc0(sizeof(blockmeta));
            bm->id = g_malloc0(64);
            if (read_bytes(f, bm->id, 64) != 0) {
                freeBlockMeta(bm);
                freeFilemeta(fm);
                goto err;
            }
            uint64_t bsize;
            if (read_uint64(f, &bsize) != 0) {
                freeBlockMeta(bm);
                freeFilemeta(fm);
                goto err;
            }
            bm->size = (size_t)bsize;
            g_queue_push_tail(fm->blockList, bm);
        }

        g_hash_table_insert(fileIndex, (gpointer)fm->id, fm);
    }

    uint64_t nblocks;
    if (read_uint64(f, &nblocks) != 0) { fclose(f); return; }

    for (uint64_t i = 0; i < nblocks; i++) {
        unsigned char *hash = g_malloc0(64);
        if (read_bytes(f, hash, 64) != 0) { g_free(hash); goto err; }
        uint64_t counter;
        if (read_uint64(f, &counter) != 0) { g_free(hash); goto err; }
        g_hash_table_insert(blockCounter, hash, GINT_TO_POINTER((int)counter));
    }

    fclose(f);
    return;

err:
    fclose(f);
}
