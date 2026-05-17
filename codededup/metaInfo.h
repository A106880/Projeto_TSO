#ifndef METAINFO_H
#define METAINFO_H

#include <glib.h>
#include <sys/types.h>
#include <pthread.h>
#include "ticket_rwlock.h"

typedef struct file{
    char *id; //path
    off_t realSize; //tamanho depois da deduplicacao
    off_t logicalSize; // tamanho antes da deduplicacao
    GPtrArray *blockList; // Array de apontadores para o bloco
    ticket_rwlock_t lock; // lock para operações neste ficheiro
} filemeta;

typedef struct block{
    off_t block_offset; //offset
    char *id; // id é a hash SHA-512
    size_t size; // tamanho do bloco
    int counter; // contador de dependencias do bloco
    ticket_rwlock_t lock; // lock para operações neste bloco
} blockmeta;

void freeFilemeta(void *data);
void freeBlockMeta(void *data);

guint blockHashFunc(gconstpointer key);
gboolean compareSHAHashes(gconstpointer key1, gconstpointer key2);

#endif
