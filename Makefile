CC?=gcc
CFLAGS=-O2 -Wall -Wextra -I./include -I./include/arch/x86/
LDFLAGS=-lnuma

BINS=mpipin

.PHONY: all clean
all: $(BINS)

mpipin: mpipin.o bitmap.o bitops.o
	$(CC) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -rf $(BINS) *.o
