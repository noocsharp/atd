CC ?= gcc
CFLAGS = 

SRC = atd.c atc.c atsim.c util.c
OBJ = $(SRC:.c=.o)

all: atd atc atsim

atd: atd.o util.o
	$(CC) $(CFLAGS) atd.o util.o -o atd

atc: atc.o
	$(CC) $(CFLAGS) atc.o -o atc

atsim: atsim.o
	$(CC) $(CFLAGS) atsim.o -o atsim

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJ) atd atc atsim
