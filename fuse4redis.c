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
#include <stdarg.h>
#include <hiredis.h>
#include <sys/types.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"

// Redis connection
redisContext *redisCtx;

// Strips path from file name
// TODO: right now this simple macro suffices since we do not support subfolders 
//       in fuse4redis. Needs to be revisited if folder support is added
#define FILE_NAME(path) (path[0] == '/' ? path + 1 : path)


//////////////////////////////////////////////////////////////////////
//
// Ancilary functions to abstract database (KVS) details
//
// TODO: Move these functions to a separate source code file

const char *hostname = "127.0.0.1";
const int port = 6379;


// Initial connection to redis upon startup. Simply aborts if it fails.
//
void kvs_init( const char *hostname, int port)
{
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    
    redisCtx = redisConnectWithTimeout(hostname, port, timeout);
    if (redisCtx == NULL ) {
        printf("Connection error: can't allocate redis context\n");
        exit(-1);
    }

    if ( redisCtx->err) {
        printf("Connection error: %s\n", redisCtx->errstr);
        redisFree(redisCtx);
        exit(-2);
    }
}

// Reconnects to redis. Simply aborts if fail. Logs errors in logfile.
// NOTE: while we could choose not to abort if we cannot reconnect, we would flood 
// the logs with error messages as the FS continues to be used, so we chose to retry 
// only once and require that the system admin (or the user) restart fuse4redis in 
// case redis was out for longer than a short while. 
//
void kvs_Reconnect( const char *hostname, int port)
{
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    
    redisCtx = redisConnectWithTimeout(hostname, port, timeout);
    if (redisCtx == NULL ) {
        log_msg("kvs_Reconnect: Connection error: can't allocate redis context\n");
        exit(-3);
    }

    if ( redisCtx->err != 0) {
        log_msg( "kvs_Reconnect: Connection error #%d: %s\n", redisCtx->err, redisCtx->errstr);
        redisFree(redisCtx);
        exit(-4);
    }
}


// Connection to redis may be lost and a reconnection may fix it.
// Instead of calling redisCommand throughout the code and handling this 
// on every call, we will wrap redisCommand and provide better error handling
// in one single place, maintaining the rest of the code cleanner.
// Guarantees that resultReply in non-NULL upon successfull return.

int kvs_RedisCommand( redisReply **resultReply, const char *cmd, ...)
{
    va_list valist;
    redisReply *kvsReply;
    int tries = 0;

    while ( tries < 2) {     // This loop retries once if redis connection error
        va_start(valist, cmd);    // Initialize valist each time used
        kvsReply = redisvCommand(redisCtx, cmd, valist);
        va_end(valist);     // Clean valist

        if (kvsReply == NULL) {
            log_msg( "Error when invoking redis: #%d: %s\n", redisCtx->err, 
                     redisCtx->errstr);
            log_msg( "Attempting to reconnect once!\n");
            redisFree(redisCtx);
            kvs_Reconnect( hostname, port);   // One retry, as kvs_Reconnect() exits if error
            tries ++;
        } else
            break;
    }
    
    if (kvsReply == NULL) {
        log_msg("kvs_RedisCommand: ERROR - returned error after successful reconnection\n");
        log_msg("kvs_RedisCommand: redis error #%d: %s\n", redisCtx->err, redisCtx->errstr);
        exit(-5);
    }

    *resultReply = kvsReply;
             
    if ( kvsReply->type == REDIS_REPLY_ERROR) {
        log_msg( "kvs_RedisCommand: ERROR - Redis says: %s\n", kvsReply->str);
        freeReplyObject( kvsReply);
        *resultReply = NULL;    // Releasing it here upon error keeps code a little cleanner
        return -EIO;
    }
    return 0;
}

// Disconnects from redis. 
void kvs_Cleanup( void)
{
    redisFree(redisCtx);
}

// Creates an empty redis key to represent an empty file
int kvs_CreateEmptyKey( const char *name)
{
    redisReply *reply;
    int result;
    
    result = kvs_RedisCommand( &reply, "SET %s %s", name, "");

    if (result >= 0) 
        freeReplyObject(reply);
    return result;
}


