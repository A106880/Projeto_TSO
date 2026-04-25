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

// #define DEBUG "/tmp/debug.log"

typedef struct context {
	GHashTable *fileIndex;
	GHashTable *blockIndex;
    pthread_mutex_t index_lock;
	GQueue *freeList;
    pthread_mutex_t freeList_lock;
    
    int backend_fd;
    off_t next_free_offset;
    pthread_mutex_t offset_lock;

    uint64_t open; 
} Context;

off_t get_new_offset(Context *ctx, size_t size) {
    off_t offset;
    pthread_mutex_lock(&ctx->freeList_lock);
	pthread_mutex_lock(&ctx->offset_lock);
    if (!g_queue_is_empty(ctx->freeList)) {
        off_t *poff = g_queue_pop_head(ctx->freeList);
        offset = *poff;
        g_free(poff);
        pthread_mutex_unlock(&ctx->freeList_lock);
        pthread_mutex_unlock(&ctx->offset_lock);
        return offset;
    }

    offset = ctx->next_free_offset;
    pthread_mutex_unlock(&ctx->offset_lock);
	pthread_mutex_unlock(&ctx->freeList_lock);
    return offset;
}

static int fill_dir_plus = 0;

char *blocksPath = "/backend/blockspath";

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
	ctx->blockIndex = g_hash_table_new_full(g_str_hash,g_str_equal,g_free,freeBlockMeta);
	ctx->fileIndex = g_hash_table_new_full(g_str_hash,g_str_equal,g_free,freeFilemeta);
	ctx->freeList = g_queue_new();

	pthread_mutex_init(&ctx->index_lock, NULL);
	pthread_mutex_init(&ctx->freeList_lock, NULL);
	pthread_mutex_init(&ctx->offset_lock, NULL);

    ctx->backend_fd = open("/backend/data.bin", O_RDWR | O_CREAT, 0644);
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
	
	// load_metadata(ctx->fileIndex, ctx->blockIndex, ctx->freeList);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ctx->fileIndex);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        filemeta *fm = (filemeta *)value;
        pthread_mutex_init(&fm->lock, NULL);
    }

	return ctx;

	
}

static void xmp_destroy(void* private_data){


	struct fuse_context *f_ctx = fuse_get_context();
	Context* p_ctx = (Context*) private_data;

	// save_metadata(p_ctx->fileIndex, p_ctx->blockIndex, p_ctx->freeList);
	pthread_mutex_destroy(&p_ctx->index_lock);
	pthread_mutex_destroy(&p_ctx->freeList_lock);
	pthread_mutex_destroy(&p_ctx->offset_lock);

    if (p_ctx->backend_fd != -1) close(p_ctx->backend_fd);

	if (p_ctx->freeList) {
        g_queue_free_full(p_ctx->freeList, g_free); 
    }

	printf("[Thread %d] Destroy called, userid %d, pid %d\n", gettid(), f_ctx->uid, f_ctx->pid);
	printf("[Thread %d] Open() - %lu\n", gettid(), p_ctx->open);

	free(private_data);

}

