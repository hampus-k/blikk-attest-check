#ifndef BLIKK_BASE64_H
#define BLIKK_BASE64_H

#include <stddef.h>

/*
 * Base64-encode `in_len` bytes at `in` into `out`.
 *
 * `out` must be at least BASE64_ENCODED_SIZE(in_len) bytes, including the
 * terminating NUL. Returns the number of characters written, excluding the
 * NUL terminator, or (size_t)-1 if `out_size` is too small.
 */
size_t base64_encode(const unsigned char *in, size_t in_len,
                      char *out, size_t out_size);

/* Encoded length (without NUL) for `n` input bytes. */
#define BASE64_ENCODED_LEN(n) ((((n) + 2) / 3) * 4)
/* Buffer size needed for base64_encode(), including the NUL terminator. */
#define BASE64_ENCODED_SIZE(n) (BASE64_ENCODED_LEN(n) + 1)

#endif /* BLIKK_BASE64_H */
