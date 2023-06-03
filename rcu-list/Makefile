CC ?= gcc
CFLAGS = -g
CFLAGS += -Wall
LDFLAGS += -lpthread
CFLAGS += -std=c99
CFLAGS += -fsanitize=thread

READER_NUM = 10
UPDATER_NUM = 1
TRACE_LOOP = 1000

CFLAGS += -D'READER_NUM=$(READER_NUM)'
CFLAGS += -D'UPDATER_NUM=$(UPDATER_NUM)'
CFLAGS += -D'TRACE_LOOP=$(TRACE_LOOP)'
CFLAGS += -D'CONFIG_TRACE_TIME'

all:
	$(CC) -o test test.c $(CFLAGS) $(LDFLAGS)

clean:
	rm -f test
