CC=gcc
CFLAGS=-Wall -Wextra -O2

.PHONY: all clean
all: cannibals

cannibals: cannibals.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f cannibals *.log