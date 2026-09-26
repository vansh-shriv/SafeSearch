#ifndef BOOT_LOGIC_H
#define BOOT_LOGIC_H

#include <stdint.h>

/*
 * Hardware-independent boot decision: metadata handling, image verification, trial accounting and
 * revert. Compiled into the bootloader and, unchanged, into the host fault-injection sweep.
 * The environment supplies flash_hal.h, the public key, and the two console hooks below.
 */
void boot_puts(const char *s);
void boot_puthex(uint32_t v);

typedef struct {
    uint8_t  slot;      /* slot to boot */
    uint8_t  trial;     /* 1 if this boot is an unconfirmed trial (arm the watchdog) */
    uint8_t  trial_no;  /* attempt number when trial */
    uint32_t version;   /* version of the image being booted */
} boot_decision_t;

/* Returns 0 and fills *d, or -1 if nothing can be booted (a message has been printed). */
int boot_decide(boot_decision_t *d);

#endif
