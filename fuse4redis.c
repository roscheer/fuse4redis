/*
  Exposes entries on a redis database as files in a FUSE file system.
  Copyright (C) 2016 Roque Luis Scheer <roqscheer@gmail.com>

  This program can be distributed under the terms of the GNU GPLv3.
  See the file COPYING.
   
  Derived  by modifying source code from the Big Brother File System
  Copyright (C) 2012 Joseph J. Pfeiffer, Jr., Ph.D. <pfeiffer@cs.nmsu.edu>

  Which in turn is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.
  A copy of that code is included in the file fuse.h
  
*/

#include "params.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <hiredis.h>
#include <sys/types.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"

// Redis connection
redisContext *redisCtx;

// Removes path from file name
// TODO: right now this simple macro suffices since we do not support subfolders 
//       in fuse4redis. Needs to be revisited if folder support is added
#define FILE_NAME(path) (path[0] == '/' ? path + 1 : path)


// Ancilary functions to abstract database (KVS) details

// Connects to redis. Simply aborts if fail.
void redis_init( const char *hostname, int port)
{
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    
    redisCtx = redisConnectWithTimeout(hostname, port, timeout);
    if (redisCtx == NULL || redisCtx->err) {
        if (redisCtx) {
            printf("Connection error: %s\n", redisCtx->errstr);
            redisFree(redisCtx);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }
}


// Creates an empty redis key to represent an empty file
int redis_CreateEmptyKey( const char *path)
{
    redisReply *reply;
    
    reply = redisCommand(redisCtx,"SET %s %s", FILE_NAME(path), "");
    if (reply->type == REDIS_REPLY_ERROR) {
        log_msg( "Unexpected result from redis %d\n", reply->type);
        freeReplyObject(reply);
        return -EIO;
    }

    freeReplyObject(reply);
    return 0;
}


// Checks if a key (representing a file) already exists
int redis_KeyExists( const char *path)
{
    redisReply *reply;

    reply = redisCommand(redisCtx,"EXISTS %s", FILE_NAME(path) );
    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "Unexpected result from redis %d\n", reply->type);
        freeReplyObject(reply);
        return -EIO;
    }
    
    return (int) reply->integer;  // 1 if exists, 0 otherwise
}

// Get length of a key (known to exist, if not redis returns len=0)
size_t redis_GetKeyLength( const char *path)
{
    redisReply *reply;
    size_t ksize;
    
    reply = redisCommand(redisCtx,"STRLEN %s", FILE_NAME(path) );
    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "Unexpected result from redis %d\n", reply->type);
        freeReplyObject(reply);
        return -EIO;
    }
    ksize = (size_t)reply->integer;
    freeReplyObject(reply);
    return ksize;
}

// Extends the value of an existing key (must exist) using null characters.
int redis_AppendZeroedBytes( const char *path, size_t newsize)
{
    redisReply *reply;
    char zbuffer[512] = {0};
    size_t chunksize;
    
    // Extending a key' value is really a corner case, so no concerns about performance
    while ( newsize > 0) {
        chunksize = newsize > 512 ? 512 : newsize;
        reply = redisCommand(redisCtx,"APPEND %s %b", FILE_NAME(path), zbuffer, 
                             chunksize);
        if (reply->type != REDIS_REPLY_INTEGER) {
            log_msg( "Unexpected result from redis %d\n", reply->type);
            freeReplyObject(reply);
            return -EIO;
        }
        freeReplyObject(reply);
        newsize -= chunksize;
    }
    return 0;
}

// Truncates the value of an existing key discarding the trailing content
int redis_TruncateKey( const char *path, size_t newsize)
{
    redisReply *reply1, *reply2;
    int result = 0;

    reply1 = redisCommand(redisCtx,"GET %s", FILE_NAME(path));
    if (reply1->type != REDIS_REPLY_STRING) {
        log_msg( "Unexpected result from redis %d\n", reply1->type);
        freeReplyObject(reply1);
        return -EIO;
    }
    reply2 = redisCommand( redisCtx, "SET %s %b", FILE_NAME(path), reply1->str, newsize);
    if ( reply2->type == REDIS_REPLY_ERROR) {
        log_msg( "Error result from redis %d\n", reply2->type);
        result = -EIO;
    }
    freeReplyObject(reply1);
    freeReplyObject(reply2);
    return result;
}

    int retstat = 0;
    redisReply *reply;

    
