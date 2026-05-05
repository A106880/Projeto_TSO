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

static void save_block_entry(gpointer key, gpointer value, gpointer user_data) {
    SaveCtx *ctx = (SaveCtx *)user_data;
    if (ctx->error) return;

    (void)key;
    blockmeta *bm = (blockmeta *)value;

    if (write_string(ctx->f, bm->id) != 0)                      { ctx->error = 1; return; }
    if (write_uint64(ctx->f, (uint64_t)bm->size) != 0)          { ctx->error = 1; return; }
    if (write_int64(ctx->f, (int64_t)bm->block_offset) != 0)    { ctx->error = 1; return; }
    if (write_uint64(ctx->f, (uint64_t)bm->counter) != 0)       { ctx->error = 1; return; }
}

static void save_file_entry(gpointer key, gpointer value, gpointer user_data) {
    (void)key;
    SaveCtx *ctx = (SaveCtx *)user_data;
    if (ctx->error) return;

    filemeta *fm = (filemeta *)value;

    if (write_string(ctx->f, fm->id) != 0)                          { ctx->error = 1; return; }
    if (write_int64(ctx->f, (int64_t)fm->realSize) != 0)            { ctx->error = 1; return; }
    if (write_int64(ctx->f, (int64_t)fm->logicalSize) != 0)         { ctx->error = 1; return; }

    uint64_t nblocks = (uint64_t)g_queue_get_length(fm->blockList);
    if (write_uint64(ctx->f, nblocks) != 0) { ctx->error = 1; return; }

    GList *node = g_queue_peek_head_link(fm->blockList);
    while (node) {
        blockmeta *bm = (blockmeta *)node->data;
        if (write_string(ctx->f, bm->id) != 0) { ctx->error = 1; return; }
        node = node->next;
    }
}

void save_metadata(GHashTable *fileIndex, GHashTable *blockIndex, GQueue *freeList, off_t next_free_offset) {
    FILE *f = fopen(PERSISTENCE_FILE, "wb");
    if (!f) return;

    SaveCtx ctx = { f, 0 };

    if (write_int64(f, (int64_t)next_free_offset) != 0) { fclose(f); return; }

    uint64_t nblocks = (uint64_t)g_hash_table_size(blockIndex);
    if (write_uint64(f, nblocks) != 0) { fclose(f); return; }
    g_hash_table_foreach(blockIndex, save_block_entry, &ctx);
    if (ctx.error) { fclose(f); return; }

    uint64_t nfiles = (uint64_t)g_hash_table_size(fileIndex);
    if (write_uint64(f, nfiles) != 0) { fclose(f); return; }
    g_hash_table_foreach(fileIndex, save_file_entry, &ctx);
    if (ctx.error) { fclose(f); return; }

    uint64_t nfree = (uint64_t)g_queue_get_length(freeList);
    if (write_uint64(f, nfree) != 0) { fclose(f); return; }
    GList *node = g_queue_peek_head_link(freeList);
    while (node) {
        off_t *off = (off_t *)node->data;
        if (write_int64(f, (int64_t)*off) != 0) { fclose(f); return; }
        node = node->next;
    }

    fclose(f);
}

void load_metadata(GHashTable *fileIndex, GHashTable *blockIndex, GQueue *freeList, off_t *next_free_offset) {
    FILE *f = fopen(PERSISTENCE_FILE, "rb");
    if (!f) return;

    int64_t saved_offset;
    if (read_int64(f, &saved_offset) != 0) { fclose(f); return; }
    *next_free_offset = (off_t)saved_offset;

    uint64_t nblocks;
    if (read_uint64(f, &nblocks) != 0) { fclose(f); return; }

    for (uint64_t i = 0; i < nblocks; i++) {
        char *id = read_string(f);
        if (!id) goto err;

        uint64_t bsize;
        if (read_uint64(f, &bsize) != 0) { g_free(id); goto err; }

        int64_t boffset;
        if (read_int64(f, &boffset) != 0) { g_free(id); goto err; }

        uint64_t bcounter;
        if (read_uint64(f, &bcounter) != 0) { g_free(id); goto err; }

        blockmeta *bm = g_malloc0(sizeof(blockmeta));
        bm->id           = id;
        bm->size         = (size_t)bsize;
        bm->block_offset = (off_t)boffset;
        bm->counter      = (int)bcounter;

        g_hash_table_insert(blockIndex, bm->id, bm);
    }

    uint64_t nfiles;
    if (read_uint64(f, &nfiles) != 0) { fclose(f); return; }

    for (uint64_t i = 0; i < nfiles; i++) {
        char *id = read_string(f);
        if (!id) goto err;

        int64_t realSize, logicalSize;
        if (read_int64(f, &realSize) != 0)    { g_free(id); goto err; }
        if (read_int64(f, &logicalSize) != 0) { g_free(id); goto err; }

        uint64_t nfile_blocks;
        if (read_uint64(f, &nfile_blocks) != 0) { g_free(id); goto err; }

        filemeta *fm = g_malloc0(sizeof(filemeta));
        fm->id          = id;
        fm->realSize    = (off_t)realSize;
        fm->logicalSize = (off_t)logicalSize;
        fm->blockList   = g_queue_new();
        pthread_mutex_init(&fm->lock, NULL);

        for (uint64_t j = 0; j < nfile_blocks; j++) {
            char *block_id = read_string(f);
            if (!block_id) { freeFilemeta(fm); goto err; }

            blockmeta *bm = g_hash_table_lookup(blockIndex, block_id);
            g_free(block_id);
            if (!bm) { freeFilemeta(fm); goto err; }

            g_queue_push_tail(fm->blockList, bm);
        }

        g_hash_table_insert(fileIndex, fm->id, fm);
    }

    uint64_t nfree;
    if (read_uint64(f, &nfree) != 0) { fclose(f); return; }

    for (uint64_t i = 0; i < nfree; i++) {
        int64_t off;
        if (read_int64(f, &off) != 0) goto err;
        off_t *poff = g_malloc(sizeof(off_t));
        *poff = (off_t)off;
        g_queue_push_tail(freeList, poff);
    }

    fclose(f);
    return;

err:
    fclose(f);
}
