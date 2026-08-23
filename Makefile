CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test sanitize thread-sanitize debug-test no-iovec-test no-tls-test strict-test lock-hook-test verify clean

all: test_pool_allocator

test_pool_allocator: pool_allocator.c pool_allocator.h test_pool_allocator.c
	$(CC) $(CFLAGS) $(WARNINGS) -pthread pool_allocator.c test_pool_allocator.c -o $@

test: test_pool_allocator
	./test_pool_allocator

sanitize:
	$(CC) -O1 -g $(WARNINGS) -pthread -fsanitize=address,undefined \
		pool_allocator.c test_pool_allocator.c -o test_pool_allocator_san
	./test_pool_allocator_san

thread-sanitize:
	$(CC) -O1 -g $(WARNINGS) -pthread -fsanitize=thread \
		pool_allocator.c test_pool_allocator.c -o test_pool_allocator_tsan
	./test_pool_allocator_tsan

debug-test:
	$(CC) -O1 -g $(WARNINGS) -pthread -DPA_DEBUG=1 \
		pool_allocator.c test_pool_allocator.c -o test_pool_allocator_debug
	./test_pool_allocator_debug

no-iovec-test:
	$(CC) $(CFLAGS) $(WARNINGS) -pthread -DPA_HAVE_IOVEC=0 \
		pool_allocator.c test_pool_allocator.c -o test_pool_allocator_no_iovec
	./test_pool_allocator_no_iovec

no-tls-test:
	$(CC) $(CFLAGS) $(WARNINGS) -DPA_NO_TLS=1 -DPA_HAVE_IOVEC=0 \
		pool_allocator.c test_explicit_context.c -o test_explicit_context
	./test_explicit_context

strict-test:
	$(CC) $(CFLAGS) $(WARNINGS) -Wconversion -Wsign-conversion -Wshadow \
		-Wstrict-prototypes -Wcast-align -Wpointer-arith -Wundef -pthread \
		pool_allocator.c test_pool_allocator.c -o test_pool_allocator_strict
	./test_pool_allocator_strict

lock-hook-test:
	$(CC) $(CFLAGS) $(WARNINGS) -pthread -include test_lock_hooks.h \
		pool_allocator.c test_pool_allocator.c -o test_pool_allocator_lock_hooks
	./test_pool_allocator_lock_hooks

verify: test sanitize thread-sanitize debug-test no-iovec-test no-tls-test strict-test lock-hook-test

clean:
	rm -f test_pool_allocator test_pool_allocator_san test_pool_allocator_tsan \
		test_pool_allocator_debug test_pool_allocator_no_iovec \
		test_pool_allocator_strict test_pool_allocator_lock_hooks \
		test_explicit_context *.o
