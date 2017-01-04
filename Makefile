# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY, to the extent permitted by law; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE.

CFLAGS = `pkg-config fuse --cflags` `pkg-config hiredis --cflags`
LIBS = `pkg-config fuse --libs` `pkg-config hiredis --libs`

LIBCUNIT = `pkg-config cunit --libs`

DEPS = log.h params.h

%.o: %.c $(DEPS)
	gcc -c -o $@ $< $(CFLAGS)

fuse4redis: fuse4redis.o log.o
	gcc -o $@ $^ $(CFLAGS) $(LIBS)

f4r_test: f4r_test.o
	gcc -o $@ $^ $(LIBCUNIT)

.PHONY: clean

clean:
	rm -f fuse4redis *.o *~ core