// This will copy the root directory file list into the FUSE buffer using the FUSE 
// 'filler' function.
//
// TODO: Passing the FUSE filler function to this KVS abstraction layer decouples  
//       the FUSE code from redis, but not vice versa. Ideally this abstraction layer 
//       would simply return a list off strings (entries), and the calling code would 
//       transfer them to fuse. However this incurs a penalty allocating space for a 
//       variable size list and deallocating it soon after. The implementation below
//       represents an acceptable compromise, given the purpose of this program.
//
// TODO: Function is not ready template is not ready to support subfolders eventually.
//
int redis_ReadDirectory( void *buf, fuse_fill_dir_t filler)
{
    redisReply *reply;
      
    reply = redisCommand(redisCtx,"KEYS *");
    if (reply->type == REDIS_REPLY_ERROR) {
        log_msg( "Failed to communicate with redis\n");
        return -EIO;
    }
        
   // The loop below exits when either all keys were copied, or filler()
   // returns something non-zero.  The first case just means I've
   // read the whole 'redis' directory; the second means the buffer is full.
    if (reply->type == REDIS_REPLY_ARRAY) {
        for (int j = 0; j < reply->elements; j++) {
            log_msg("calling filler with name %s\n", reply->element[j]->str);
            if (filler(buf, reply->element[j]->str, NULL, 0) != 0) {
	        log_msg("    ERROR bb_readdir filler:  buffer full");
                freeReplyObject(reply);
	        return -ENOMEM;
	    }
	}
    }
    freeReplyObject(reply);
    return 0;
}


//  All the paths I see are relative to the root of the mounted
//  filesystem.  In order to get to the underlying filesystem, I need to
//  have the mountpoint.  I'll save it away early on in main(), and then
//  whenever I need a path for something I'll call this to construct
//  it.
static void bb_fullpath(char fpath[PATH_MAX], const char *path)
{
    strcpy(fpath, BB_DATA->rootdir);
    strncat(fpath, path, PATH_MAX); // ridiculously long paths will
				    // break here

    log_msg("    bb_fullpath:  rootdir = \"%s\", path = \"%s\", fpath = \"%s\"\n",
	    BB_DATA->rootdir, path, fpath);
}

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come from /usr/include/fuse.h
//
/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int bb_getattr(const char *path, struct stat *statbuf)
{
    size_t fsize = 0;
    
    log_msg( "Called getattr for path=%s\n", path);
    
    statbuf->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    if (strcmp(path, "/") == 0) {   // Atributes for the FS' root dir
        statbuf->st_mode = statbuf->st_mode | S_IFDIR;
        statbuf->st_size = 0;
    } else {
        // First check if file/key exists, because STRLEN simply returns 0 if it doesn't
        int exists = redis_KeyExists( path);
        
        if (exists < 0 )
            return -EIO;

        if ( ! exists)   // Key does not exist
            return -ENOENT;

        statbuf->st_mode = statbuf->st_mode | S_IFREG;
        fsize = redis_GetKeyLength( path);
        if ( fsize < 0 )
            return -EIO;
        log_msg( "File size is %ld\n", fsize);
    }
    statbuf->st_size = fsize;
    statbuf->st_uid = getuid();
    statbuf->st_gid = getgid();
    statbuf->st_blksize = 512;  // Arbitrarily chosen! Not relevant for redis.
    statbuf->st_blocks = fsize > 0 ? (fsize / 512) + 1 : 0;
    return 0;
}

/** Read the target of a symbolic link
 *
 * The buffer should be filled with a null terminated string.  The
 * buffer size argument includes the space for the terminating
 * null character.  If the linkname is too long to fit in the
 * buffer, it should be truncated.  The return value should be 0
 * for success.
 */
