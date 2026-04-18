#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "metaInfo.h"
#include <glib.h>

#define PERSISTENCE_FILE "/backend/.metadata"

void save_metadata(GHashTable *fileIndex, GHashTable *blockCounter);

void load_metadata(GHashTable *fileIndex, GHashTable *blockCounter);

#endif
