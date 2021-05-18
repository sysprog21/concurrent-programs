all:
	$(CC) -Wall -o rcu_list rcu_list.c -lpthread -g -fsanitize=thread

indent:
	clang-format -i rcu_list.c

clean:
	rm -f rcu_list
