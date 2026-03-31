#include <glib.h>


typedef struct file{
    int id;
    off_t realSize;
    off_t logicalSize;
    struct GQueue blockList;
} filemeta;

typedef struct block{
    char *in_buf; //apontador para a localizacao em memoria do bloco
    size_t in_size; // tamanho do bloco
} blockmeta;

struct GHashTable fileIndex; // key -> id do ficheiro  // value -> filemeta

struct GHashTable blockCounter; // key -> id do bloco  // value -> contador

struct GHashTable partialIndex; // key -> id do bloco  // value -> bloco

