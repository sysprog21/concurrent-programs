CFLAGS = -Wall -Wextra
LDFLAGS = -lpthread

BINS = bench-lock bench-lockfree

all: $(BINS)

bench-lock: bench.c lock.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bench-lockfree: bench.c lockfree.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BINS)

check: $(BINS)
	./bench-lock
	@echo
	./bench-lockfree

indent:
	clang-format -i *.[ch]
