CC?=gcc
CFLAGS=-O2 -Wall -Wextra
LDFLAGS=-lnuma

BINS=mpipin

.PHONY: all clean
all: $(BINS)

%: %.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(BINS)
