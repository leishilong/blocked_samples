CC=gcc
CFLAGS=-g -gdwarf-4 -fno-omit-frame-pointer -Wl,--no-as-needed -ldl -Wl,--rpath=/usr/local/lib/glibc-testing/lib -Wl,--dynamic-linker=/trusted/local/lib/glibc-testing/lib/ld-linux.so.2 -lpthread

TARGET=test_io
SRC=test_io.c

TARGET2=test_io_bcoz
SRC2=test_io_bcoz.c

all: $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET2) $(SRC2)

clean:
	rm $(TARGET) $(TARGET2)
