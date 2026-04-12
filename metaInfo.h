#ifndef METAINFO_H
#define METAINFO_H

#include <glib.h>


typedef struct file{
    char *id; //path
    off_t realSize; //tamanho depois da deduplicacao
    off_t logicalSize; // tamanho antes da deduplicacao
    GQueue *blockList; // value é o blockmeta
} filemeta;

typedef struct block{
    char *in_buf; //apontador para a localizacao em memoria do bloco
    char *id; // id é a hash SHA-512
    size_t size; // tamanho do bloco
} blockmeta;

// struct GHashTable fileIndex; // key -> id do ficheiro  // value -> filemeta

// struct GHashTable blockCounter; // key -> id do bloco  // value -> contador

// struct GHashTable partialIndex; // key -> id do bloco  // value -> bloco

void freeFilemeta(void *data);

void freeBlockMeta(void *data);

guint blockHashFunc(gconstpointer key);

gboolean compareSHAHashes(gconstpointer key1, gconstpointer key2);

#endif
