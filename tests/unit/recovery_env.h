#ifndef RECOVERY_ENV_H
#define RECOVERY_ENV_H

#include <stddef.h>
#include <stdint.h>

/*
 * Host implementation of the recovery byte stream (recovery_getc / recovery_putc from recovery.h) plus a
 * builder for the host->device stream of a complete transfer. Shared by the recovery test, the fault sweep
 * and the fuzz targets.
 */
void rec_env_set_input(const uint8_t *data, size_t len);   /* also clears the captured output */
const uint8_t *rec_env_output(size_t *len);

/* Stream for a full transfer of `img`: BEGIN, DATA chunks of 256, END. Returns its length. */
size_t rec_stream_from_image(uint8_t *out, const uint8_t *img, size_t img_len);

/* Parse the device's responses: count ACK / NAK frames and remember the last NAK reason + detail. */
typedef struct {
    unsigned acks, naks;
    unsigned last_nak_reason, last_nak_detail;
    uint32_t last_ack_offset;
} rec_replies_t;
rec_replies_t rec_env_parse_replies(void);

#endif
