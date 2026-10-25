# Сборка и проверка tanks_sem (ИДЗ-2, вариант 24).
#   make            — tanks_sem (-O2, все предупреждения)
#   make test       — полный набор tests/battle_tests.sh
#   make tsan       — сборка с ThreadSanitizer и сокращённый набор тестов на ней
#   make asan       — сборка с AddressSanitizer + UBSan (+ LeakSanitizer) и тот же набор
#   make clean      — удалить бинарники и результаты тестов
CC       = gcc
STD      = -std=c11
WARN     = -Wall -Wextra -Wpedantic
CFLAGS   = -O2
SANFLAGS = -O1 -g -fno-omit-frame-pointer
LDLIBS   = -pthread
TESTS    = bash tests/battle_tests.sh

PROG = tanks_sem
SRC  = tanks_sem.c

COND     = tanks_cond
COND_SRC = tanks_cond.c

all: $(PROG) $(COND)

$(PROG): $(SRC)
	$(CC) $(STD) $(WARN) $(CFLAGS) -pthread $< -o $@ $(LDLIBS)

$(PROG)_tsan: $(SRC)
	$(CC) $(STD) $(WARN) $(SANFLAGS) -fsanitize=thread -pthread $< -o $@ $(LDLIBS)

$(PROG)_asan: $(SRC)
	$(CC) $(STD) $(WARN) $(SANFLAGS) -fsanitize=address,undefined -pthread $< -o $@ $(LDLIBS)

$(COND): $(COND_SRC)
	$(CC) $(STD) $(WARN) $(CFLAGS) -pthread $< -o $@ $(LDLIBS)

$(COND)_tsan: $(COND_SRC)
	$(CC) $(STD) $(WARN) $(SANFLAGS) -fsanitize=thread -pthread $< -o $@ $(LDLIBS)

$(COND)_asan: $(COND_SRC)
	$(CC) $(STD) $(WARN) $(SANFLAGS) -fsanitize=address,undefined -pthread $< -o $@ $(LDLIBS)

test: $(PROG) $(COND)
	$(TESTS) ./$(PROG)
	$(TESTS) ./$(COND)

tsan: $(PROG)_tsan $(COND)_tsan
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" $(TESTS) --quick ./$(PROG)_tsan
	TSAN_OPTIONS="halt_on_error=1 exitcode=66" $(TESTS) --quick ./$(COND)_tsan

asan: $(PROG)_asan $(COND)_asan
	ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
		$(TESTS) --quick ./$(PROG)_asan
	ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
		$(TESTS) --quick ./$(COND)_asan

clean:
	rm -f $(PROG) $(PROG)_tsan $(PROG)_asan $(COND) $(COND)_tsan $(COND)_asan
	rm -rf tests/out

.PHONY: all test tsan asan clean
