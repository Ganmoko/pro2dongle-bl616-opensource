CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

SOURCES := test_protocol.c \
	../src/pro2_protocol.c \
	../src/pro2_rumble.c \
	../src/pro2_settings.c \
	../src/vendor_protocol.c

.PHONY: all test clean
all: test

test: test_protocol
	./test_protocol

test_protocol: $(SOURCES)
	$(CC) $(CFLAGS) -DPRO2_HOST_TEST -I../src $(SOURCES) -o $@

clean:
	$(RM) test_protocol
