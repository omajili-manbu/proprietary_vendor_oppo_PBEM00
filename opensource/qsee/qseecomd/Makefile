# Rebuilt qseecomd (skeleton)
#
# Targets:
#   make            - build qseecomd (cross aarch64 by default)
#   make check      - syntax-only validation with the host compiler
#   make clean
#
# On-device builds (Android) should link against libcutils for
# property_set and drop the host fallback in service_stubs.c.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -I. -I../qsee_lib/include -I../qsee_lib/src
LDFLAGS := -ldl -lpthread

SRCS := qseecomd.c service_stubs.c
OBJS := $(SRCS:.c=.o)

TARGET := qseecomd

.PHONY: all check clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) \
	      -L../qsee_lib -lQSEEComAPI

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
