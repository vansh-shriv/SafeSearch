# Fuzzing. Run from tests/fuzz; needs `make` at the repo root first (loads build/app_v*.img and build/pubkey.c).
#   mingw32-make -C tests/fuzz run                 portable mutation driver, all targets (works anywhere)
#   mingw32-make -C tests/fuzz run ITERS=200000    more iterations
#   make -C tests/fuzz libfuzzer CC=clang          libFuzzer + ASan + UBSan binaries (Linux/clang)
# make predefines CC=cc, which MinGW lacks; only keep it when the user (or CI) set CC explicitly
ifeq ($(origin CC),default)
CC = gcc
endif
ITERS ?= 20000
SRC = ../../bootloader/src
APP = ../../app/src
CRYPTO = ../../crypto
UECC = $(CRYPTO)/micro-ecc
BUILD = ../../build
UNIT = ../unit

UECC_FLAGS = -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=0 \
             -DuECC_SUPPORTS_secp256r1=1 -DuECC_SUPPORTS_secp256k1=0 -DuECC_SUPPORT_COMPRESSED_POINT=0
CFLAGS = -std=c99 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Werror -O1 -g -I$(SRC) -I$(APP) -I$(CRYPTO) -I$(UECC) -I$(UNIT) -I.
ifeq ($(OS),Windows_NT)
LDLIBS = -ladvapi32
endif

COMMON = fuzz_targets.c $(UNIT)/mock_flash.c $(UNIT)/recovery_env.c $(SRC)/recovery.c $(SRC)/crc32.c $(SRC)/metadata.c $(SRC)/image_verify.c \
         $(SRC)/image_crypto.c $(SRC)/boot_logic.c $(CRYPTO)/sha256.c $(APP)/safeflash_app.c \
         $(APP)/safeflash_update.c
HEADERS = $(wildcard *.h) $(wildcard $(SRC)/*.h) $(wildcard $(APP)/*.h) $(wildcard $(UNIT)/*.h)
TARGETS = fuzz_image fuzz_metadata fuzz_boot fuzz_install fuzz_recovery

.PHONY: run libfuzzer clean
run: fuzz_driver.exe
	./fuzz_driver.exe image $(ITERS)
	./fuzz_driver.exe metadata $(ITERS)
	./fuzz_driver.exe boot $(ITERS)
	./fuzz_driver.exe install $(ITERS)
	./fuzz_driver.exe recovery $(ITERS)

# micro-ecc is vendored third-party code: compiled unmodified, uninstrumented, warnings silenced
uecc_fuzz.o: $(UECC)/uECC.c
	$(CC) -std=gnu99 -O2 -w $(UECC_FLAGS) -I$(UECC) -c $< -o $@

fuzz_driver.exe: fuzz_driver.c $(COMMON) uecc_fuzz.o $(BUILD)/pubkey.c $(HEADERS)
	$(CC) $(CFLAGS) $(UECC_FLAGS) fuzz_driver.c $(COMMON) $(BUILD)/pubkey.c uecc_fuzz.o $(LDLIBS) -o $@

# libFuzzer: one binary per target; ASan+UBSan on our code, micro-ecc left uninstrumented
libfuzzer: uecc_fuzz.o
	for t in $(TARGETS); do \
	  $(CC) -std=c99 -Wall -Wextra -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -I$(SRC) -I$(APP) -I$(CRYPTO) -I$(UECC) -I$(UNIT) -I. $(UECC_FLAGS) -DFUZZ_TARGET=$$t \
	    fuzz_libfuzzer.c $(COMMON) $(BUILD)/pubkey.c uecc_fuzz.o -o $$t || exit 1; \
	done

# Line/branch coverage of the code under test reached by the driver (gcov). Reports in cov/.
COV_SRCS = $(SRC)/recovery.c $(SRC)/metadata.c $(SRC)/image_verify.c $(SRC)/image_crypto.c $(SRC)/boot_logic.c \
           $(APP)/safeflash_app.c $(APP)/safeflash_update.c
coverage: uecc_fuzz.o
	rm -rf cov && mkdir cov
	$(CC) $(CFLAGS) --coverage -O0 $(UECC_FLAGS) fuzz_driver.c $(COMMON) $(BUILD)/pubkey.c uecc_fuzz.o $(LDLIBS) -o cov/fuzz_cov.exe
	./cov/fuzz_cov.exe image $(ITERS) && ./cov/fuzz_cov.exe metadata $(ITERS) && ./cov/fuzz_cov.exe boot $(ITERS) \
	  && ./cov/fuzz_cov.exe install $(ITERS) && ./cov/fuzz_cov.exe recovery $(ITERS)
	for f in $(notdir $(basename $(COV_SRCS))); do gcov -b $$f.gcda 2>/dev/null; done \
	  | grep -A4 "^File '.*\(recovery\|metadata\|image_verify\|image_crypto\|boot_logic\|safeflash_app\|safeflash_update\)\.c'"
	mv *.gcov cov/ 2>/dev/null; rm -f *.gcda *.gcno

clean:
	rm -rf fuzz_driver.exe uecc_fuzz.o $(TARGETS) cov
