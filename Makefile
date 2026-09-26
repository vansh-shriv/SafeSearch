# Target firmware build. Toolchain is not on PATH by default; override if installed elsewhere.
#   mingw32-make            build bootloader + signed app images
#   mingw32-make clean      (keeps keys/)
TC_BIN ?= C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/12.2 mpacbti-rel1/bin
CC      = "$(TC_BIN)/arm-none-eabi-gcc"
OBJCOPY = "$(TC_BIN)/arm-none-eabi-objcopy"
SIZE    = "$(TC_BIN)/arm-none-eabi-size"
PY     ?= python

BL_SRC  = bootloader/src
UECC    = crypto/micro-ecc
BUILD   = build

CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g -std=c99 -Wall -Wextra -Werror \
          -ffreestanding -ffunction-sections -fdata-sections \
          -I$(BL_SRC) -Icrypto -I$(UECC)
# micro-ecc: P-256 only, no compressed points
UECC_FLAGS = -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=0 \
             -DuECC_SUPPORTS_secp256r1=1 -DuECC_SUPPORTS_secp256k1=0 -DuECC_SUPPORT_COMPRESSED_POINT=0
LDFLAGS = -nostartfiles -nostdlib -Wl,--gc-sections

BL_OBJS = $(addprefix $(BUILD)/bl/, startup.o main.o crc32.o image_verify.o image_crypto.o libc_min.o \
          metadata.o flash_stm32.o sha256.o uECC.o pubkey.o)

.PHONY: all clean
all: $(BUILD)/bootloader.elf $(BUILD)/app_v1_slotA.img $(BUILD)/app_v2_slotB.img
	$(SIZE) $(BUILD)/bootloader.elf $(BUILD)/app_v1_slotA.elf

$(BUILD)/bl:
	mkdir -p $@

$(BUILD)/bl/%.o: $(BL_SRC)/%.c | $(BUILD)/bl
	$(CC) $(CFLAGS) $(UECC_FLAGS) -c $< -o $@

$(BUILD)/bl/sha256.o: crypto/sha256.c | $(BUILD)/bl
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored code is compiled unmodified; silence its warnings rather than editing it.
$(BUILD)/bl/uECC.o: $(UECC)/uECC.c | $(BUILD)/bl
	$(CC) $(CFLAGS) $(UECC_FLAGS) -w -c $< -o $@

$(BUILD)/bl/pubkey.o: $(BUILD)/pubkey.c | $(BUILD)/bl
	$(CC) $(CFLAGS) -c $< -o $@

keys/private.pem:
	$(PY) tools/keytool.py gen

$(BUILD)/pubkey.c: keys/private.pem tools/keytool.py
	mkdir -p $(BUILD)
	$(PY) tools/keytool.py header $@

$(BUILD)/bootloader.elf: $(BL_OBJS) bootloader/linker/bootloader.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -Tbootloader/linker/bootloader.ld $(BL_OBJS) -o $@

# app_v<N>_slot<X>: version N linked for slot X
$(BUILD)/app_v1_slotA.elf: app/src/startup.c app/src/main.c app/linker/app_slot_a.ld
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DAPP_VERSION=1 $(LDFLAGS) -Tapp/linker/app_slot_a.ld app/src/startup.c app/src/main.c -o $@

$(BUILD)/app_v2_slotB.elf: app/src/startup.c app/src/main.c app/linker/app_slot_b.ld
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DAPP_VERSION=2 $(LDFLAGS) -Tapp/linker/app_slot_b.ld app/src/startup.c app/src/main.c -o $@

$(BUILD)/app_v1_slotA.img: $(BUILD)/app_v1_slotA.elf tools/sign_image.py keys/private.pem
	$(OBJCOPY) -O binary $< $(BUILD)/app_v1_slotA.bin
	$(PY) tools/sign_image.py $(BUILD)/app_v1_slotA.bin $@ --version 1

$(BUILD)/app_v2_slotB.img: $(BUILD)/app_v2_slotB.elf tools/sign_image.py keys/private.pem
	$(OBJCOPY) -O binary $< $(BUILD)/app_v2_slotB.bin
	$(PY) tools/sign_image.py $(BUILD)/app_v2_slotB.bin $@ --version 2

clean:
	rm -rf $(BUILD)
