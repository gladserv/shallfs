all : mkshallfs readshallfs shallfsck shalluserlog

PREFIX = /usr/local

CFLAGS += -D_FILE_OFFSET_BITS=64 -I../include -I../shallfs -Wall -O2

mkshallfs : mkshallfs.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o mkshallfs mkshallfs.o shallfs-common.o

mkshallfs.o : mkshallfs.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o mkshallfs.o mkshallfs.c

readshallfs : readshallfs.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o readshallfs readshallfs.o shallfs-common.o

readshallfs.o : readshallfs.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o readshallfs.o readshallfs.c

shallfsck : shallfsck.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -lm -o shallfsck shallfsck.o shallfs-common.o

shallfsck.o : shallfsck.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shallfsck.o shallfsck.c

shalluserlog : shalluserlog.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o shalluserlog shalluserlog.o shallfs-common.o

shalluserlog.o : shalluserlog.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shalluserlog.o shalluserlog.c

shallfs-common.o : shallfs-common.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shallfs-common.o shallfs-common.c

install :
	install -d $(PREFIX)/sbin
	install mkshallfs readshallfs shallfsck shalluserlog $(PREFIX)/sbin

clean :
	rm -f *.o mkshallfs readshallfs shallfsck shalluserlog

