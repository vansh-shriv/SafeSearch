# Host tests. MinGW is not on PATH by default.
#   mingw32-make -C tests/unit test    unit tests (logic against a mock flash)
#   mingw32-make -C tests/unit sweep   exhaustive power-cut sweep over update/confirm/boot (needs `make` at repo root first)
CC = gcc
SRC = ../../bootloader/src
APP = ../../app/src
CRYPTO = ../../crypto
UECC = $(CRYPTO)/micro-ecc
BUILD = ../../build

CFLAGS = -std=c99 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror -O2 -g -I$(SRC) -I$(APP) -I$(CRYPTO) -I$(UECC) -I.
UECC_FLAGS = -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=0 \
             -DuECC_SUPPORTS_secp256r1=1 -DuECC_SUPPORTS_secp256k1=0 -DuECC_SUPPORT_COMPRESSED_POINT=0

# uECC's host RNG (unused for verify, still linked) needs advapi32 on Windows only.
ifeq ($(OS),Windows_NT)
LDLIBS = -ladvapi32
endif

UNIT_SRCS =test_main.c mock_flash.c $(SRC)/crc32.c $(SRC)/metadata.c $(SRC)/image_verify.c $(CRYPTO)/sha256.c

SWEEP_SRCS = fault_sweep.c mock_flash.c $(SRC)/crc32.c $(SRC)/metadata.c $(SRC)/image_verify.c \
             $(SRC)/image_crypto.c $(SRC)/boot_logic.c $(CRYPTO)/sha256.c \
             $(APP)/safeflash_app.c $(APP)/safeflash_update.c

.PHONY: test sweep clean
test: test_main.exe
	./test_main.exe

sweep: fault_sweep.exe
	./fault_sweep.exe

test_main.exe: $(UNIT_SRCS) $(wildcard *.h) $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) $(UNIT_SRCS) -o $@

# Vendored micro-ecc compiled unmodified with its warnings silenced; host RNG (unused for verify) needs advapi32.
uecc_host.o: $(UECC)/uECC.c
	$(CC) -std=gnu99 -O2 -w $(UECC_FLAGS) -I$(UECC) -c $< -o $@

fault_sweep.exe: $(SWEEP_SRCS) uecc_host.o $(BUILD)/pubkey.c $(wildcard *.h) $(wildcard $(SRC)/*.h) $(wildcard $(APP)/*.h)
	$(CC) $(CFLAGS) $(UECC_FLAGS) $(SWEEP_SRCS) $(BUILD)/pubkey.c uecc_host.o $(LDLIBS) -o $@

clean:
	rm -f test_main.exe fault_sweep.exe uecc_host.o
