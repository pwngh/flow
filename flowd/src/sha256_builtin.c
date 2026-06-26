/* src/sha256_builtin.c
 *
 * SHA-256 per FIPS 180-4. A stock implementation — the only project-specific
 * facts are in sha256_builtin.h (why it exists, and that it must match
 * OpenSSL). Kept dependency-free so the WASM build needs nothing but a C
 * compiler.
 */
#include "sha256_builtin.h"

#include <stdint.h>
#include <string.h>

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define BSIG0(x)  (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define BSIG1(x)  (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SSIG0(x)  (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SSIG1(x)  (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void
sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + BSIG1(e) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void
flowd_sha256_builtin(const void *data, size_t len, unsigned char out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const unsigned char *p = (const unsigned char *)data;

    size_t whole = len / 64u;
    for (size_t i = 0; i < whole; i++)
        sha256_block(h, p + i * 64u);

    /* Final block(s): the leftover bytes, a 0x80 terminator, zero padding,
     * and the message length in bits as a 64-bit big-endian trailer. A tail
     * of 56+ bytes leaves no room for the trailer, so it spills to a second
     * block. */
    unsigned char tail[128] = { 0 };
    size_t rem = len - whole * 64u;
    memcpy(tail, p + whole * 64u, rem);
    tail[rem] = 0x80;
    size_t blocks = (rem < 56u) ? 64u : 128u;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++)
        tail[blocks - 1 - i] = (unsigned char)(bits >> (i * 8));

    sha256_block(h, tail);
    if (blocks == 128u)
        sha256_block(h, tail + 64);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(h[i]);
    }
}
