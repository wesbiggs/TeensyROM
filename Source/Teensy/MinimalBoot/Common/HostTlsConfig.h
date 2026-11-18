// SPDX-License-Identifier: MIT
//
// mbedTLS settings for a --host-tls extension host, layered over the config.h
// that Teensyduino 1.61.0 bundles in FNET, which includes this file through
// MBEDTLS_USER_CONFIG_FILE just before its check_config.h.
#pragma once

// The memory buffer allocator lets the host hand mbedTLS a RAM2 buffer of its
// own instead of the 16 KiB host heap.
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C

// FNET compiles each mbedTLS source into one .flashmem section, so linking any
// function from a file keeps that file's self-test, and x509.c's self-test keeps
// certs.c's test CA, client and server keys. The self-tests also link newlib's
// printf, and libc code stays in ITCM. A host uses neither.
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_CERTS_C

// FNET's config.h caps records at 10 KiB. TLS 1.2 allows 16 KiB, and servers
// that ignore max_fragment_length (release-assets.githubusercontent.com among
// them) send full 16 KiB records, which a smaller receive buffer rejects. A
// client with no client certificate sends only small records, so the send
// buffer stays small.
#undef MBEDTLS_SSL_MAX_CONTENT_LEN
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
