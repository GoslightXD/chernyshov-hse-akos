CC = gcc
CFLAGS = -Wall -Wextra -O2
HDR = common.h net.h
BASE = common.c net.c

.PHONY: all clean
all: cannibals_server cannibals_cook cannibals_savage cannibals_observer

cannibals_server: $(BASE) server.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(BASE) server.c

cannibals_cook: $(BASE) cook.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(BASE) cook.c

cannibals_savage: $(BASE) savage.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(BASE) savage.c

cannibals_observer: $(BASE) observer.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(BASE) observer.c

clean:
	rm -f cannibals_* *.log
