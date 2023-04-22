CFLAGS = -Wall
CFLAGS += -g
CFLAGS += -std=gnu11
CFLAGS += -D'N_READERS=100'
CFLAGS += -D'N_UPDATE_RUN=5'
CFLAGS += -fsanitize=thread
LDFLAGS += -lpthread

# The pthread mutex initializer will warning:
# thrd_rcu.h:95:42: warning: Using plain integer as NULL pointer
# We can ignore it.
SPARSE_FLAGS = -Wno-non-pointer-null

main: main.c rcu.h
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

clang: CC=clang
clang: main

all: main

# Semantic Checker
# https://www.kernel.org/doc/html/latest/dev-tools/sparse.html
sparse:
	sparse main.c $(CFLAGS) $(SPARSE_FLAGS)

indent:
	clang-format -i *.[ch]

clean:
	rm -f main
