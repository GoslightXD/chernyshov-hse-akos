CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
COMMON = common.c core.c main.c common.h sync.h
SRC = common.c core.c main.c

.PHONY: all tsan clean
all: cannibals_sem cannibals_cond

cannibals_sem: $(COMMON) sync_sem.c
	$(CC) $(CFLAGS) -o $@ $(SRC) sync_sem.c

cannibals_cond: $(COMMON) sync_cond.c
	$(CC) $(CFLAGS) -o $@ $(SRC) sync_cond.c

# сборка с ThreadSanitizer для поиска гонок
tsan:
	$(CC) -Wall -Wextra -g -O1 -pthread -fsanitize=thread -o cannibals_sem_tsan $(SRC) sync_sem.c
	$(CC) -Wall -Wextra -g -O1 -pthread -fsanitize=thread -o cannibals_cond_tsan $(SRC) sync_cond.c

clean:
	rm -f cannibals_sem cannibals_cond cannibals_sem_tsan cannibals_cond_tsan *.log