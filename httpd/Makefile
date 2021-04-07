httpd: httpd.c
	$(CC) -Wall -Wextra -o httpd httpd.c -lpthread

clean:
	rm -f httpd

indent:
	clang-format -i httpd.c
