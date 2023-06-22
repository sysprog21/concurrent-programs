CFLAGS = -std=gnu11 -O3 -Wall -Wextra
LDFLAGS = -lpthread

targets = main1 main2 main3 main4

all: $(targets)

tsan: CFLAGS += -fsanitize=thread -g
tsan: all

clean:
	rm -f $(targets)

main%: lfq.c test.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

main1: CFLAGS += -D MAX_PRODUCER=1 -D MAX_CONSUMER=1
main2: CFLAGS += -D MAX_PRODUCER=4 -D MAX_CONSUMER=4
main3: CFLAGS += -D MAX_PRODUCER=100 -D MAX_CONSUMER=10
main4: CFLAGS += -D MAX_PRODUCER=10 -D MAX_CONSUMER=100
