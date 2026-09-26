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

BL_OBJS = $(addprefix $(BUILD)/bl/, startup.o main.o boot_logic.o crc32.o image_verify.o image_crypto.o libc_min.o \
          metadata.o flash_stm32.o sha256.o uECC.o pubkey.o)

.PHONY: all clean
all: $(BUILD)/bootloader.elf $(BUILD)/app_v1_slotA.img $(BUILD)/app_v2_slotB.img $(BUILD)/app_v3_slotB_bad.img
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

# Apps link the SafeFlash app library (confirm_healthy / watchdog gating), which needs the metadata + flash driver.
APP_SRCS = app/src/startup.c app/src/main.c app/src/safeflash_app.c app/src/safeflash_update.c $(BL_SRC)/metadata.c $(BL_SRC)/crc32.c $(BL_SRC)/flash_stm32.c $(BL_SRC)/libc_min.c

# APP_RULE(name, version, linker script, extra flags): builds build/<name>.elf and the signed build/<name>.img
define APP_RULE
$(BUILD)/$(1).elf: $(APP_SRCS) app/linker/$(3).ld $(wildcard app/src/*.h $(BL_SRC)/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DAPP_VERSION=$(2) $(4) $(LDFLAGS) -Tapp/linker/$(3).ld $(APP_SRCS) -o $$@

$(BUILD)/$(1).img: $(BUILD)/$(1).elf tools/sign_image.py keys/private.pem
	$(OBJCOPY) -O binary $$< $(BUILD)/$(1).bin
	$(PY) tools/sign_image.py $(BUILD)/$(1).bin $$@ --version $(2)
endef

$(eval $(call APP_RULE,app_v1_slotA,1,app_slot_a,))
$(eval $(call APP_RULE,app_v2_slotB,2,app_slot_b,))
# v3 "bad" image for slot B: runs but never calls sf_confirm_healthy(), so the trial must expire and revert
$(eval $(call APP_RULE,app_v3_slotB_bad,3,app_slot_b,-DAPP_CONFIRM=0))

clean:
	rm -rf $(BUILD)
