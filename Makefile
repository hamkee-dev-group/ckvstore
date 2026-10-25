CC ?= gcc
AR ?= ar
RANLIB ?= ranlib

CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -fno-common
CPPFLAGS += -Iinclude -Isrc -D_FILE_OFFSET_BITS=64

LIB = libckvstore.a
LIB_OBJS = \
    src/ckv_crc32.o \
    src/ckv_io.o \
    src/ckv_log.o \
    src/ckv_index.o \
    src/ckv_store.o \
    src/ckvstore_version.o

CLI = ckvcli
CLI_OBJS = cli/ckvcli.o

TESTBIN = ckvtests
TESTS_OBJS = tests/tests.o
FAULTBIN = ckvfaults

.PHONY: all clean test

all: $(LIB) $(CLI) $(TESTBIN)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(CLI): $(CLI_OBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJS) $(LIB)

$(TESTBIN): $(TESTS_OBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(TESTS_OBJS) $(LIB)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(FAULTBIN): tests/faults.c src/ckv_store.c src/ckv_index.c src/ckv_log.c src/ckv_io.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/faults.c $(LIB)

test: $(TESTBIN) $(FAULTBIN)
	./$(TESTBIN)
	./$(FAULTBIN)

clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(TESTS_OBJS) $(LIB) $(CLI) $(TESTBIN) $(FAULTBIN)
