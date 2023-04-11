all:
	$(CC) -Wall -O2 -std=c11 -o mpmc mpmc.c -lpthread

indent:
	clang-format -i mpmc.c

clean:
	rm -f mpmc