// Checks if a key (representing a file) already exists
int kvs_KeyExists( const char *name)
{
    redisReply *reply;
    int exists, result;

    result = kvs_RedisCommand( &reply, "EXISTS %s", name);
    if (result < 0)
        return result;
    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "kvs_KeyExists: ERROR - Unexpected response from redis type=%d\n", 
                 reply->type);
        freeReplyObject(reply);
        return -EPROTO;
    }
    exists = (int) reply->integer; // 1 if exists, 0 otherwise
    freeReplyObject(reply);
    
    return exists;  
}

// Delete key from KVS
int kvs_DeleteKey( const char *name)
{
    redisReply *reply;
    int result;

    result = kvs_RedisCommand( &reply, "DEL %s", name);
    if ( result < 0 )
        return result;
    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "kvs_DeleteKey: ERROR - Unexpected result from redis type=%d\n", reply->type);
        freeReplyObject(reply);
        return -EPROTO;
    }
    
    if ( reply->integer == 0)  // 1 if key existed, 0 otherwise
        return -ENOENT;
    freeReplyObject(reply);

    return 0;
}

// Rename key in KVS
int kvs_RenameKey( const char *name, const char *newname)
{
    redisReply *reply;
    int result;

    // Some KVS will blindly replace existing keys, wich is the expected FS behaviour
    // Redis does blindly replace!
    result = kvs_RedisCommand( &reply, "RENAME %s %s", name, newname);
    if ( result < 0)
        return result; 

    freeReplyObject(reply);
    
    return 0;
}


// Get length of a key (known to exist, if not redis returns len=0)
size_t kvs_GetKeyLength( const char *name)
{
    redisReply *reply;
    size_t ksize;
    int result;
    
    // Redis returns length 0 for nonexisting keys, so explicitly check
    result = kvs_KeyExists( name);
    if ( result < 0)    // redis error
        return result;
    if ( result == 0)
        return -ENOENT;
        
    result = kvs_RedisCommand( &reply, "STRLEN %s", name);
    if ( result < 0)    // redis error
        return result;
    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "kvs_GetKeyLength: ERROR - Unexpected result from redis type=%d\n", 
                 reply->type);
        freeReplyObject(reply);
        return -EPROTO;
    }
    ksize = (size_t)reply->integer;
    freeReplyObject(reply);
    return ksize;
}

// Extends the value of an existing key (must exist) using null characters.
// Caller must ensure newsize is larger than current size 
int kvs_AppendZeroedBytes( const char *name, size_t newsize)
{
    redisReply *reply;
    char zbuffer[1] = {0};
    int result;
    
    // Extending a key's value is really a corner case. Take advantage that redis does it
    // automatically when we set bytes beyond current size
    result = kvs_RedisCommand( &reply, "SETRANGE %s %ld %b", name, newsize - 1, zbuffer, 1);
    if ( result < 0)    // redis error
        return result;

    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "kvs_AppendZeroedBytes: ERROR - Unexpected result from redis type=%d\n",
                 reply->type);
        freeReplyObject(reply);
        return -EPROTO;
    }
    freeReplyObject(reply);

    return 0;
}

