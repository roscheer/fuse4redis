This is a simple FUSE filesystem to demonstrate both how to create a file system in user space and exercise redis KVS' hiredis API.

The part of the code that interacts with redis is clearly separate from the code that implements the file system logic, even though currently everything is still in a single source file. This separation aims to simplify the change to a diferent KVS database.

Since this is useful for educational purposes only, not much efoort was put on performance. Not all file system functionality is implemented, either.

