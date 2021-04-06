picosh: picosh.o
	$(CC) $^ -Wall -Wextra -std=c99 -o $@

clean:
	rm -f picosh.o picosh

indent:
	clang-format -i picosh.c

.PHONY: clean
