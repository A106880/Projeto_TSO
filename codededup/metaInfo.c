#include "metaInfo.h"

void freeFilemeta(void *data){
    if (!data) return;
	filemeta *file = (filemeta*)data;
	g_ptr_array_free(file->blockList, TRUE);
    if (file->id) g_free(file->id);
    ticket_rwlock_destroy(&file->lock);
    g_free(file);
}

void freeBlockMeta(void *data){
    if (!data) return;
    blockmeta *block = (blockmeta *)data;
    if (block->id) g_free(block->id);
    ticket_rwlock_destroy(&block->lock);
    g_free(block);
}

guint blockHashFunc(gconstpointer key){
    const guint32 *p = (const guint32 *)key;
    guint h = 0;
    for (int i = 0; i < 4; i++) {
        h ^= p[i];
    }
    return h;
}

gboolean compareSHAHashes(gconstpointer hash1, gconstpointer hash2){
    if (memcmp(hash1, hash2, 64) == 0) {
        return TRUE;
    } else {
        return FALSE;
    }
}
