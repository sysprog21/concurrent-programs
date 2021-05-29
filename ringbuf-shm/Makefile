all:
	$(CC) -Wall -std=gnu11 -o ringbuffer ringbuffer.c -lpthread -lrt

clean:
	rm -f ringbuffer

indent:
	clang-format -i ringbuffer.c
