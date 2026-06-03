/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. Its performance is terrible.
 *
 * Compile with
 *
 *     gcc -Wall passthrough.c `pkg-config fuse3 --cflags --libs` -o passthrough
 *
 * ## Source code ##
 * \include passthrough.c
 */


#define FUSE_USE_VERSION 31

#define _GNU_SOURCE

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <stdlib.h>
#include <pthread.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "passthrough_helpers.h"
#include "metaInfo.h"
#include "persistance.h"
#include <openssl/sha.h>
#include "ticket_rwlock.h"

// #define DEBUG "/tmp/debug.log"

typedef struct context {
	GHashTable *fileIndex;
	GHashTable *blockIndex;
    ticket_rwlock_t file_index_lock;
    ticket_rwlock_t block_index_lock;
    GArray *freeList;
    pthread_mutex_t freeList_lock;
    
    int backend_fd;
    off_t next_free_offset;
    pthread_mutex_t offset_lock;

    uint64_t open; 
} Context;

static int fill_dir_plus = 0;

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;


	//Initialize context
	Context *ctx=malloc(sizeof(Context));

	ctx->open=0;
	ctx->blockIndex = g_hash_table_new_full(blockHashFunc,compareSHAHashes,NULL,freeBlockMeta);
	ctx->fileIndex = g_hash_table_new_full(g_str_hash,g_str_equal,NULL,freeFilemeta);
	ctx->freeList = g_array_new(FALSE, FALSE, sizeof(off_t));

	ticket_rwlock_init(&ctx->file_index_lock);
	ticket_rwlock_init(&ctx->block_index_lock);
	pthread_mutex_init(&ctx->freeList_lock, NULL);
	pthread_mutex_init(&ctx->offset_lock, NULL);

    ctx->backend_fd = open("/backend/.sysdata", O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (ctx->backend_fd != -1) {
        struct stat st;
        if (fstat(ctx->backend_fd, &st) == 0) {
            ctx->next_free_offset = st.st_size;
        } else {
            ctx->next_free_offset = 0;
        }
    }

    struct fuse_context *f_ctx = fuse_get_context();
	printf("[Thread %d] Init called, userid %d, pid %d\n", gettid(), f_ctx->uid, f_ctx->pid);
	
    load_metadata(ctx->fileIndex, ctx->blockIndex, ctx->freeList, &ctx->next_free_offset);

	return ctx;
}

static void xmp_destroy(void* private_data){


	struct fuse_context *f_ctx = fuse_get_context();
	Context* p_ctx = (Context*) private_data;
    save_metadata(p_ctx->fileIndex, p_ctx->blockIndex, p_ctx->freeList, p_ctx->next_free_offset);
	ticket_rwlock_destroy(&p_ctx->file_index_lock);
	ticket_rwlock_destroy(&p_ctx->block_index_lock);
	pthread_mutex_destroy(&p_ctx->freeList_lock);
	pthread_mutex_destroy(&p_ctx->offset_lock);

    if (p_ctx->backend_fd != -1) close(p_ctx->backend_fd);

	if (p_ctx->freeList) {
        g_array_free(p_ctx->freeList, TRUE); 
    }
    
    if (p_ctx->fileIndex) {
        g_hash_table_destroy(p_ctx->fileIndex);
    }
    
    if (p_ctx->blockIndex) {
        g_hash_table_destroy(p_ctx->blockIndex);
    }

    printf("[Thread %d] Destroy called, userid %d, pid %d\n", gettid(), f_ctx->uid, f_ctx->pid);
	printf("[Thread %d] Open() - %lu\n", gettid(), p_ctx->open);

	free(private_data);

}

