CFLAGS = -Wall -O2

ifeq ("$(TSAN)", "1")
    CFLAGS += -g -fsanitize=thread
endif

all:
	gcc $(CFLAGS) -o test-cmap \
		cmap.c  random.c  rcu.c  test-cmap.c \
		-lpthread

clean:
	rm -f test-cmap