// Note the system readlink() will truncate and lose the terminating
// null.  So, the size passed to to the system readlink() must be one
// less than the size passed to bb_readlink()
// bb_readlink() code by Bernardo F Costa (thanks!)
int bb_readlink(const char *path, char *link, size_t size)
{
    int retstat;
    char fpath[PATH_MAX];
    
    log_msg("bb_readlink(path=\"%s\", link=\"%s\", size=%d)\n",
	  path, link, size);
    bb_fullpath(fpath, path);

    retstat = log_syscall("fpath", readlink(fpath, link, size - 1), 0);
    if (retstat >= 0) {
	link[retstat] = '\0';
	retstat = 0;
    }
    
    return retstat;
}

/** Create a file node
 *
 * There is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
int bb_mknod(const char *path, mode_t mode, dev_t dev)
{
    
    log_msg( "Called mknod for path=%s\n", path);
    
    if ( ! S_ISREG(mode)) // fuse4redis only support regular file creation
        return -EINVAL;
        
    // With O_EXCL, file/key cannot already exist 
    if (mode & O_EXCL) {
        int exists = redis_KeyExists(path);
        
        if (exists < 0)
            return -EIO;

        if ( exists)
            return -EEXIST;
    }
    
    // Create an empty redis key to represent an empty file
    if (redis_CreateEmptyKey(path) < 0)
        return -EIO;
 
    return 0;
}

/** Create a directory */
int bb_mkdir(const char *path, mode_t mode)
{
    log_msg( "Called mkdir for path=%s\n", path);
    return -ENOSYS;
}

/** Remove a file */
int bb_unlink(const char *path)
{
    char fpath[PATH_MAX];
    
    // redis DEL key
    
    log_msg( "Called unlink for path=%s\n", path);

    bb_fullpath(fpath, path);

    return log_syscall("unlink", unlink(fpath), 0);
}

/** Remove a directory */
int bb_rmdir(const char *path)
{
    log_msg( "Called rmdir for path=%s\n", path);

    return -ENOSYS;
}


/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
int bb_symlink(const char *path, const char *link)
{
    log_msg( "Called symlink for path=%s\n", path);
    return -ENOSYS;
}

/** Rename a file */
// both path and newpath are fs-relative
int bb_rename(const char *path, const char *newpath)
{
    char fpath[PATH_MAX];
    char fnewpath[PATH_MAX];
    
    // redis RENAME key newkey
    // EXISTS key
    
    log_msg( "Called rename for path=%s newpath=%s\n", path, newpath);
    
    bb_fullpath(fpath, path);
    bb_fullpath(fnewpath, newpath);

    return log_syscall("rename", rename(fpath, fnewpath), 0);
}

/** Create a hard link to a file */
int bb_link(const char *path, const char *newpath)
{
    log_msg( "Called link for path=%s\n", path);
    return -ENOSYS;
}

/** Change the permission bits of a file */
int bb_chmod(const char *path, mode_t mode)
{
    log_msg( "Called chmod for path=%s\n", path);
    return -ENOSYS;
}

/** Change the owner and group of a file */
int bb_chown(const char *path, uid_t uid, gid_t gid)
{
    log_msg( "Called chown for path=%s\n", path);
    return -ENOSYS;
}

/** Change the size of a file */
int bb_truncate(const char *path, off_t newsize)
{
    int exists, result;
    size_t ksize;
    log_msg( "Called truncate for path=%s\n", path);
    
    exists = redis_KeyExists( path);
    if ( exists < 0)
        return -EIO;
    if ( ! exists)
        return -ENOENT;
        
    ksize = redis_GetKeyLength( path);
    if ( ksize < 0)
        return -EIO;
        
    if ( ksize == newsize)
        return 0;       // Nothing to be done
    
    if ( newsize > ksize)
        result = redis_AppendZeroedBytes( path, newsize - ksize);
    else
        result = redis_TruncateKey( path, newsize);
     
    return result;
}

