This is a simple FUSE filesystem to demonstrate both how to create a file system in user space and interact with the Redis KVS using hiredis API.

The part of the code that interacts with Redis is clearly separate from the code that implements the file system logic, even though currently everything is still in a single source file. This separation aims to simplify the change to a diferent KVS database, if anyone eventually desires to do so.

Since this is useful for educational purposes only, not much effort was put on performance. Not all file system functionality is implemented, either.

The code is based on the FUSE tutorial created by Joseph J. Pfeiffer, Jr. (http://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/). Most of the code was changed, however. Only the FUSE callbacks prototypes, FUSE initialization, and the logging functionality, are actually being reused. The logging functionality is really useful for debugging purposes, since FUSE disconnects from the terminal when running.

This was developed and tested on Ubuntu 16.04 only, using Ubuntu's pre-packaged versions of fuse and libfuse-dev packages. It should compile and run on different distributions, though it was not yet tested.

Redis installation was performed according to documentation that can be found here: https://www.digitalocean.com/community/tutorials/how-to-install-and-configure-redis-on-ubuntu-16-04 .

The hiredis API, used to access Redis, was downloaded and built from here: https://github.com/redis/hiredis .

