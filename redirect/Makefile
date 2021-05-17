all:
	gcc -Wall -Wextra -o redirect main.c

indent:
	clang-format -i main.c
	cloc main.c

check: all
	(cat /proc/uptime ; sleep 2; cat /proc/uptime; sleep 2; cat /proc/uptime; sleep 2; cat /proc/uptime) | ./redirect
clean:
	rm -f redirect
