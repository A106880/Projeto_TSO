#include <glib.h>


typedef struct file{
    char *id; //path
    off_t realSize; //tamanho depois da deduplicacao
    off_t logicalSize; // tamanho antes da deduplicacao
    GQueue *blockList; // value é o id do block
} filemeta;

typedef struct block{
    int block_offset; //offset do bloco
    char *id; // id é a hash SHA-512
    size_t size; // tamanho do bloco
    char *blockPath; // caminho do bloco no fuse
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

