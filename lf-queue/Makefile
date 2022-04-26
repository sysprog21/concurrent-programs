CFLAGS =  -Wall -O2 -g -I.
LDFLAGS = -lpthread

# Enable ThreadSanitizer
# CFLAGS += -fsanitize=thread
# LDFLAGS += -fsanitize=thread

all: test

test: test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f test
