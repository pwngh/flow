/* src/sha256_builtin.h
 *
 * Self-contained SHA-256 for builds that must not link OpenSSL (the WASM
 * playground). util.c selects it over libcrypto's SHA256() when compiled
 * with -DFLOWD_BUILTIN_SHA256; the native build is unchanged.
 *
 * It must produce byte-identical digests to OpenSSL: the trace value store
 * is content-addressed by these hashes, so a divergent digest would split
 * the address space between native and WASM runs.
 */
#ifndef FLOWD_SHA256_BUILTIN_H
#define FLOWD_SHA256_BUILTIN_H

#include <stddef.h>

/* Hash `len` bytes of `data` into the 32-byte big-endian digest `out`. */
void flowd_sha256_builtin(const void *data, size_t len, unsigned char out[32]);

#endif /* FLOWD_SHA256_BUILTIN_H */
