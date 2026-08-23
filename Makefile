CC ?= cc
CFLAGS ?= -O2
WARNINGS = -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test sanitize thread-sanitize clean

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

clean:
	rm -f test_pool_allocator test_pool_allocator_san test_pool_allocator_tsan *.o