// Truncates the value of an existing key discarding the trailing content
int kvs_TruncateKey( const char *name, size_t newsize)
{
    redisReply *reply1 = NULL,
               *reply2;
    int result;

    if ( newsize > 0 ) {    // Need to preserve beginning of value 
        result = kvs_RedisCommand( &reply1,"GETRANGE %s %ld %ld", name, (size_t)0, newsize);
        if ( result < 0)
            return result;
        if (reply1->type != REDIS_REPLY_STRING) {
            log_msg( "kvs_TruncateKey: ERROR - Unexpected result from redis type=%d\n", 
                     reply1->type);
            freeReplyObject(reply1);
            return -EPROTO;
        }
    }
    result = kvs_RedisCommand( &reply2, "SET %s %b", name, 
                           newsize > 0 ? reply1->str : "", newsize);
    if ( reply1 != NULL)
        freeReplyObject(reply1);
    if ( result >= 0)
        freeReplyObject(reply2);
    return result;
}

    
// This will copy the root directory file list into the FUSE buffer using the FUSE 
// 'filler' function.
//
// TODO: Passing the FUSE filler function to this KVS abstraction layer decouples  
//       the FUSE code from redis, but not vice versa. Ideally this abstraction layer 
//       would simply return a list of strings (entries), and the calling code would 
//       transfer them to FUSE. However this incurs a penalty allocating space for a 
//       variable size list and deallocating it soon after. The implementation below
//       represents an acceptable compromise, given the purpose of this program.
//
// TODO: Function prototype is not adequate to support subfolders eventually. Root 
//       folder is implicitly assumed.
//
// TODO: Fuse4redis creates only string values. However if keys with other value types 
//       (e.g. integer) are created using redis-cli, these keys will result in errors 
//       when accessed.
//
int kvs_ReadDirectory( void *buf, fuse_fill_dir_t filler)
{
    redisReply *reply;
    int result;
      
    result = kvs_RedisCommand( &reply, "KEYS *");
    if ( result < 0)
        return result;
        
   // The loop below exits when either all keys were copied, or filler()
   // returns something non-zero.  The first case just means I've
   // read the whole 'redis' directory; the second means the buffer is full.
    if (reply->type == REDIS_REPLY_ARRAY) {
        int j;
        for ( j = 0; j < reply->elements; j++) {
            if (filler(buf, reply->element[j]->str, NULL, 0) != 0) {
	            log_msg("kvs_ReadDirectory: ERROR - filler returned buffer full\n");
                freeReplyObject(reply);
	            return -ENOMEM;
	        }
        }
    } else {  // Only array is an acceptable result
        log_msg( "kvs_ReadDirectory: ERROR - query did not return a list, returned type=%d\n",
                 reply->type);
        return -EPROTO;
    }
    freeReplyObject(reply);
    return 0;
}

// Reads the partial contents of a key starting at offset
int kvs_ReadPartialValue(const char *keyname, char *buf, size_t size, off_t offset)
{
    redisReply *reply;
    int length, result;
  
    // Redis has command to get substrings, which is handy!
    result = kvs_RedisCommand(&reply,"GETRANGE %s %ld %ld", keyname,
                         offset, offset + size - 1);  // Assuming size will never be 0
    if ( result < 0)
        return result;
    if (reply->type != REDIS_REPLY_STRING) {
        log_msg( "kvs_ReadPartialValue: ERROR - Unexpected result from redis type=%d\n", 
                 reply->type);
        freeReplyObject(reply);
        return -EPROTO;
    }
    
    length = reply->len;
    memcpy( buf, reply->str, length);
    freeReplyObject(reply);
  
    return length;
}

// Writes/overwrites the partial contents of a key starting at offset
int kvs_WritePartialValue(const char *keyname, const char *buf, size_t size, off_t offset)
{
    redisReply *reply;
    int result;
  
    // Redis has a command to write partial values of keys, which is handy!
    // Impressively, redis handles writes beyond the current length as expected, 
    // including filling with zeroes when offset is beyond current length. In a nutshell,
    // it already implements the same semantics a the write call in Linux. Nice!!!
    // But beware, different from write, redis returns the resulting total length of 
    // the new key.
    result = kvs_RedisCommand(&reply,"SETRANGE %s %ld %b", keyname,
                         offset, buf, size);
    if ( result < 0)
        return result;

    if (reply->type != REDIS_REPLY_INTEGER) {
        log_msg( "kvs_WritePartialValue: ERROR - Unexpected result from redis type=%d\n", 
                 reply->type);
        freeReplyObject(reply);
        return -EPROTO;
    }
    freeReplyObject(reply);
    
    return size; // Success: return number of bytes in buf actually written.
}
//////

