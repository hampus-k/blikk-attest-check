#include "base64.h"

static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const unsigned char *in, size_t in_len,
                      char *out, size_t out_size)
{
    const size_t needed = BASE64_ENCODED_SIZE(in_len);
    size_t i;
    size_t o = 0;

    if (out == NULL || out_size < needed) {
        return (size_t)-1;
    }

    for (i = 0; i + 3 <= in_len; i += 3) {
        const unsigned int chunk =
            ((unsigned int)in[i] << 16) |
            ((unsigned int)in[i + 1] << 8) |
            (unsigned int)in[i + 2];

        out[o++] = BASE64_TABLE[(chunk >> 18) & 0x3F];
        out[o++] = BASE64_TABLE[(chunk >> 12) & 0x3F];
        out[o++] = BASE64_TABLE[(chunk >> 6) & 0x3F];
        out[o++] = BASE64_TABLE[chunk & 0x3F];
    }

    switch (in_len - i) {
    case 1: {
        const unsigned int chunk = (unsigned int)in[i] << 16;
        out[o++] = BASE64_TABLE[(chunk >> 18) & 0x3F];
        out[o++] = BASE64_TABLE[(chunk >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
        break;
    }
    case 2: {
        const unsigned int chunk =
            ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        out[o++] = BASE64_TABLE[(chunk >> 18) & 0x3F];
        out[o++] = BASE64_TABLE[(chunk >> 12) & 0x3F];
        out[o++] = BASE64_TABLE[(chunk >> 6) & 0x3F];
        out[o++] = '=';
        break;
    }
    default:
        break;
    }

    out[o] = '\0';
    return o;
}