static int xmp_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	struct fuse_context *f_ctx = fuse_get_context();
	Context *ctx = (Context *) f_ctx->private_data;

	pthread_mutex_lock(&ctx->index_lock);
	filemeta *file = NULL;
	if (ctx && ctx->fileIndex) {
		file = g_hash_table_lookup(ctx->fileIndex, path);
		if (file != NULL) {
            pthread_mutex_lock(&file->lock);
			stbuf->st_size = file->logicalSize;
			stbuf->st_blocks = (file->logicalSize + 511) / 512;
            pthread_mutex_unlock(&file->lock);
		}
	}
    
    printf("[GETATTR] path: %s | tamanho_fisico: %ld | tamanho_logico: %ld\n", 
           path, stbuf->st_size, file ? file->logicalSize : -1);
	pthread_mutex_unlock(&ctx->index_lock);

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
	struct fuse_context *f_ctx = fuse_get_context();
    Context *ctx = (Context *) f_ctx->private_data;
    int res;

    res = unlink(path);
    if (res == -1)
        return -errno;

    pthread_mutex_lock(&ctx->index_lock);
    filemeta *file = g_hash_table_lookup(ctx->fileIndex, path);
    if (file != NULL) {
        pthread_mutex_lock(&file->lock);
        g_hash_table_steal(ctx->fileIndex, path);

        GQueue *blockList = file->blockList;
        for (int i = 0; i < g_queue_get_length(blockList); i++) {
            blockmeta *block = g_queue_peek_nth(blockList, i);
            block->counter--;
            if (block->counter == 0) {
                pthread_mutex_lock(&ctx->freeList_lock);
                off_t *freed_offset = g_malloc(sizeof(off_t));
                *freed_offset = block->block_offset;
                g_queue_push_tail(ctx->freeList, freed_offset);
                pthread_mutex_unlock(&ctx->freeList_lock);

                g_hash_table_remove(ctx->blockIndex, block->id);
            }
        }
        pthread_mutex_unlock(&file->lock);
        pthread_mutex_destroy(&file->lock);
        freeFilemeta(file);
    }
    pthread_mutex_unlock(&ctx->index_lock);

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
	int res;

	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	struct fuse_context *f_ctx = fuse_get_context();
	Context *ctx = (Context *) f_ctx->private_data;

	pthread_mutex_lock(&ctx->index_lock);
	filemeta *file = g_hash_table_lookup(ctx->fileIndex, from);
	if (file != NULL) {
        pthread_mutex_lock(&file->lock);
		g_hash_table_steal(ctx->fileIndex, from); // remove without freeing
		g_free(file->id);
		file->id = g_strdup(to);
		g_hash_table_insert(ctx->fileIndex, g_strdup(to), file);
        pthread_mutex_unlock(&file->lock);
	}
	pthread_mutex_unlock(&ctx->index_lock);

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
	int res;

