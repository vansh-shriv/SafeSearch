/*
 * libFuzzer entry point. Build one binary per target with -DFUZZ_TARGET=fuzz_image (etc.):
 *   clang -fsanitize=fuzzer,address,undefined ... fuzz_libfuzzer.c fuzz_targets.c ...
 * Run from tests/fuzz so the relative ../../build/ image paths resolve.
 */
#include "fuzz_targets.h"

#ifndef FUZZ_TARGET
#error "define FUZZ_TARGET to one of fuzz_image, fuzz_metadata, fuzz_boot, fuzz_install, fuzz_recovery"
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return FUZZ_TARGET(data, size);
}