//
// End of section that abstracts database (KVS) details
//
//////////////////////////////////////////////////////////////////////



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
int f4r_getattr(const char *path, struct stat *statbuf)
{
    size_t fsize = 0;
    const char *filename = FILE_NAME(path);
    
    log_msg( "f4r_getattr: Called for path=%s\n", path);
    
    statbuf->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    if (strcmp(path, "/") == 0) {   // Atributes for the FS' root dir
        statbuf->st_mode = statbuf->st_mode | S_IFDIR;
        statbuf->st_size = 0;
    } else {
        // First check if file/key exists, because STRLEN simply returns 0 if it doesn't
        int exists = kvs_KeyExists( filename);
        
        if (exists < 0 )
            return exists;

        if ( ! exists)   // Key does not exist
            return -ENOENT;

        statbuf->st_mode = statbuf->st_mode | S_IFREG;
        fsize = kvs_GetKeyLength( filename);
        if ( fsize < 0 )
            return fsize;
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
// less than the size passed to f4r_readlink()
// f4r_readlink() code by Bernardo F Costa (thanks!)
int f4r_readlink(const char *path, char *link, size_t size)
{
    log_msg( "f4r_readlink: Called for path=%s\n", path);
    
    return -ENOSYS;     // Not supported
}

/** Create a file node
 *
 * There is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
int f4r_mknod(const char *path, mode_t mode, dev_t dev)
{
    const char *filename = FILE_NAME(path);
    int result;
    
    log_msg( "f4r_mknod: Called for path=%s\n", path);
    
    if ( ! S_ISREG(mode)) // fuse4redis only support regular file creation
        return -EINVAL;
        
    // With O_EXCL, file/key cannot already exist 
    if (mode & O_EXCL) {
        int exists = kvs_KeyExists( filename);
        
        if (exists < 0)
            return exists;

        if ( exists)
            return -EEXIST;
    }
    
    // Create an empty redis key to represent an empty file
    result = kvs_CreateEmptyKey( filename);
    if ( result < 0)
        return result;
 
    return 0;
}

/** Create a directory */
int f4r_mkdir(const char *path, mode_t mode)
{
    log_msg( "f4r_mkdir: Called for path=%s\n", path);
    return -ENOSYS;
}

/** Remove a file */
int f4r_unlink(const char *path)
{    
    log_msg( "f4r_unlink: Called for path=%s\n", path);

    if (strcmp(path, "/") == 0) {   // Trying to delete the FS' root dir
        return -EISDIR;
    }

    return kvs_DeleteKey( FILE_NAME(path));
}

/** Remove a directory */
int f4r_rmdir(const char *path)
{
    log_msg( "f4r_rmdir: Called for path=%s\n", path);

    return -ENOSYS;
}


/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.
int f4r_symlink(const char *path, const char *link)
{
    log_msg( "f4r_symlink: Called for path=%s\n", path);
    
    return -ENOSYS;
}

/** Rename a file */
// both path and newpath are fs-relative
int f4r_rename(const char *path, const char *newpath)
{
    const char *filename = FILE_NAME(path),
               *newname = FILE_NAME(newpath);
    
    log_msg( "f4r_rename: Called for path=%s newpath=%s\n", path, newpath);
    
     return kvs_RenameKey( filename, newname);
}

/** Create a hard link to a file */
int f4r_link(const char *path, const char *newpath)
{
    log_msg( "f4r_link: Called for path=%s\n", path);
    
    return -ENOSYS;
}

/** Change the permission bits of a file */
int f4r_chmod(const char *path, mode_t mode)
{
    log_msg( "f4r_chmod: Called for path=%s\n", path);
    return -ENOSYS;
}

/** Change the owner and group of a file */
int f4r_chown(const char *path, uid_t uid, gid_t gid)
{
    log_msg( "f4r_chown: Called for path=%s\n", path);
    return -ENOSYS;
}

/** Change the size of a file */
int f4r_truncate(const char *path, off_t newsize)
{
    size_t ksize;
    const char *filename = FILE_NAME( path);
    
    log_msg( "f4r_truncate: Called for path=%s\n", path);
    
    ksize = kvs_GetKeyLength( filename);
    if ( ksize < 0)
        return ksize;
        
    if ( ksize == newsize)
        return 0;       // Nothing to be done

    // Documentation says semantics for setting new size beyond current size
    // is implementation dependent. We will pad the file with null bytes.
    if ( newsize > ksize)
        return kvs_AppendZeroedBytes( filename, newsize);
    else
        return kvs_TruncateKey( filename, newsize);
}

/** Change the access and/or modification times of a file */
int f4r_utime(const char *path, struct utimbuf *ubuf)
{
    log_msg( "f4r_utime: Called for path=%s\n", path);
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
int f4r_open(const char *path, struct fuse_file_info *fi)
{
    int exists, result;
    const char *filename = FILE_NAME( path);
    
    log_msg( "f4r_open: Called for path=%s\n", path);
    
    if (strcmp(path, "/") == 0) {   // Trying to open the FS' root dir
        return -EISDIR;
    }
    
    exists = kvs_KeyExists( filename);
    if ( exists < 0)
        return exists;

    if ( ! exists) {
        if( fi->flags & O_CREAT) {
            // Create an empty redis key to represent an empty file
            result = kvs_CreateEmptyKey( filename);
            if ( result < 0)
                return result;
        } else        
            return -ENOENT;
    } else {
        if( fi->flags & O_TRUNC) {
            // Empty the file, if existing
            result = kvs_TruncateKey( filename, 0);
            if ( result < 0)
                return result;
        }
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
int f4r_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    log_msg( "f4r_read: Called for path=%s\n", path);
    
    if (strcmp(path, "/") == 0) {   // Trying to read the FS' root dir
        return -EISDIR;
    }

    // Note that we do not check if file is open for reading. Other layers
    // in the FS stack already do it.        
    return kvs_ReadPartialValue(FILE_NAME(path), buf, size, offset);
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int f4r_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
    log_msg( "f4r_write: Called path=%s\n", path);

    if (strcmp(path, "/") == 0) {   // Trying to read the FS' root dir
        return -EISDIR;
    }
    
    // Note that we do not check if file is open for writing. Other layers
    // in the FS stack already do it.        
    return kvs_WritePartialValue( FILE_NAME(path), buf, size, offset);
}

/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 * Replaced 'struct statfs' parameter with 'struct statvfs' in
 * version 2.5
 */
int f4r_statfs(const char *path, struct statvfs *statv)
{
    log_msg( "f4r_statfs: Called for path=%s\n", path);
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
int f4r_flush(const char *path, struct fuse_file_info *fi)
{
    log_msg( "f4r_flush: Called path=%s\n", path);
    
    return 0;   // No op. Nothing to flush to KVS.
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
int f4r_release(const char *path, struct fuse_file_info *fi)
{
    log_msg( "f4r_release: Called for path=%s\n", path);

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
int f4r_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    log_msg( "f4r_fsync: Called for path=%s\n", path);
    return 0;
}

#ifdef HAVE_SYS_XATTR_H
/** Set extended attributes */
int f4r_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    log_msg( "f4r_setxattr: Called for path=%s\n", path);
    return -ENOSYS;
}

/** Get extended attributes */
int f4r_getxattr(const char *path, const char *name, char *value, size_t size)
{
    log_msg( "f4r_getxattr: Called for path=%s\n", path);
    return -ENOSYS;
}

/** List extended attributes */
int f4r_listxattr(const char *path, char *list, size_t size)
{
    log_msg( "f4r_listxattr: Called for path=%s\n", path);
    return -ENOSYS;
}

/** Remove extended attributes */
int f4r_removexattr(const char *path, const char *name)
{
    log_msg( "f4r_removexattr: Called for path=%s\n", path);
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
int f4r_opendir(const char *path, struct fuse_file_info *fi)
{
    log_msg( "f4r_opendir: Called for path=%s\n", path);
    
    if (strcmp(path, "/") != 0) {   // Trying to open dir other than FS' root dir
        return -ENOTDIR;
    }

    // No handles maintained for root dir. Nothing to do. 
    return 0;
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

int f4r_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
    log_msg( "f4r_readdir: Called for path=%s\n", path);
    
    if (strcmp(path, "/") != 0)   // Only the FS' root dir is currently allowed
        return -ENOTDIR;
    
    return kvs_ReadDirectory( buf, filler);
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int f4r_releasedir(const char *path, struct fuse_file_info *fi)
{
    log_msg( "f4r_releasedir: Called for path=%s\n", path);
    
    // We do not keep any oopen directory related state. Nothing to do here!
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
int f4r_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
    log_msg( "f4r_fsyncdir: Called for path=%s\n", path);
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
void *f4r_init(struct fuse_conn_info *conn)
{
    log_msg( "f4r_init: Called init. FUSE is initializing!\n");
    
    return F4R_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void f4r_destroy(void *userdata)
{
    log_msg( "f4r_destroy: Called cleanup operation.\n");
    
    kvs_Cleanup();
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
int f4r_access(const char *path, int mask)
{
    log_msg( "f4r_access: Called for path=%s with mask=%d\n", path, mask);
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
int f4r_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
    log_msg( "f4r_ftruncate: Called for path=%s\n", path);
    
    // Since we have the path and do not use handles, ftruncate and truncate are equal
    return f4r_truncate( FILE_NAME( path), offset);
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
int f4r_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
    log_msg( "f4r_fgetattr: Called for path=%s\n", path);

    // Since we do not keep file handles, we simply delegate to f4r_getattr()
    return f4r_getattr( FILE_NAME( path), statbuf);
}

struct fuse_operations f4r_oper = {
  .getattr = f4r_getattr,
  .readlink = f4r_readlink,
  // no .getdir -- that's deprecated
  .getdir = NULL,
  .mknod = f4r_mknod,
  .mkdir = f4r_mkdir,
  .unlink = f4r_unlink,
  .rmdir = f4r_rmdir,
  .symlink = f4r_symlink,
  .rename = f4r_rename,
  .link = f4r_link,
  .chmod = f4r_chmod,
  .chown = f4r_chown,
  .truncate = f4r_truncate,
  .utime = f4r_utime,
  .open = f4r_open,
  .read = f4r_read,
  .write = f4r_write,
  /** Just a placeholder, don't set */ // huh???
  .statfs = f4r_statfs,
  .flush = f4r_flush,
  .release = f4r_release,
  .fsync = f4r_fsync,
  
#ifdef HAVE_SYS_XATTR_H
  .setxattr = f4r_setxattr,
  .getxattr = f4r_getxattr,
  .listxattr = f4r_listxattr,
  .removexattr = f4r_removexattr,
#endif
  
  .opendir = f4r_opendir,
  .readdir = f4r_readdir,
  .releasedir = f4r_releasedir,
  .fsyncdir = f4r_fsyncdir,
  .init = f4r_init,
  .destroy = f4r_destroy,
  .access = f4r_access,
  .ftruncate = f4r_ftruncate,
  .fgetattr = f4r_fgetattr
};


int main(int argc, char *argv[])
{
    int fuse_stat;
    struct f4r_state *f4r_data;


    // See which version of fuse we're running
    fprintf(stderr, "Fuse library version %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
    
    // Perform some sanity checking on the command line:  make sure
    // there are enough arguments, and that mount folder does not
    // start with a hyphen (this will break if you actually have a
    // mountpoint whose name starts with a hyphen, but so will 
    // other programs too)
    if ((argc < 2) || (argv[argc-1][0] == '-')) {
        fprintf(stderr, "usage:  fuse4redis [FUSE and mount options] mountPoint\n");
        exit( -1);
    }
 	
    f4r_data = malloc(sizeof(struct f4r_state));
    if (f4r_data == NULL) {
        perror("main calloc");
        abort();
    }
    f4r_data->logfile = log_open();


    // TODO: implement option to connect to a remote redis host
    kvs_init(hostname, port);
    
    // turn over control to fuse
    
    fuse_stat = fuse_main(argc, argv, &f4r_oper, f4r_data);
    
    return fuse_stat;
}
