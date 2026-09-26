#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>

/*
 * Serial recovery mode: entered when nothing bootable exists (or on request), receives a complete signed
 * image over a byte stream, verifies it exactly like a normal boot, and only then commits metadata.
 * Hardware-independent: the environment supplies the byte stream (UART on target, arrays on host).
 *
 * Frame:  0xA5 | type u8 | len u16 LE | payload[len] | crc32 u32 LE   (crc over type, len, payload)
 *   host -> device: BEGIN  payload = total image length u32
 *                   DATA   payload = offset u32 + up to 256 bytes (offset multiple of 256, in order)
 *                   END    payload empty
 *                   ABORT  payload empty
 *   device -> host: ACK    payload = next expected offset u32
 *                   NAK    payload = reason u8 [, image status u8 when reason == REC_NAK_BAD_IMAGE]
 * Stop-and-wait: the host sends a frame, waits for ACK/NAK, and retries on NAK or timeout. A repeated DATA
 * frame (offset already received) is ACKed without being written again, so retries are idempotent.
 *
 * Safety properties (checked by the host sweep and fuzzers):
 *   - writes only the target slot and, after successful verification, the metadata sectors;
 *   - the first IMAGE_HEADER_SIZE bytes are held in RAM and written last, so an interrupted transfer never
 *     leaves a valid-looking header;
 *   - metadata is committed only after image_check_basic + image_check_signature pass, so recovery can never
 *     install something the normal boot would reject, including images below the anti-rollback floor.
 */
#define REC_SOF          0xA5u
#define REC_MAX_DATA     256u
#define REC_MAX_PAYLOAD  (4u + REC_MAX_DATA)

#define REC_BEGIN  0x01u
#define REC_DATA   0x02u
#define REC_END    0x03u
#define REC_ABORT  0x04u
#define REC_ACK    0x81u
#define REC_NAK    0x82u

#define REC_NAK_CRC        1u
#define REC_NAK_LEN        2u
#define REC_NAK_STATE      3u
#define REC_NAK_OFFSET     4u
#define REC_NAK_SIZE       5u
#define REC_NAK_FLASH      6u
#define REC_NAK_BAD_IMAGE  7u
#define REC_NAK_TYPE       8u

#define RECOVERY_INSTALLED   0
#define RECOVERY_EOF       (-1)   /* host tests only: the input stream ended */

/* Environment: next input byte (0..255), or -1 when the stream ends (never on target); output byte. */
int recovery_getc(void);
void recovery_putc(uint8_t b);

/* Runs until an image has been received, verified and committed (RECOVERY_INSTALLED) or the input ends. */
int recovery_run(void);

/* Host-side helper shared by tests, tools and the fuzzers: append one frame to buf, returns its length. */
uint32_t recovery_build_frame(uint8_t *buf, uint8_t type, const uint8_t *payload, uint16_t len);

#endif
