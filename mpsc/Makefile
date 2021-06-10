all:
	$(CC) -Wall -std=c11 -o mpsc mpsc.c -lpthread

indent:
	clang-format -i mpsc.c

clean:
	rm -f mpsc
