#ifndef SAFEFLASH_APP_H
#define SAFEFLASH_APP_H

/*
 * Application-side SafeFlash API.
 *
 * sf_confirm_healthy(): mark the running image good. Call once the app has proven it works. Until then,
 * a TRIAL image gets no watchdog kicks and will be reset, eventually reverting to the previous slot.
 * Returns 0 on success.
 *
 * sf_wdt_kick(): call regularly from the main loop. Kicks the IWDG only when it is safe to (image
 * confirmed, or not in a trial). Harmless when the watchdog is not running.
 */
int sf_confirm_healthy(void);
void sf_wdt_kick(void);

#endif
