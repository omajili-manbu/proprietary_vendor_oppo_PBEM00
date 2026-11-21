# qsee_selftest - on-device probe ladder for the 7.3 qseecom stack
#
# Targets:
#   make            - build the selftest binary (cross aarch64 by default)
#   make check      - syntax-only validation with the host compiler
#   make clean
#
# The binary dlopen()s libQSEEComAPI.so at runtime for stages S3-S5, so
# stages S0-S2 (dma-heap / driver plumbing) run before the library is
# deployed.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -I. -I../qsee_lib/include -I../qsee_lib/src
LDFLAGS := -ldl

SRCS := qsee_selftest.c
OBJS := $(SRCS:.c=.o)

TARGET := qsee_selftest

.PHONY: all check clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

check:
	@for f in $(SRCS); do \
		$(CC) -std=gnu11 -fsyntax-only -Wall -I. \
		      -I../qsee_lib/include -I../qsee_lib/src $$f || exit 1; \
	done
	@echo "syntax OK"

clean:
	rm -f $(OBJS) $(TARGET)
