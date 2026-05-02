# Reference Makefile for a Rust firmware against rvvm-hal.
#
# The cargo invocation links against ../../libhal.a (built by HAL's
# top-level Makefile) using HAL's link.ld. We pass `-nostartfiles`
# because start.S in libhal.a already provides _start.

HAL      := ../..
OBJCOPY  ?= llvm-objcopy
CARGO    ?= cargo
CARGO_PROFILE ?= release
TARGET   := riscv64gc-unknown-none-elf
ELF      := target/$(TARGET)/$(CARGO_PROFILE)/firmware

CARGO_FLAGS :=
ifeq ($(CARGO_PROFILE),release)
CARGO_FLAGS += --release
endif

all: firmware.bin

.PHONY: $(HAL)/libhal.a
$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

# Cargo writes the ELF; we depend on libhal.a being current first
# so the linker step finds it. Cargo's own dep-tracking covers
# everything under src/.
$(ELF): $(HAL)/libhal.a
	$(CARGO) build $(CARGO_FLAGS)

firmware.elf: $(ELF)
	cp $< $@

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	rvvm firmware.bin -bochs_display -nonet -nosound

run-headless: firmware.bin
	rvvm firmware.bin -res 800x480 -nogui -nonet -nosound

run-qemu: firmware.elf
	qemu-system-riscv64 -M virt -nographic \
	    -cpu rv64,zba=true,zbb=true,zbs=true,zicond=true,zicboz=true \
	    -bios none -kernel firmware.elf

clean:
	$(CARGO) clean
	rm -f firmware.elf firmware.bin

.PHONY: all run run-qemu clean
