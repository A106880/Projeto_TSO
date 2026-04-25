#ifndef METAINFO_H
#define METAINFO_H

#include <glib.h>
#include <sys/types.h>


typedef struct file{
    char *id; //path
    off_t realSize; //tamanho depois da deduplicacao
    off_t logicalSize; // tamanho antes da deduplicacao
    GQueue *blockList; // value é o id do block
    pthread_mutex_t lock; // lock para operações neste ficheiro
} filemeta;

typedef struct block{
    off_t block_offset; //offset
    char *id; // id é a hash SHA-512
    size_t size; // tamanho do bloco
    int counter; // contador de dependencias do bloco
} blockmeta;

// struct GHashTable fileIndex; // key -> id do ficheiro  // value -> filemeta

//////////// struct GHashTable blockCounter; // key -> id do bloco  // value -> contador

// struct GHashTable blockIndex; // key -> id do bloco // value -> blockmeta

// struct GHashTable partialIndex; // key -> id do bloco  // value -> bloco

void freeFilemeta(void *data);

void freeBlockMeta(void *data);

guint blockHashFunc(gconstpointer key);

gboolean compareSHAHashes(gconstpointer key1, gconstpointer key2);

#endif
