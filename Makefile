# rvvm-hal — bare-metal RISC-V drivers for RVVM's emulated devices.
#
# Builds libhal.a — a static archive linkable into any bare-metal
# RVVM firmware. Headers live in include/, sources in src/. Consumers
# pass -Iinclude to find the headers and link libhal.a.

TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
AR       := llvm-ar

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -Iinclude

SRCS     := $(wildcard src/*.c) $(wildcard src/*.S)
OBJS     := $(patsubst src/%.c,build/%.o,$(filter %.c,$(SRCS))) \
            $(patsubst src/%.S,build/%.o,$(filter %.S,$(SRCS)))

all: libhal.a

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

libhal.a: $(OBJS)
	$(AR) rcs $@ $^
	@printf '\nBuilt %s (%s bytes, %d objects)\n' \
		"$@" "$$(stat -c %s $@)" "$$(echo $^ | wc -w)"

clean:
	rm -rf build libhal.a

.PHONY: all clean
