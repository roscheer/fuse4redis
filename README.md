This is a simple FUSE filesystem to demonstrate both how to create a file system in user space and interact with the Redis KVS using hiredis API.

The part of the code that interacts with Redis is clearly separate from the code that implements the file system logic, even though currently everything is still in a single source file. This separation aims to simplify the change to a diferent KVS database, if anyone eventually desires to do so.

Since this is useful for educational purposes only, not much effort was put on performance. Not all file system functionality is implemented, either.

A test program to exercise most of the functionality implemented by fuse4redis is provided in the file 'f4r_test.c'. This program uses the CUnit test framework. 

The code is based on the FUSE tutorial created by Joseph J. Pfeiffer, Jr. (http://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/). Most of the code was changed, however. Only the FUSE callbacks prototypes, FUSE initialization, and the logging functionality, are actually being reused. The logging functionality is really useful for debugging purposes, since FUSE disconnects from the terminal when running.

This was developed and tested on Ubuntu 16.04 and SUSE Linux Enterprise Desktop SP2 only, using the pre-packaged versions of fuse and libfuse-dev packages provided by these distributions. It should compile and run on different distributions, though it was not yet tested.

Redis installation was performed according to documentation that can be found here: https://www.digitalocean.com/community/tutorials/how-to-install-and-configure-redis-on-ubuntu-16-04 (these instructions work on SUSE, just use the useradd/groupadd commands instead of adduser for creating the redis user and group accounts).

The hiredis API, used to access Redis, was downloaded and built from here: https://github.com/redis/hiredis .

