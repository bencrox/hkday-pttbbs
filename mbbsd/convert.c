/* $Id: convert.c 5527 2012-01-26 16:39:25Z piaip $ */
#include "bbs.h"

#ifdef CONVERT
int (*convert_write)(VBUF *v, char c) = vbuf_add;
int (*convert_read)(VBUF *v, const void *buf, size_t len) = vbuf_putblk;
ConvertMode convert_mode = CONV_NORMAL;

int
convert_write_utf8(VBUF *v, char c) {
    static char trail[2] = {0};
    static uint8_t utf8[4];

    // trail must be little endian.
    if (trail[1]) {
        int len, i;
        uint16_t ucs;

        trail[0] = c;
        ucs = b2u_table[*(uint16_t*)trail];

        len = ucs2utf(ucs, utf8);
        utf8[len] = 0;

        // assert(len > 0 && len < 4);
        for (i = 0; i < len; i++)
            vbuf_add(v, utf8[i]);

        trail[1] = 0;
        return 1;
    }

    if (isascii(c)) {
        vbuf_add(v, c);
        return 1;
    }

    trail[1] = c;
    return 0;
}

int convert_read_utf8(VBUF *v, const void *buf, size_t len) {
    static uint8_t trail[6];
    static int ctrail = 0;
    uint16_t ucs;
    uint8_t c;
    int written = 0;

    while (len-- > 0) {
        c = *(uint8_t*)buf ++;
        if (ctrail) {
            trail[ctrail++] = c;
            // TODO this may create invalid chars.
            if (utf2ucs(trail, &ucs) > ctrail)
                continue;
            ucs = u2b_table[ucs];
            vbuf_add(v, ucs >> 8);
            vbuf_add(v, ucs & 0xFF);
            written += 2;
            ctrail = 0;
            continue;
        }

        if (isascii(c)) {
            vbuf_add(v, c);
            written++;
        } else {
            trail[0] = c;
            ctrail = 1;
        }
    }
    return written;
}

void set_converting_type(ConvertMode mode)
{
    switch(mode) {
        case CONV_NORMAL:
            convert_read = vbuf_putblk;
            convert_write = vbuf_add;
            break;

        case CONV_UTF8:
            convert_read = convert_read_utf8;
            convert_write = convert_write_utf8;
            break;
    }
    convert_mode = mode;
}

void init_convert()
{
    // nothing now.
}

#endif
