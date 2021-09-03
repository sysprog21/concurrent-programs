.PHONY: all clean
TARGET = test-hashmap
all: $(TARGET)

include common.mk

CFLAGS = -I.
CFLAGS += -O2 -g
CFLAGS += -std=gnu11 -Wall

LDFLAGS = -lpthread

# standard build rules
.SUFFIXES: .o .c
.c.o:
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

OBJS = \
	free_later.o \
	hashmap.o \
	test-hashmap.o

deps += $(OBJS:%.o=%.o.d)

$(TARGET): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

check: $(TARGET)
	$(Q)./$^ && $(call pass)

clean:
	$(VECHO) "  Cleaning...\n"
	$(Q)$(RM) $(TARGET) $(OBJS) $(deps)

-include $(deps)
