all:
	$(CC) -Wall -g -o rcu_queue rcu_queue.c -lpthread

indent:
	clang-format -i rcu_queue.c

clean:
	rm -f rcu_queue
