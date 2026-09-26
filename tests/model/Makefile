# Explicit-state model check of the production boot/metadata/confirm/install code (crypto abstracted).
#   mingw32-make -C tests/model check
# Does not need the firmware build: no images, no keys, no crypto.
ifeq ($(origin CC),default)
CC = gcc
endif
SRC = ../../bootloader/src
APP = ../../app/src
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -g -I$(SRC) -I$(APP)

# Real: boot_logic, metadata, crc32, confirm, install.  Replaced by the harness: flash HAL, image verifier.
SRCS = model_check.c $(SRC)/boot_logic.c $(SRC)/metadata.c $(SRC)/crc32.c $(APP)/safeflash_app.c $(APP)/safeflash_update.c

.PHONY: check clean
check: model_check.exe
	./model_check.exe

model_check.exe: $(SRCS) $(wildcard $(SRC)/*.h) $(wildcard $(APP)/*.h)
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -f model_check.exe