/** Change the access and/or modification times of a file */
int bb_utime(const char *path, struct utimbuf *ubuf)
{
    log_msg( "Called utime for path=%s\n", path);
    return -ENOSYS;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int bb_open(const char *path, struct fuse_file_info *fi)
{
    int exists;
    
    log_msg( "Called open for path=%s\n", path);
    
    if (strcmp(path, "/") == 0) {   // Trying to open the FS' root dir
        return -EISDIR;
    }
    
    exists = redis_KeyExists(path);
    if ( exists < 0)
        return -EIO;

    if ( ! exists) {
        if( fi->flags & O_CREAT) {
            // Create an empty redis key to represent an empty file
            if (redis_CreateEmptyKey(path) < 0)
                return -EIO;
        } else        
            return -ENOENT;
    }

    return 0;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
int bb_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    redisReply *reply;
    
    log_msg( "Called read for path=%s\n", path);
    
    if (strcmp(path, "/") == 0) {   // Trying to read the FS' root dir
        return -EISDIR;
    }
    
    reply = redisCommand(redisCtx,"GETRANGE %s %ld %ld", FILE_NAME(path),
                         offset, offset + size - 1);  // Assuming size will never be 0
    if (reply->type != REDIS_REPLY_STRING) {
        log_msg( "Unexpected result from redis %d\n", reply->type);
        freeReplyObject(reply);
        return -EIO;
    }
    
    memcpy( buf, reply->str, reply->len);
    
    return reply->len;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int bb_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
    log_msg( "Called writefor path=%s\n", path);
    
    return log_syscall("pwrite", pwrite(fi->fh, buf, size, offset), 0);
}

/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 * Replaced 'struct statfs' parameter with 'struct statvfs' in
 * version 2.5
 */
int bb_statfs(const char *path, struct statvfs *statv)
{
    log_msg( "Called statfs for path=%s\n", path);
    return -ENOSYS;
}

/** Possibly flush cached data
 *
 * BIG NOTE: This is not equivalent to fsync().  It's not a
 * request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor.  So if a
 * filesystem wants to return write errors in close() and the file
 * has cached dirty data, this is a good place to write back data
 * and return any errors.  Since many applications ignore close()
 * errors this is not always useful.
 *
 * NOTE: The flush() method may be called more than once for each
 * open().  This happens if more than one file descriptor refers
 * to an opened file due to dup(), dup2() or fork() calls.  It is
 * not possible to determine if a flush is final, so each flush
 * should be treated equally.  Multiple write-flush sequences are
 * relatively rare, so this shouldn't be a problem.
 *
 * Filesystems shouldn't assume that flush will always be called
 * after some writes, or that if will be called at all.
 *
 * Changed in version 2.2
 */
int bb_flush(const char *path, struct fuse_file_info *fi)
{
    log_msg( "Called flushfor path=%s\n", path);
    return 0;   // No op. Nothing to flush to redis.
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int bb_release(const char *path, struct fuse_file_info *fi)
{
    log_msg( "Called release for path=%s\n", path);

    // We do not keep any file related state and do not use handles. Nothing to do here!
    return 0;
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 *
 * Changed in version 2.2
 */
int bb_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    log_msg( "Called fsync for path=%s\n", path);
    return 0;
}

#ifdef HAVE_SYS_XATTR_H
/** Set extended attributes */
int bb_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    log_msg( "Called setxattr for path=%s\n", path);
    return -ENOSYS;
}

/** Get extended attributes */
int bb_getxattr(const char *path, const char *name, char *value, size_t size)
{
    log_msg( "Called getxattr for path=%s\n", path);
    return -ENOSYS;
}

/** List extended attributes */
int bb_listxattr(const char *path, char *list, size_t size)
{
    log_msg( "Called listxattr for path=%s\n", path);
    return -ENOSYS;
}

/** Remove extended attributes */
int bb_removexattr(const char *path, const char *name)
{
    log_msg( "Called removexattr for path=%s\n", path);
    return -ENOSYS;
}
#endif

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int bb_opendir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    
    log_msg( "Called opendir for path=%s\n", path);
    
    fi->fh = 0;  // No handles used 
    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */

int bb_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
    log_msg( "Called readdir for path=%s\n", path);
    
    if (strcmp(path, "/") != 0)   // Only the FS' root dir is currently allowed
        return -ENOTDIR;
    
    return redis_ReadDirectory( buf, filler);
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int bb_releasedir(const char *path, struct fuse_file_info *fi)
{
    log_msg( "Called releasedir for path=%s\n", path);
    
    // We do not keep any directory related state and do not use handles. Nothing to do here!
    return 0;
}

/** Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data
 *
 * Introduced in version 2.3
 */
// when exactly is this called?  when a user calls fsync and it
// happens to be a directory? ??? >>> I need to implement this...
int bb_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
    log_msg( "Called fsyncdir for path=%s\n", path);
    return 0;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *bb_init(struct fuse_conn_info *conn)
{
    log_msg( "Called init\n");
    
    return BB_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void bb_destroy(void *userdata)
{
    log_msg( "Called destroy (a.k.a. cleanup)\n");
}

/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 *
 * Introduced in version 2.5
 */
int bb_access(const char *path, int mask)
{
    log_msg( "Called access for path=%s mask=%d\n", path, mask);
    return 0;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
// Not implemented.  I had a version that used creat() to create and
// open the file, which it turned out opened the file write-only.

/**
 * Change the size of an open file
 *
 * This method is called instead of the truncate() method if the
 * truncation was invoked from an ftruncate() system call.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the truncate() method will be
 * called instead.
 *
 * Introduced in version 2.5
 */
int bb_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
    log_msg( "Called ftruncate for path=%s\n", path);
    return -ENOSYS;
}

/**
 * Get attributes from an open file
 *
 * This method is called instead of the getattr() method if the
 * file information is available.
 *
 * Currently this is only called after the create() method if that
 * is implemented (see above).  Later it may be called for
 * invocations of fstat() too.
 *
 * Introduced in version 2.5
 */
int bb_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
    log_msg( "Called fgetattr for path=%s\n", path);

    // Since we do not keep file handles, we simply delegate to bb_getattr()
    return bb_getattr(path, statbuf);
}

struct fuse_operations bb_oper = {
  .getattr = bb_getattr,
  .readlink = bb_readlink,
  // no .getdir -- that's deprecated
  .getdir = NULL,
  .mknod = bb_mknod,
  .mkdir = bb_mkdir,
  .unlink = bb_unlink,
  .rmdir = bb_rmdir,
  .symlink = bb_symlink,
  .rename = bb_rename,
  .link = bb_link,
  .chmod = bb_chmod,
  .chown = bb_chown,
  .truncate = bb_truncate,
  .utime = bb_utime,
  .open = bb_open,
  .read = bb_read,
  .write = bb_write,
  /** Just a placeholder, don't set */ // huh???
  .statfs = bb_statfs,
  .flush = bb_flush,
  .release = bb_release,
  .fsync = bb_fsync,
  
#ifdef HAVE_SYS_XATTR_H
  .setxattr = bb_setxattr,
  .getxattr = bb_getxattr,
  .listxattr = bb_listxattr,
  .removexattr = bb_removexattr,
#endif
  
  .opendir = bb_opendir,
  .readdir = bb_readdir,
  .releasedir = bb_releasedir,
  .fsyncdir = bb_fsyncdir,
  .init = bb_init,
  .destroy = bb_destroy,
  .access = bb_access,
  .ftruncate = bb_ftruncate,
  .fgetattr = bb_fgetattr
};

void bb_usage()
{
    fprintf(stderr, "usage:  bbfs [FUSE and mount options] rootDir mountPoint\n");
    abort();
}


int main(int argc, char *argv[])
{
    const char *hostname = "127.0.0.1";
    int port = 6379;
    int fuse_stat;
    struct bb_state *bb_data;


    // See which version of fuse we're running
    fprintf(stderr, "Fuse library version %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
    
    // Perform some sanity checking on the command line:  make sure
    // there are enough arguments, and that mount folder does not
    // start with a hyphen (this will break if you actually have a
    // mountpoint whose name starts with a hyphen, but so will a 
    // other programs too)
    if ((argc < 2) || (argv[argc-1][0] == '-'))
	bb_usage();
	
    bb_data = malloc(sizeof(struct bb_state));
    if (bb_data == NULL) {
        perror("main calloc");
        abort();
    }
    bb_data->logfile = log_open();


// TODO: implement option to connect to a remote redis host
    redis_init(hostname, port);
    
    // turn over control to fuse
    
    fprintf(stderr, "Calling fuse_main()\n");

    fuse_stat = fuse_main(argc, argv, &bb_oper, bb_data);
    
    log_msg( "Returned from fuse_main() status=%d\n", fuse_stat);
    
    redisFree(redisCtx);
    return fuse_stat;
}
