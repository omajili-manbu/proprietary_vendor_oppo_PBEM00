# Rebuilt libQSEEComAPI.so
#
# Targets:
#   make            - build objects + shared library with the cross toolchain
#   make check      - syntax-only validation with the host compiler
#   make clean
#
# The library targets aarch64 (Android/bionic or any glibc rootfs); it only
# needs libc + the kernel UAPI constants mirrored in qsee_internal.h, so it
# compiles standalone without Android build scaffolding.

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -fPIC \
           -Iinclude -Isrc
LDFLAGS := -shared -Wl,-soname,libQSEEComAPI.so -lpthread

SRCS := src/qsee_com_api.c src/qsee_dma_heap.c src/qsee_image.c
OBJS := $(SRCS:.c=.o)

TARGET := libQSEEComAPI.so

.PHONY: all check clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Syntax validation on any Linux host without a cross toolchain:
check:
	@for f in $(SRCS); do \
		$(CC) -std=gnu11 -fsyntax-only -Wall -Iinclude -Isrc $$f || exit 1; \
	done
	@echo "syntax OK"

clean:
	rm -f $(OBJS) $(TARGET)
