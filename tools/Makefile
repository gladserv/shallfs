all : mkshallfs readshallfs shallfsck

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

shallfs-common.o : shallfs-common.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shallfs-common.o shallfs-common.c

clean :
	rm -f *.o mkshallfs readshallfs

