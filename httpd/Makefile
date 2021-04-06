httpd: httpd.c
	gcc -Wall -Wextra -o httpd httpd.c -lpthread

clean:
	rm -f httpd

indent:
	clang-format -i httpd.c
