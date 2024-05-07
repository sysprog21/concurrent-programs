CFLAGS := -I.. -Wall -O2 -D_GNU_SOURCE
LDFLAGS := -lpthread

ALL := test_pthread test_linux

all: $(ALL)
.PHONY: all

test_%: main.c
	$(CC) $(CFLAGS) main.c -o $@ $(LDFLAGS)

test_pthread: CFLAGS += -DUSE_PTHREADS
test_pthread: test_pthread
test_linux: CFLAGS += -DUSE_LINUX
test_linux: test_linux

clean:
	$(RM) test_pthread
	$(RM) test_linux
.PHONY: clean
