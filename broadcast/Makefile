all:
	gcc -Wall -O2 -D_GNU_SOURCE \
		-o stress \
		stress.c \
		broadcast.c \
		pool.c \
		-lpthread

clean:
	rm -f stress
