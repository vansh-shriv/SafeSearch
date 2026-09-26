#include "recovery_env.h"
#include "recovery.h"

#include <string.h>

#define OUT_CAP (64 * 1024)

static const uint8_t *in_data;
static size_t in_len, in_pos;
static uint8_t out[OUT_CAP];
static size_t out_len;

void rec_env_set_input(const uint8_t *data, size_t len)
{
    in_data = data;
    in_len = len;
    in_pos = 0;
    out_len = 0;
}

const uint8_t *rec_env_output(size_t *len)
{
    *len = out_len;
    return out;
}

int recovery_getc(void)
{
    return in_pos < in_len ? in_data[in_pos++] : -1;
}

void recovery_putc(uint8_t b)
{
    if (out_len < OUT_CAP)
        out[out_len++] = b;
}

size_t rec_stream_from_image(uint8_t *o, const uint8_t *img, size_t img_len)
{
    size_t n = 0;
    uint8_t p[4 + REC_MAX_DATA];

    p[0] = (uint8_t)img_len;
    p[1] = (uint8_t)(img_len >> 8);
    p[2] = (uint8_t)(img_len >> 16);
    p[3] = (uint8_t)(img_len >> 24);
    n += recovery_build_frame(o + n, REC_BEGIN, p, 4);
    for (size_t off = 0; off < img_len; off += REC_MAX_DATA) {
        size_t c = img_len - off < REC_MAX_DATA ? img_len - off : REC_MAX_DATA;
        p[0] = (uint8_t)off;
        p[1] = (uint8_t)(off >> 8);
        p[2] = (uint8_t)(off >> 16);
        p[3] = (uint8_t)(off >> 24);
        memcpy(p + 4, img + off, c);
        n += recovery_build_frame(o + n, REC_DATA, p, (uint16_t)(4 + c));
    }
    n += recovery_build_frame(o + n, REC_END, NULL, 0);
    return n;
}

rec_replies_t rec_env_parse_replies(void)
{
    rec_replies_t r;
    size_t i = 0;

    memset(&r, 0, sizeof r);
    while (i + 8 <= out_len) {
        uint16_t len;
        if (out[i] != REC_SOF) {
            i++;
            continue;
        }
        len = (uint16_t)(out[i + 2] | (out[i + 3] << 8));
        if (i + 8 + len > out_len)
            break;
        if (out[i + 1] == REC_ACK && len == 4) {
            r.acks++;
            r.last_ack_offset = (uint32_t)out[i + 4] | ((uint32_t)out[i + 5] << 8) | ((uint32_t)out[i + 6] << 16) |
                                ((uint32_t)out[i + 7] << 24);
        } else if (out[i + 1] == REC_NAK) {
            r.naks++;
            r.last_nak_reason = out[i + 4];
            r.last_nak_detail = len > 1 ? out[i + 5] : 0;
        }
        i += 8 + len;
    }
    return r;
}
