#ifndef PERSISTANCE_H
#define PERSISTANCE_H

#include "metaInfo.h"
#include <glib.h>

#define PERSISTENCE_FILE "/backend/.metadata"

void save_metadata(GHashTable *fileIndex, GHashTable *blockIndex, GQueue *freeList, off_t next_free_offset);
void load_metadata(GHashTable *fileIndex, GHashTable *blockIndex, GQueue *freeList, off_t *next_free_offset);

#endif
