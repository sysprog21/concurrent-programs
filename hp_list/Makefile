all:
	$(CC) -Wall -o list main.c -lpthread -g -fsanitize=thread

indent:
	clang-format -i *.[ch]

clean:
	rm -f list
