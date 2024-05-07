CFLAGS := -I.. -std=c11 -Wall -g -O2 -D_GNU_SOURCE -fsanitize=thread
LDFLAGS := -lpthread

ALL := test_pthread test_linux

all: $(ALL)
.PHONY: all

test_%: main.c
	$(CC) $(CFLAGS) main.c -o $@ $(LDFLAGS)

test_pthread: CFLAGS += -DUSE_PTHREADS
test_linux: CFLAGS += -DUSE_LINUX

# Test suite
NAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    PRINTF = printf
else
    PRINTF = env printf
endif
PASS_COLOR = \e[32;01m
NO_COLOR = \e[0m
notice = $(PRINTF) "$(PASS_COLOR)$(strip $1)$(NO_COLOR)\n"

check: $(ALL)
	@$(foreach t,$^,\
	    $(PRINTF) "Running $(t) ... "; \
	    ./$(t) && $(call notice, [OK]); \
	)

clean:
	$(RM) $(ALL)
.PHONY: clean

-include $(OUT)/*.d
