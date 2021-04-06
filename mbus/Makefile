all:
	$(CC) -Wall -Wextra -o mbus mbus.c -lpthread -latomic \
		-Og -g3 -fsanitize=thread

clean:
	rm -f mbus

indent:
	clang-format -i mbus.c
