CC ?= gcc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS ?=

CPPFLAGS += -Iinclude
CFLAGS += -Wall -Wextra -Werror -std=c11

LIB := libconzone_base.a
OBJS := src/conzone_base.o
EXAMPLES := examples/conzone_demo examples/cz_write_bw

.PHONY: all clean examples

all: $(LIB) examples

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

src/%.o: src/%.c include/conzone_base.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

examples: $(EXAMPLES)

examples/conzone_demo: examples/conzone_demo.c $(LIB) include/conzone_base.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) -o $@

examples/cz_write_bw: examples/cz_write_bw.c $(LIB) include/conzone_base.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) -o $@

clean:
	rm -f $(OBJS) $(LIB) $(EXAMPLES)
