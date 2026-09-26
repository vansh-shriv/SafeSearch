#ifndef SAFEFLASH_APP_H
#define SAFEFLASH_APP_H

#include <stddef.h>

/*
 * Application-side SafeFlash API.
 *
 * sf_confirm_healthy(): mark the running image good. Call once the app has proven it works. Until then,
 * a TRIAL image gets no watchdog kicks and will be reset, eventually reverting to the previous slot.
 * Also ratchets the anti-rollback floor up to the running image's version. Returns 0 on success.
 *
 * sf_wdt_kick(): call regularly from the main loop. Kicks the IWDG only when it is safe to (image
 * confirmed, or not in a trial). Harmless when the watchdog is not running.
 *
 * sf_install_update(): write a complete signed image (header + payload, as produced by sign_image.py)
 * into the INACTIVE slot, then atomically switch metadata to it in TRIAL state. The running image is
 * never touched. Does not verify the signature: the bootloader does, and reverts if it fails. Returns
 * SF_OK and the caller should reset to boot the new image.
 */
#define SF_OK              0
#define SF_ERR_STATE      (-1)   /* no valid metadata */
#define SF_ERR_IMAGE      (-2)   /* not a plausible image (length / magic) */
#define SF_ERR_ROLLBACK   (-3)   /* version below the anti-rollback floor */
#define SF_ERR_FLASH      (-4)

int sf_confirm_healthy(void);
void sf_wdt_kick(void);
int sf_install_update(const void *img, size_t len);

#endif
