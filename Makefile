CC=g++
CFLAGS=-I.

all: fwdd fwd

fwdd: fwdd.o
	$(CC) -o fwdd fwdd.o

fwd: fwd.o
	$(CC) -o fwd fwd.o