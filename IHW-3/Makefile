CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -pthread -lrt
HDR = common.h shm.h

.PHONY: all clean
all: cannibals_mq_host cannibals_mq_cook cannibals_mq_savage cannibals_fork_named cannibals_fork_unnamed cannibals_host cannibals_cook cannibals_savage

cannibals_fork_named: common.c core_shm.c sync_named.c fork_main.c $(HDR)
	$(CC) $(CFLAGS) -o $@ common.c core_shm.c sync_named.c fork_main.c $(LIBS)

cannibals_fork_unnamed: common.c core_shm.c sync_unnamed.c fork_main.c $(HDR)
	$(CC) $(CFLAGS) -o $@ common.c core_shm.c sync_unnamed.c fork_main.c $(LIBS)

cannibals_host: common.c core_shm.c sync_named.c ind_host.c $(HDR)
	$(CC) $(CFLAGS) -o $@ common.c core_shm.c sync_named.c ind_host.c $(LIBS)

cannibals_cook: common.c core_shm.c sync_named.c ind_cook.c $(HDR)
	$(CC) $(CFLAGS) -o $@ common.c core_shm.c sync_named.c ind_cook.c $(LIBS)

cannibals_savage: common.c core_shm.c sync_named.c ind_savage.c $(HDR)
	$(CC) $(CFLAGS) -o $@ common.c core_shm.c sync_named.c ind_savage.c $(LIBS)

cannibals_mq_host: common.c mq_host.c mq_proto.h common.h
	$(CC) $(CFLAGS) -o $@ common.c mq_host.c $(LIBS)

cannibals_mq_cook: common.c mq_cook.c mq_proto.h common.h
	$(CC) $(CFLAGS) -o $@ common.c mq_cook.c $(LIBS)

cannibals_mq_savage: common.c mq_savage.c mq_proto.h common.h
	$(CC) $(CFLAGS) -o $@ common.c mq_savage.c $(LIBS)

clean:
	rm -f cannibals_* *.log
