This is a simple FUSE filesystem to demonstrate both how to create a file system in user space and exercise redis KVS' hiredis API.

The part of the code that interacts with redis is clearly separate from the code that implements the file system logic, even though currently everything is still in a single source file. This separation aims to simplify the change to a diferent KVS database.

Since this is useful for educational purposes only, not much efoort was put on performance. Not all file system functionality is implemented, either.

The code is based on the FUSE tutoria created by Joseph J. Pfeiffer, Jr. (http://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/). Most of the code was changed, however. Only the FUSE callbacks prototypes, and the logging functionality, are actually being reused.


