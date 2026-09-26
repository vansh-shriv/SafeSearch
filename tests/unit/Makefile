# Host unit tests. Run with: mingw32-make -C tests/unit test
CC = gcc
SRC = ../../bootloader/src
CFLAGS = -std=c99 -Wall -Wextra -Werror -g -I$(SRC) -I.
OBJS = test_main.c mock_flash.c $(SRC)/crc32.c $(SRC)/metadata.c $(SRC)/image_verify.c

test: test_main.exe
	./test_main.exe

test_main.exe: $(OBJS) $(wildcard *.h) $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) $(OBJS) -o $@

clean:
	rm -f test_main.exe
