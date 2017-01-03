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

// Test open and close
//
void test_open( void)
{
    int fd;
    char filename[ 32];
    
    sprintf( filename, "testfile%d", rand());
    
    fd = open( filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if ( fd < 0 )
        printf( "test_open: Open failed with error %d\n", errno);
    else if ( close(fd) < 0)
        printf( "test_open: close failed with error %d\n", errno);
    else
        printf( "test_open: Success\n");
}


// Test open and close
//
void test_offset( void)
{
    int fd, result;
    char filename[ 32];
    char buffer1[ 512],
         buffer2[ 512];
    
    sprintf( filename, "testfile%d", rand());
    
    fd = open( filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if ( fd < 0 )
        printf( "test_offset: Open failed with error %d\n", errno);
        

    memset( buffer1, '*', 512);
    if ( write( fd, buffer1, 512) < 512)
        printf( "test_offset: First write failed\n");
        
    if ( lseek( fd, 80, SEEK_SET) < 0 )
        printf( "test_offset: first lseek failed\n");
        
    memset( buffer1 + 80, '=', 80);  // Change 80 bytes starting at offset 80
    if ( write( fd, buffer1 + 80, 80) < 80)  // and write only bytes changed
        printf( "test_offset: Second write failed\n");
        
    if ( lseek( fd, 0, SEEK_SET) < 0 )
        printf( "test_offset: second lseek failed\n");
    result = read( fd, buffer2, 512);
    if ( result  < 512)
        printf( "test_offset: Read failed read %d bytes\n", result);

    if ( memcmp( buffer1, buffer2, 512) != 0 )
        printf( "test_offset: Comparison failed\n");
    
    if ( close(fd) < 0)
        printf( "test_offset: Close failed with error %d\n", errno);
    else
        printf( "test_offset: Success\n");
}

int main( int argc, char*argv[])
{

    srand( time( NULL));

    test_open();
    test_offset();
}


