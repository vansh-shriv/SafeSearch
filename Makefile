# Target firmware build. Toolchain is not on PATH by default; override if installed elsewhere.
#   mingw32-make            build bootloader + app images
#   mingw32-make clean
TC_BIN ?= C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/12.2 mpacbti-rel1/bin
CC      = "$(TC_BIN)/arm-none-eabi-gcc"
OBJCOPY = "$(TC_BIN)/arm-none-eabi-objcopy"
SIZE    = "$(TC_BIN)/arm-none-eabi-size"
PY     ?= python

BL_SRC  = bootloader/src
CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g -std=c99 -Wall -Wextra -Werror \
          -ffreestanding -ffunction-sections -fdata-sections -I$(BL_SRC)
LDFLAGS = -nostartfiles -nostdlib -Wl,--gc-sections

BUILD = build
BL_OBJS = $(BL_SRC)/startup.c $(BL_SRC)/main.c $(BL_SRC)/crc32.c $(BL_SRC)/image_verify.c \
          $(BL_SRC)/libc_min.c $(BL_SRC)/metadata.c $(BL_SRC)/flash_stm32.c

.PHONY: all clean
all: $(BUILD)/bootloader.elf $(BUILD)/app_v1_slotA.img $(BUILD)/app_v2_slotB.img
	$(SIZE) $(BUILD)/bootloader.elf $(BUILD)/app_v1_slotA.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bootloader.elf: $(BL_OBJS) bootloader/linker/bootloader.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -Tbootloader/linker/bootloader.ld $(BL_OBJS) -o $@

# app_v<N>_slot<X>: version N linked for slot X
$(BUILD)/app_v1_slotA.elf: app/src/startup.c app/src/main.c app/linker/app_slot_a.ld | $(BUILD)
	$(CC) $(CFLAGS) -DAPP_VERSION=1 $(LDFLAGS) -Tapp/linker/app_slot_a.ld app/src/startup.c app/src/main.c -o $@

$(BUILD)/app_v2_slotB.elf: app/src/startup.c app/src/main.c app/linker/app_slot_b.ld | $(BUILD)
	$(CC) $(CFLAGS) -DAPP_VERSION=2 $(LDFLAGS) -Tapp/linker/app_slot_b.ld app/src/startup.c app/src/main.c -o $@

$(BUILD)/app_v1_slotA.img: $(BUILD)/app_v1_slotA.elf tools/pack_image.py
	$(OBJCOPY) -O binary $< $(BUILD)/app_v1_slotA.bin
	$(PY) tools/pack_image.py $(BUILD)/app_v1_slotA.bin $@ --version 1

$(BUILD)/app_v2_slotB.img: $(BUILD)/app_v2_slotB.elf tools/pack_image.py
	$(OBJCOPY) -O binary $< $(BUILD)/app_v2_slotB.bin
	$(PY) tools/pack_image.py $(BUILD)/app_v2_slotB.bin $@ --version 2

clean:
	rm -rf $(BUILD)
