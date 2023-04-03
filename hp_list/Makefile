CC ?= gcc
CFLAGS = -Wall -g
CFLAGS += -fsanitize=thread
LIBS = -lpthread
BIN = list

all: list list2

$(BIN): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

list2: list2.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

all: CFLAGS += -O2
all: $(BIN)

# Once RUNTIME_STAT is defined, the program will show runtime states in
# statistics.
analyze: CFLAGS +=-D RUNTIME_STAT
analyze: $(BIN)

indent:
	clang-format -i *.[ch]

clean:
	rm -f $(BIN) list2
