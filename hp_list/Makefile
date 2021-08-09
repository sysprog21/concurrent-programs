CC ?= gcc
CFLAGS = -Wall -g
CFLAGS += -fsanitize=thread
LIBS = -lpthread
BIN = list

all: list

$(BIN): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

all: CFLAGS += -O2
all: $(BIN)

indent:
	clang-format -i *.[ch]

clean:
	rm -f $(BIN)
