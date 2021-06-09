CFLAGS = -O2 -Wall -std=gnu99

TARGET = task_sched
all: $(TARGET)
%: %.c
	$(CC) $(CFLAGS) -o $@ $^

task_sched.c: list.h

list.h:
	wget -q https://raw.githubusercontent.com/sysprog21/linux-list/master/include/list.h
	touch $@

indent:
	clang-format -i task_sched.c

check: all
	./task_sched

clean:
	$(RM) $(TARGET) *~

distclean: clean
	$(RM) list.h