static int xmp_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
{
	if (strcmp(path, "/backend/.metadata") == 0 || strcmp(path, "/backend/.sysdata") == 0) {
        return -ENOENT;
    }
	(void) fi;
	int res;
    
    res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	struct fuse_context *f_ctx = fuse_get_context();
	Context *ctx = (Context *) f_ctx->private_data;

	ticket_rwlock_read_lock(&ctx->file_index_lock);
	filemeta *file = g_hash_table_lookup(ctx->fileIndex, path);
	if (file != NULL) {
        ticket_rwlock_read_lock(&file->lock);
		stbuf->st_size = file->logicalSize;
		stbuf->st_blocks = (file->logicalSize + 511) / 512;
        ticket_rwlock_read_unlock(&file->lock);
	}

    // printf("[GETATTR] path: %s | tamanho_fisico: %ld | tamanho_logico: %ld\n", 
        // path, stbuf->st_size, file ? file->logicalSize : stbuf->st_size);
	ticket_rwlock_read_unlock(&ctx->file_index_lock);

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;


	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;

	dp = opendir(path);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		if (strcmp(de->d_name, ".metadata") == 0 || strcmp(de->d_name, ".sysdata") == 0) {
			continue;
		}
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0, fill_dir_plus))
			break;
	}

	closedir(dp);
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;

	res = mknod_wrapper(AT_FDCWD, path, NULL, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	if (strcmp(path, "/backend/.metadata") == 0 || strcmp(path, "/backend/.sysdata") == 0) {
	    return -EPERM;
	}
    struct fuse_context *f_ctx = fuse_get_context();
    Context *ctx = (Context *) f_ctx->private_data;

    ticket_rwlock_write_lock(&ctx->file_index_lock);
    filemeta *file = g_hash_table_lookup(ctx->fileIndex, path);
    if (file == NULL) {
        ticket_rwlock_write_unlock(&ctx->file_index_lock);
        return -ENOENT;
    }
    g_hash_table_steal(ctx->fileIndex, path);
    ticket_rwlock_write_unlock(&ctx->file_index_lock);

    ticket_rwlock_write_lock(&file->lock);
    int res = unlink(path);
    if (res == -1 && errno != ENOENT) {
        int err = -errno;
        ticket_rwlock_write_unlock(&file->lock);
        ticket_rwlock_write_lock(&ctx->file_index_lock);
        g_hash_table_insert(ctx->fileIndex, g_strdup(path), file);
        ticket_rwlock_write_unlock(&ctx->file_index_lock);
        return err;
    }

    ticket_rwlock_write_lock(&ctx->block_index_lock);

    GHashTable *blockCounts = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int i = 0; i < file->blockList->len; i++) {
        blockmeta *block = (blockmeta *)g_ptr_array_index(file->blockList, i);
        gpointer val = g_hash_table_lookup(blockCounts, block);
        g_hash_table_replace(blockCounts, block, GINT_TO_POINTER(GPOINTER_TO_INT(val) + 1));
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, blockCounts);
    
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        blockmeta *block = (blockmeta *)key;
        int ocorrencias_neste_ficheiro = GPOINTER_TO_INT(value);

        ticket_rwlock_write_lock(&block->lock);
        
        block->counter -= ocorrencias_neste_ficheiro;

        if (block->counter == 0) {
            g_hash_table_steal(ctx->blockIndex, block->id);
            
            pthread_mutex_lock(&ctx->freeList_lock);
            g_array_append_val(ctx->freeList, block->block_offset);
            pthread_mutex_unlock(&ctx->freeList_lock);
            
            ticket_rwlock_write_unlock(&block->lock);
            freeBlockMeta(block);
        } else {
            ticket_rwlock_write_unlock(&block->lock);
        }
    }

    g_hash_table_destroy(blockCounts);
    
    ticket_rwlock_write_unlock(&ctx->block_index_lock);

    ticket_rwlock_write_unlock(&file->lock);
    freeFilemeta(file);
    return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
	if (strcmp(from, "/backend/.metadata") == 0 || strcmp(from, "/backend/.sysdata") == 0 ||
	    strcmp(to, "/backend/.metadata") == 0 || strcmp(to, "/backend/.sysdata") == 0) {
	    return -EPERM;
	}
	int res;

	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size,
			struct fuse_file_info *fi)
{
	int res;

