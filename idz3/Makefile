CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS = -pthread -lrt
BIN = tanks_unnamed tanks_named tanks_independent tanks_queue tanks_worker

all: $(BIN)

$(BIN): tanks.c ipc.c ipc.h
	$(CC) $(CFLAGS) tanks.c ipc.c -o $@ $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean

test: all
	bash tests/run_tests.sh

.PHONY: test
