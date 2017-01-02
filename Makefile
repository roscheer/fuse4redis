# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY, to the extent permitted by law; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE.

FUSE_PARAMS = `pkg-config fuse --cflags` `pkg-config fuse --libs`
REDIS_PARAMS = `pkg-config hiredis --cflags` `pkg-config hiredis --libs`

all:	fuse4redis.c log.c log.h params.h
	gcc -Wall -g -O2 -o fuse4redis fuse4redis.c log.c $(FUSE_PARAMS) $(REDIS_PARAMS)
            
clean:
	rm fuse4redis
                    