	if (fi != NULL)
		res = ftruncate(fi->fh, size);
	else
		res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	if (strcmp(path, "/backend/.metadata") == 0 || strcmp(path, "/backend/.sysdata") == 0) {
	    return -EACCES;
	}
	int res;

#ifdef DEBUG
	struct fuse_context *f_ctx = fuse_get_context();
	Context *p_ctx = f_ctx->private_data;
	p_ctx->open++;
	printf("[Thread %d] Create for path %s, userid %d, pid %d\n", gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

	res = open(path, fi->flags | O_DIRECT, mode);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode){
	int res;

#ifdef DEBUG
	struct fuse_context *f_ctx = fuse_get_context();
	printf("[Thread %d] Make dir for path %s, userid %d, pid %d\n", gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path){
	int res;

#ifdef DEBUG
	struct fuse_context *f_ctx = fuse_get_context();
	printf("[Thread %d] Remove dir for path %s, userid %d, pid %d\n", gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path, "/backend/.metadata") == 0 || strcmp(path, "/backend/.sysdata") == 0) {
	    return -EACCES;
	}
	int res;

#ifdef DEBUG
	struct fuse_context *f_ctx = fuse_get_context();
	Context *p_ctx = (Context *) f_ctx->private_data;
	p_ctx->open++;
	printf("[Thread %d] Open for path %s, userid %d, pid %d\n", gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

	res = open(path, fi->flags | O_DIRECT);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static gint compare_blocks(gconstpointer a, gconstpointer b) {
    blockmeta *block_a = (blockmeta *)a;
    blockmeta *block_b = (blockmeta *)b;
    return memcmp(block_a->id, block_b->id, 64);
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
            struct fuse_file_info *fi)
{
    if (size % 4096 != 0) return -EINVAL;
    struct fuse_context *f_ctx = fuse_get_context();
    Context *ctx = (Context *) f_ctx->private_data;

    ticket_rwlock_read_lock(&ctx->file_index_lock);
    filemeta *file = g_hash_table_lookup(ctx->fileIndex,path);
    if (file == NULL){
        ticket_rwlock_read_unlock(&ctx->file_index_lock);
        return -ENOENT;
    }
    ticket_rwlock_read_lock(&file->lock);
    // printf("[READ] path: %s | offset_pedido: %ld | size_pedido: %zu | logicalSize: %ld\n", 
            // path, offset, size, file->logicalSize);
    ticket_rwlock_read_unlock(&ctx->file_index_lock); 

    if (offset >= file->logicalSize) {
        ticket_rwlock_read_unlock(&file->lock);
        return 0;
    }
    if (offset + size > file->logicalSize) {
        size = file->logicalSize - offset;
    }

    int start_index = offset / 4096;
    int end_index = (offset + size - 1) / 4096;
    ticket_rwlock_read_lock(&ctx->block_index_lock);

    GHashTable *uniqueBlocks = g_hash_table_new(blockHashFunc, compareSHAHashes);
    for (int i = start_index; i <= end_index; i++) {
        blockmeta *block = (blockmeta *)g_ptr_array_index(file->blockList, i);
        g_hash_table_insert(uniqueBlocks, block->id, block);
    }

    GList *blocks_to_lock = g_hash_table_get_values(uniqueBlocks);
    blocks_to_lock = g_list_sort(blocks_to_lock, compare_blocks);
    for (GList *l = blocks_to_lock; l != NULL; l = l->next) {
        blockmeta *block = (blockmeta *)l->data;
        ticket_rwlock_read_lock(&block->lock);
    }
    g_list_free(blocks_to_lock);
    ticket_rwlock_read_unlock(&ctx->block_index_lock);

    size_t total_bytes_read = 0;
    char *buf_ptr = buf;
    int err = 0;

    for (int i = start_index; i <= end_index; i++) {
        blockmeta *block = (blockmeta *)g_ptr_array_index(file->blockList, i);
        
        off_t block_start_logical = (off_t)i * 4096;
        off_t read_start_in_block = (offset > block_start_logical) ? (offset - block_start_logical) : 0;
        size_t bytes_to_read = 4096 - read_start_in_block;
        
        if (total_bytes_read + bytes_to_read > size) {
            bytes_to_read = size - total_bytes_read;
        }

        if (ctx->backend_fd != -1) {
            void *aligned_buffer;
            if (posix_memalign(&aligned_buffer, 4096, 4096) != 0) {
                err = -ENOMEM;
                break;
            }

            ssize_t ret = pread(ctx->backend_fd, aligned_buffer, 4096, block->block_offset);
            
            if (ret == -1) { 
                err = -errno; 
                free(aligned_buffer);
                break; 
            }

            memcpy(buf_ptr, (char *)aligned_buffer + read_start_in_block, bytes_to_read);

            free(aligned_buffer);

        } else { 
            err = -EBADF; 
            break; 
        }

        buf_ptr += bytes_to_read;
        total_bytes_read += bytes_to_read;
    }      

    GHashTableIter b_iter;
    gpointer b_key, b_value;
    g_hash_table_iter_init(&b_iter, uniqueBlocks);
    while (g_hash_table_iter_next(&b_iter, &b_key, &b_value)) {
        blockmeta *block = (blockmeta *)b_value;
        ticket_rwlock_read_unlock(&block->lock);
    }
    g_hash_table_destroy(uniqueBlocks);
    ticket_rwlock_read_unlock(&file->lock);

    return err ? err : (int)total_bytes_read;
}

static int xmp_write(const char *path, const char *buf, size_t size,
             off_t offset, struct fuse_file_info *fi)
{
    struct fuse_context *f_ctx = fuse_get_context();
    Context *ctx = (Context *) f_ctx->private_data;
    if (size % 4096 != 0) return -EINVAL;

    int num_blocks = size / 4096;
    unsigned char (*blockHashes)[64] = g_malloc0(num_blocks * 64);
    for(int i = 0; i < num_blocks; i++) {
        SHA512((const unsigned char *)(buf + i * 4096), 4096, blockHashes[i]);
    }

    ticket_rwlock_read_lock(&ctx->file_index_lock);
    filemeta *file = g_hash_table_lookup(ctx->fileIndex, path);
    
    if (file == NULL) {
        ticket_rwlock_read_unlock(&ctx->file_index_lock);
        ticket_rwlock_write_lock(&ctx->file_index_lock);
        
        file = g_hash_table_lookup(ctx->fileIndex, path);
        if (file == NULL) {
            file = g_malloc0(sizeof(filemeta));
            file->id = g_strdup(path);
            file->blockList = g_ptr_array_new();
            ticket_rwlock_init(&file->lock);
            g_hash_table_insert(ctx->fileIndex, (gpointer)file->id, file);
        }
        ticket_rwlock_write_unlock(&ctx->file_index_lock);
        ticket_rwlock_read_lock(&ctx->file_index_lock); 
    }
    
    ticket_rwlock_write_lock(&file->lock);
    
    if (offset != file->logicalSize) {
        ticket_rwlock_write_unlock(&file->lock);
        ticket_rwlock_read_unlock(&ctx->file_index_lock);
        g_free(blockHashes);
        return -ENOTSUP;
    }

    ticket_rwlock_read_lock(&ctx->block_index_lock);

    GPtrArray *blockList = file->blockList;
    GHashTable *lockedBlocks = g_hash_table_new(blockHashFunc, compareSHAHashes);
    GPtrArray *newBlocksInfo = g_ptr_array_new();
    GHashTable *my_created_blocks = g_hash_table_new(g_direct_hash, g_direct_equal); 

    for(int i = 0; i < num_blocks; i++){
        blockmeta *block = g_hash_table_lookup(ctx->blockIndex, blockHashes[i]);
        
        if (!block) {
            ticket_rwlock_read_unlock(&ctx->block_index_lock);
            ticket_rwlock_write_lock(&ctx->block_index_lock);
            
            block = g_hash_table_lookup(ctx->blockIndex, blockHashes[i]);
            if (!block) {
                block = g_malloc0(sizeof(blockmeta));
                block->id = g_memdup2(blockHashes[i], 64);
                block->size = 4096;
                block->counter = 1; 
                ticket_rwlock_init(&block->lock);
                g_hash_table_insert(ctx->blockIndex, block->id, block);
                g_hash_table_insert(my_created_blocks, block, GINT_TO_POINTER(1));
            } else {
                __sync_fetch_and_add(&block->counter, 1);
            }
            ticket_rwlock_write_unlock(&ctx->block_index_lock);
            ticket_rwlock_read_lock(&ctx->block_index_lock);
        } else {
            __sync_fetch_and_add(&block->counter, 1);
        }
        
        if (!g_hash_table_lookup(lockedBlocks, block->id)) {
            g_hash_table_insert(lockedBlocks, block->id, block);
        }
        g_ptr_array_add(newBlocksInfo, block);
    }

    GList *blocks_to_lock = g_hash_table_get_values(lockedBlocks);
    blocks_to_lock = g_list_sort(blocks_to_lock, compare_blocks);
    for (GList *l = blocks_to_lock; l != NULL; l = l->next) {
        blockmeta *block = (blockmeta *)l->data;
        ticket_rwlock_write_lock(&block->lock);
    }

    pthread_mutex_lock(&ctx->freeList_lock);
    pthread_mutex_lock(&ctx->offset_lock);

    for (GList *l = blocks_to_lock; l != NULL; l = l->next) {
        blockmeta *block = (blockmeta *)l->data;
        if (g_hash_table_contains(my_created_blocks, block)) { 
            if (ctx->freeList->len > 0) {
				block->block_offset = g_array_index(ctx->freeList, off_t, ctx->freeList->len - 1);
                g_array_remove_index(ctx->freeList, ctx->freeList->len - 1);
            } else {
                block->block_offset = ctx->next_free_offset;
                ctx->next_free_offset += block->size;
            }
        }
    }

    pthread_mutex_unlock(&ctx->offset_lock);
    pthread_mutex_unlock(&ctx->freeList_lock);
    ticket_rwlock_read_unlock(&ctx->block_index_lock);
    ticket_rwlock_read_unlock(&ctx->file_index_lock);

    int err = 0;
    const char *buf_ptr = buf;

    void *aligned_block = NULL;
    if (posix_memalign(&aligned_block, 4096, 4096) != 0) {
        err = -ENOMEM;
        goto write_cleanup;
    }

    for (guint i = 0; i < newBlocksInfo->len; i++) {
        blockmeta *block = g_ptr_array_index(newBlocksInfo, i);
        if (g_hash_table_contains(my_created_blocks, block)) {
            if (ctx->backend_fd != -1) {
                memcpy(aligned_block, buf_ptr, block->size);
                if (pwrite(ctx->backend_fd, aligned_block, block->size, block->block_offset) == -1) err = -errno;
            }
            file->realSize += block->size; 
			g_hash_table_remove(my_created_blocks, block);
        }
        g_ptr_array_add(blockList, block); 
        buf_ptr += 4096;
        if (err) break;
    }

write_cleanup:
    if (!err && offset + size > file->logicalSize) file->logicalSize = offset + size;
	if (aligned_block) {
        free(aligned_block);
    }
    for (GList *l = blocks_to_lock; l != NULL; l = l->next) {
        blockmeta *block = (blockmeta *)l->data;
        ticket_rwlock_write_unlock(&block->lock);
    }
    ticket_rwlock_write_unlock(&file->lock);

    g_list_free(blocks_to_lock);
    g_hash_table_destroy(lockedBlocks);
    g_ptr_array_free(newBlocksInfo, TRUE);
    g_free(blockHashes);
	g_hash_table_destroy(my_created_blocks);

    return err ? err : (int)size;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;
	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;

	return close(fi->fh);
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{

	(void) path;

	//If the datasync parameter is non-zero, then only the user data should be flushed, not the meta data.
	if(isdatasync==0){
		return fsync(fi->fh);
	}
	
	return fdatasync(fi->fh);
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;

	if (mode)
		return -EOPNOTSUPP;

	if(fi == NULL)
		fd = open(path, O_WRONLY | O_DIRECT);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	if(fi == NULL)
		close(fd);
	return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_COPY_FILE_RANGE
static ssize_t xmp_copy_file_range(const char *path_in,
				   struct fuse_file_info *fi_in,
				   off_t offset_in, const char *path_out,
				   struct fuse_file_info *fi_out,
				   off_t offset_out, size_t len, int flags)
{
	int fd_in, fd_out;
	ssize_t res;

	if(fi_in == NULL)
		fd_in = open(path_in, O_RDONLY | O_DIRECT);
	else
		fd_in = fi_in->fh;

	if (fd_in == -1)
		return -errno;

	if(fi_out == NULL)
		fd_out = open(path_out, O_WRONLY | O_DIRECT);
	else
		fd_out = fi_out->fh;

	if (fd_out == -1) {
		close(fd_in);
		return -errno;
	}

	res = copy_file_range(fd_in, &offset_in, fd_out, &offset_out, len,
			      flags);
	if (res == -1)
		res = -errno;

	if (fi_out == NULL)
		close(fd_out);
	if (fi_in == NULL)
		close(fd_in);

	return res;
}
#endif

static off_t xmp_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi)
{
	int fd;
	off_t res;

	if (fi == NULL)
		fd = open(path, O_RDONLY | O_DIRECT);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = lseek(fd, off, whence);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}

static const struct fuse_operations xmp_oper = {
	.init       = xmp_init,
	.destroy 	= xmp_destroy,
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.mknod		= xmp_mknod,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.mkdir		= xmp_mkdir,
	.rmdir		= xmp_rmdir,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.open		= xmp_open,
	.create 	= xmp_create,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
#ifdef HAVE_COPY_FILE_RANGE
	.copy_file_range = xmp_copy_file_range,
#endif
	.lseek		= xmp_lseek,
};


int main(int argc, char *argv[])
{
	enum { MAX_ARGS = 10 };
	int i,new_argc;
	char *new_argv[MAX_ARGS];


	umask(0);
			/* Process the "--plus" option apart */
	for (i=0, new_argc=0; (i<argc) && (new_argc<MAX_ARGS); i++) {
		if (!strcmp(argv[i], "--plus")) {
			fill_dir_plus = FUSE_FILL_DIR_PLUS;
		} else {
			new_argv[new_argc++] = argv[i];
		}
	}

	return fuse_main(new_argc, new_argv, &xmp_oper, NULL);
}
