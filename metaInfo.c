#include "metaInfo.h"

void freeFilemeta(void *data){
    if (!data) return;
	filemeta *file = (filemeta*)data;
	g_queue_free(file->blockList); //value da block list é um apontador para blockmeta
    if (file->id) g_free(file->id);
    g_free(file);
}

void freeBlockMeta(void *data){
    if (!data) return;
    blockmeta *block = (blockmeta *)data;
    // free(block->in_buf);
    if (block->id) g_free(block->id);
    g_free(block);
}

guint blockHashFunc(gconstpointer key){
    guint *value = (guint *) key;

    guint finalKey = *value;

    return finalKey;
}


gboolean compareSHAHashes(gconstpointer hash1, gconstpointer hash2){
    if (memcmp(hash1, hash2, 64) == 0) {
        return TRUE;
    } else {
        return FALSE;
    }
}