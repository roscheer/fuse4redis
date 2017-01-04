/*
  Tests part of the standard functionality of any file system. 
  However it was developed to provide unit tests for the fuse4redis FUSE implementation.
  
  Copyright (C) 2017 Roque Luis Scheer <roqscheer@gmail.com>

  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
  
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <sys/types.h>

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>


// Test open and close
//
void test_open( void)
{
    int fd;
    char filename[ 32];
    
    sprintf( filename, "testfile%d", rand());
    
    fd = open( filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);

    CU_ASSERT( close(fd) >= 0);
    
    // Open without O_CREAT, must exist
    fd = open( filename, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);

    CU_ASSERT( close(fd) >= 0);
    
    CU_ASSERT( unlink( filename) == 0);
}


// Test file offset setting and handling
//
void test_offset( void)
{
    int fd, result;
    char filename[ 32];
    char buffer1[ 512],
         buffer2[ 512];
    
    sprintf( filename, "testfile%d", rand());
    
    fd = open( filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT(  fd >= 0 );        

    memset( buffer1, '*', 512);
    CU_ASSERT( write( fd, buffer1, 512) == 512);        
    CU_ASSERT( lseek( fd, 80, SEEK_SET) >= 0 );
        
    memset( buffer1 + 80, '=', 80);  // Change 80 bytes starting at offset 80
    CU_ASSERT( write( fd, buffer1 + 80, 80) == 80);  // and write only bytes changed
        
    CU_ASSERT( lseek( fd, 0, SEEK_SET) >= 0 );

    // Read entire file and compare
    result = read( fd, buffer2, 512);
    CU_ASSERT( result == 512);

    CU_ASSERT( memcmp( buffer1, buffer2, 512) == 0 );
    
    CU_ASSERT( close(fd) >= 0);
    
    CU_ASSERT( unlink( filename) == 0);
}

// Test extending files
//
void test_extend( void)
{
    int fd;
    char filename[ 32];
    char buffer1[ 1024] = {0},
         buffer2[ 1024];
         
    sprintf( filename, "testfile%d", rand());
    
    fd = open( filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0 );
    
    memset( buffer1, '*', 512);
    CU_ASSERT( write( fd, buffer1, 512) == 512);
    
    CU_ASSERT( lseek( fd, 1000, SEEK_SET) >= 0 );
        
    memset( buffer1 + 1000, '=', 24);  // Write 24 bytes at position 1000 (beyond current size) 
    CU_ASSERT( write( fd, buffer1 + 1000, 24) == 24);  // and write only bytes changed
        
    CU_ASSERT( lseek( fd, 0, SEEK_SET) >= 0 );

    // Read entire file and compare. File must have been extended and filled with
    // zeros in the range [512-1000]
    CU_ASSERT( read( fd, buffer2, 1024) == 1024);

    CU_ASSERT( memcmp( buffer1, buffer2, 1024) == 0 );
      
    CU_ASSERT( close(fd) >= 0);
    CU_ASSERT( unlink( filename) == 0);
}    

// Test truncating files
//
void test_truncate( void)
{
    int fd, result;
    char filename[ 32];
    char buffer1[ 1024] = {0},
         buffer2[ 1024];
         
    sprintf( filename, "testfile%d", rand());
    
    fd = open( filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0 );
    
    memset( buffer1, '*', 1024);
    CU_ASSERT( write( fd, buffer1, 1024) == 1024);
    
    // Read entire file. File must have be 1024 bytes
    CU_ASSERT( lseek( fd, 0, SEEK_SET) >= 0 );
    result = read( fd, buffer2, 1024);
    CU_ASSERT( result == 1024);
    
    CU_ASSERT( ftruncate( fd, 512) == 0 );

    // Read entire file. File must have been reduced to 512
    CU_ASSERT( lseek( fd, 0, SEEK_SET) >= 0 );
    result = read( fd, buffer2, 1024);  // Attempt or read beyond end
    CU_ASSERT( result == 512);

    CU_ASSERT( ftruncate( fd, 0) == 0 );

    // Read entire file and compare. File must have been reduced to 512
    CU_ASSERT( lseek( fd, 0, SEEK_SET) >= 0 );
    result = read( fd, buffer2, 1024);  // Attempt or read beyond end
    CU_ASSERT( result == 0);    // File must be empty

    CU_ASSERT( close(fd) >= 0);
    CU_ASSERT( unlink( filename) == 0);
}    

// Test rename
//
void test_rename( void)
{
    int fd;
    char filename1[ 32],
         filename2[ 32],
         randomstr[ 32] = {0},
         buffer[ 32];
    
    sprintf( filename1, "testfile%d", rand());
    sprintf( filename2, "testfile%d", rand());
    sprintf( randomstr, "randomic text %d", rand());
    
    // Create file 1
    fd = open( filename1, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);
    CU_ASSERT( write( fd, randomstr, 32) == 32);  // Write unique content
    CU_ASSERT( close(fd) >= 0);
    
    // Create file 2
    fd = open( filename2, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);
    CU_ASSERT( close(fd) >= 0);
    
    CU_ASSERT( rename( filename1, filename2) == 0);  // Must replace existing file
    
    // Open without O_CREAT, must not exist
    fd = open( filename1, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd < 0);
 
     // Open without O_CREAT, must exist
    fd = open( filename2, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);
    
    // Read and verify that content written to file named filename1 is in filename2
    CU_ASSERT( read( fd, buffer, 32) == 32);
    CU_ASSERT( memcmp( buffer, randomstr, 32) == 0 );

    CU_ASSERT( close(fd) >= 0);
    CU_ASSERT( unlink( filename2) == 0);
}

// Test flags protection flags passed to open
//
void test_openflags( void)
{
    int fd;
    char filename[ 32],
         buffer[ 32] = {0};
    
    sprintf( filename, "testfile%d", rand());
    
    // Create file and open it write only
    fd = open( filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);
    CU_ASSERT( write( fd, buffer, 32) == 32);  // Write some content

    // Attempt to read the file, must fail
    CU_ASSERT( read( fd, buffer, 32) < 0);
    CU_ASSERT( errno == EBADF);

    CU_ASSERT( close(fd) >= 0);
    
    // Reopen it read only
    fd = open( filename, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    CU_ASSERT( fd >= 0);
    
    // Attempt to write some content, must fail
    CU_ASSERT( write( fd, buffer, 32) < 0);  
    CU_ASSERT( errno == EBADF);

    CU_ASSERT( close(fd) >= 0);
    CU_ASSERT( unlink( filename) == 0);
}

int main( int argc, char*argv[])
{
    CU_pSuite pSuite;
    
    srand( time( NULL));
    
    if ( CU_initialize_registry() != CUE_SUCCESS ) {
        fprintf( stderr, "Failed to initialize CUnit\n");
        exit( -1);
    }
    pSuite = CU_add_suite( "FUSE_for_Redis", NULL, NULL);
    if ( pSuite == NULL) {
        fprintf( stderr, "Failed to add suite error=%d\n", CU_get_error());
        exit( -2);
    }
    
    CU_ADD_TEST(pSuite, test_open);
    CU_ADD_TEST(pSuite, test_offset);
    CU_ADD_TEST(pSuite, test_extend);
    CU_ADD_TEST(pSuite, test_truncate);
    CU_ADD_TEST(pSuite, test_rename);
    CU_ADD_TEST(pSuite, test_openflags);
    
    CU_basic_set_mode( CU_BRM_VERBOSE);
    CU_basic_run_tests();
    
    CU_cleanup_registry();
}