#ifdef DEBUG
	struct fuse_context *f_ctx = fuse_get_context();
	Context *p_ctx = f_ctx->private_data;
	p_ctx->open++;
	printf("[Thread %d] Create for path %s, userid %d, pid %d\n", gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

	res = open(path, fi->flags, mode);
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
	int res;

#ifdef DEBUG
	struct fuse_context *f_ctx = fuse_get_context();
	Context *p_ctx = (Context *) f_ctx->private_data;
	p_ctx->open++;
	printf("[Thread %d] Open for path %s, userid %d, pid %d\n", gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

	res = open(path, fi->flags);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	struct fuse_context *f_ctx = fuse_get_context();
	Context *ctx = (Context *) f_ctx->private_data;

	pthread_mutex_lock(&ctx->index_lock);
	filemeta *file = g_hash_table_lookup(ctx->fileIndex,path);
	if (file == NULL){
		pthread_mutex_unlock(&ctx->index_lock);
		return -ENOENT;
	}
    pthread_mutex_lock(&file->lock);
    pthread_mutex_unlock(&ctx->index_lock);

	printf("[READ] path: %s | offset_pedido: %ld | size_pedido: %zu | logicalSize: %ld\n", 
           path, offset, size, file->logicalSize);

	GQueue *blockList = file->blockList;
    off_t current_logical_offset = 0;
    size_t total_bytes_read = 0;
    char *buf_ptr = buf;

	for (int i = 0; i < g_queue_get_length(blockList); i++){
		blockmeta *block = g_queue_peek_nth(blockList, i);

		off_t block_start = current_logical_offset;
        off_t block_end = current_logical_offset + block->size;

		if (offset < block_end && (offset + size) > block_start){
            off_t read_start_in_block = 0;
            if (offset > block_start) {
                read_start_in_block = offset - block_start;
            }

            size_t bytes_to_read_from_block = block->size - read_start_in_block;
            
            if (total_bytes_read + bytes_to_read_from_block > size) {
                bytes_to_read_from_block = size - total_bytes_read;
            }

            if (ctx->backend_fd != -1) {
                pread(ctx->backend_fd, buf_ptr, bytes_to_read_from_block, block->block_offset + read_start_in_block);
            } else {
                pthread_mutex_unlock(&file->lock);
                return -errno; 
            }

            buf_ptr += bytes_to_read_from_block;
            total_bytes_read += bytes_to_read_from_block;

            if (total_bytes_read == size) {
                break;
            }
		}
		current_logical_offset += block->size;
	}	

	pthread_mutex_unlock(&file->lock);

	return total_bytes_read;

}

static char *hash_to_hex(const unsigned char *hash) {
    char *hex_str = g_malloc0(129); 
    
    for (int i = 0; i < 64; i++) {
        sprintf(hex_str + (i * 2), "%02x", hash[i]);
    }
    
    return hex_str;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	struct fuse_context *f_ctx = fuse_get_context();
	Context *ctx = (Context *) f_ctx->private_data;
	
	pthread_mutex_lock(&ctx->index_lock);
	filemeta *file = g_hash_table_lookup(ctx->fileIndex,path);
	if (file == NULL){
		file = g_malloc0(sizeof(filemeta));
		file->id = g_strdup(path);
		file->blockList = g_queue_new();
        pthread_mutex_init(&file->lock, NULL);
		g_hash_table_insert(ctx->fileIndex,(gpointer)file->id,file);
	}
    pthread_mutex_lock(&file->lock);
    pthread_mutex_unlock(&ctx->index_lock);

	GQueue *blockList = file->blockList;

	for(int i = 0; i<size;i+=4096){
		unsigned char blockHash[64];

		size_t bytes;
		if (size - i < 4096){
			bytes = (size - i);
		}else{
			bytes = 4096;
		}
		
		SHA512((const unsigned char *)(buf+i), bytes, blockHash);
		char *hexHash = hash_to_hex(blockHash);

        pthread_mutex_lock(&ctx->index_lock);
		blockmeta *block = g_hash_table_lookup(ctx->blockIndex, hexHash);
		if (!block) {
            pthread_mutex_unlock(&ctx->index_lock);
            
            off_t offset = get_new_offset(ctx, bytes);
            if (ctx->backend_fd != -1) {
                pwrite(ctx->backend_fd, buf+i, bytes, offset);
            }
			ctx->next_free_offset += size; //FIXME

            pthread_mutex_lock(&ctx->index_lock);
            blockmeta *block2 = g_hash_table_lookup(ctx->blockIndex, hexHash);
            if (block2) {
                block2->counter++;
                block = block2;
                g_free(hexHash);
                
                pthread_mutex_lock(&ctx->freeList_lock);
                off_t *waste_off = g_malloc(sizeof(off_t));
                *waste_off = offset;
                g_queue_push_tail(ctx->freeList, waste_off);
                pthread_mutex_unlock(&ctx->freeList_lock);
            } else {
                block = g_malloc0(sizeof(blockmeta));
                block->id = hexHash;
                block->size = bytes;
                block->counter = 1;
                block->block_offset = offset;
                g_hash_table_insert(ctx->blockIndex, block->id, block);
                file->realSize += bytes;
            }
            pthread_mutex_unlock(&ctx->index_lock);
		} else {
			block->counter++;
            pthread_mutex_unlock(&ctx->index_lock);
			g_free(hexHash);
		}
			
		g_queue_push_tail(blockList, block);
	}
	
	if (offset + size > file->logicalSize) {
		file->logicalSize = offset + size;
	}

	printf("[WRITE] path: %s | offset: %ld | size_pedido: %zu | logicalSize_final: %ld\n", 
           path, offset, size, file->logicalSize);

	pthread_mutex_unlock(&file->lock);

	return size;
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
		fd = open(path, O_WRONLY);
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
		fd_in = open(path_in, O_RDONLY);
	else
		fd_in = fi_in->fh;

	if (fd_in == -1)
		return -errno;

	if(fi_out == NULL)
		fd_out = open(path_out, O_WRONLY);
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
		fd = open(path, O_RDONLY);
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
	.init           = xmp_init,
	.destroy = xmp_destroy,
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
