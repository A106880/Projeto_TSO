CC=gcc
CFLAGS= -Wall
DEPS = metaInfo.h
OBJ = passthrough.o main.o

LIBS=`pkg-config --cflags --libs glib-2.0` `pkg-config fuse3 --cflags --libs` -lpthread -lbz2 -lcrypto

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS) $(LIBS)

passthrough: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f *.o passthrough