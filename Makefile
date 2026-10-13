PLATFORM ?= qemu
CROSS ?= xenon-
CC := $(CROSS)gcc
LD := $(CROSS)ld

BUILD ?= build/$(PLATFORM)
ifeq ($(PLATFORM),xenon)
BASE := 0x100000
else ifeq ($(PLATFORM),mac)
BASE := 0x10000000
else
BASE := 0x20010000
endif
CFLAGS := -m64 -mcpu=cell -mno-altivec -mno-vsx -msoft-float -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-delete-null-pointer-checks -fno-asynchronous-unwind-tables \
	-O2 -g -Wall -Wextra -Wno-unused-parameter -Iinclude $(EXTRA)
ASFLAGS := -m64 -mcpu=cell -Iinclude -D__ASSEMBLER__

SRC := $(wildcard arch/ppc64/*.S) $(wildcard kernel/*.S) $(wildcard kernel/*.c) $(wildcard kernel/usb/*.c) $(wildcard kernel/shell/*.c) \
	$(wildcard platform/$(PLATFORM)/*.c) $(wildcard platform/$(PLATFORM)/*.S)
OBJ := $(patsubst %,$(BUILD)/%.o,$(basename $(SRC)))

# the kernel carries its own source, src in the shell writes it out
TREE := README.md LICENSE LICENSE.fonts Makefile kernel.lds test.sh .gitignore .gitattributes arch include kernel \
	platform tools
TREE_FILES := $(shell find $(wildcard $(TREE)) -type f)

all: $(BUILD)/kernel.elf $(if $(filter xenon,$(PLATFORM)),$(BUILD)/xenon.elf)

$(BUILD)/kernel.elf: $(OBJ) kernel.lds
	$(LD) -T kernel.lds --defsym=KERNEL_BASE=$(BASE) -z max-page-size=0x10000 -o $@ $(OBJ)

# xell only takes elf32, the code inside stays 64 bit
$(BUILD)/xenon.elf: $(BUILD)/kernel.elf
	$(CROSS)objcopy -O elf32-powerpc $< $@

$(BUILD)/%.o: %.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/source.tar: $(TREE_FILES)
	@mkdir -p $(dir $@)
	tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name -cf $@ $(wildcard $(TREE))

$(BUILD)/kernel/source.o: $(BUILD)/source.tar
$(BUILD)/kernel/source.o: ASFLAGS += -DSOURCE_TAR='"$(BUILD)/source.tar"'

$(BUILD)/%.o: %.S $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf build

.PHONY: all clean
