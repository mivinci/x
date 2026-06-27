/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * uuid.h - UUID generation (v4, v5, v7), formatting, and parsing
 */

#ifndef XCRYPTO_UUID_H
#define XCRYPTO_UUID_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief A 128-bit UUID (RFC 4122 / RFC 9562).
 *
 * Value type — stack-allocatable, pass by value, no lifetime management.
 */
XDEF_STRUCT(xUuid) {
  uint8_t bytes[16];
};

/* ── Generation ─────────────────────────────────────────────────── */

/**
 * @brief Generate a version-4 (random) UUID.
 *
 * Uses xRandomBytes for cryptographically secure randomness.
 */
XCAPI(xUuid) xUuidV4(void);

/**
 * @brief Generate a version-7 (time-ordered random) UUID per RFC 9562.
 *
 * 48-bit Unix millisecond timestamp + 74 bits of randomness.
 * UUIDs generated in sequence are sortable by creation time.
 */
XCAPI(xUuid) xUuidV7(void);

/**
 * @brief Generate a version-5 (namespace + name SHA-1) UUID.
 *
 * Deterministic: the same namespace + name always produces the same UUID.
 * Requires linking against xcrypto for SHA-1.
 *
 * @param namespace  The namespace UUID (e.g. xUuidNamespaceDns()).
 * @param name       NUL-terminated name string.
 * @return           The generated UUID.
 */
XCAPI(xUuid) xUuidV5(xUuid ns, const char *name);

/* ── Formatting ─────────────────────────────────────────────────── */

/**
 * @brief Format a UUID as a lowercase hyphenated string.
 *
 * Output: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars + NUL = 37 bytes).
 *
 * @param uuid  The UUID to format.
 * @param buf   Destination buffer (must be at least 37 bytes).
 */
XCAPI(void) xUuidToString(xUuid uuid, char buf[37]);

/**
 * @brief Parse a UUID string into an xUuid.
 *
 * Accepts hyphenated ("550e8400-e29b-41d4-a716-446655440000") and
 * non-hyphenated ("550e8400e29b41d4a716446655440000") forms.
 * Case-insensitive.
 *
 * @param str  The UUID string to parse.
 * @param out  Output UUID (must not be NULL).
 * @return     xErrno_Ok on success, xErrno_InvalidArg on malformed input.
 */
XCAPI(xErrno) xUuidFromString(const char *str, xUuid *out);

/* ── Comparison ─────────────────────────────────────────────────── */

/**
 * @brief Compare two UUIDs lexicographically (memcmp-style).
 * @return -1, 0, or 1.
 */
XCAPI(int) xUuidCompare(xUuid a, xUuid b);

/**
 * @brief Check if a UUID is the nil UUID (all zeros).
 */
XCAPI(bool) xUuidIsNil(xUuid uuid);

/* ── Predefined namespace UUIDs (RFC 4122 Appendix C) ──────────── */

/** @brief DNS namespace UUID: 6ba7b810-9dad-11d1-80b4-00c04fd430c8 */
XCAPI(const xUuid *) xUuidNamespaceDns(void);

/** @brief URL namespace UUID: 6ba7b811-9dad-11d1-80b4-00c04fd430c8 */
XCAPI(const xUuid *) xUuidNamespaceUrl(void);

#endif /* XCRYPTO_UUID_H */
