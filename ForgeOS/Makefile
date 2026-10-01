CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread
LDFLAGS=-lncurses -pthread

all: forgeos

forgeos: main.c
	$(CC) $(CFLAGS) main.c -o forgeos $(LDFLAGS)

clean:
	rm -f forgeos

run: forgeos
	./forgeos
