/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json_parse.h - Internal JSON Tokenizer
 *
 * Pure lexical helpers for JSON parsing.  These functions are shared by the
 * DOM parser (json.c) and the SAX parser (json_sax.c).  They operate on a
 * pointer pair (cur, end) and do NOT allocate xJson nodes — callers decide
 * what to build from the returned tokens.
 *
 * All symbols use XCAPI_LOCAL (hidden visibility); this header is not
 * installed as part of the public API.
 */

#ifndef XJSON_PARSE_H
#define XJSON_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/arena.h>
#include <x/base/base.h>

/* ───────────────────── Tokenizer API ───────────────────── */

/**
 * @brief Advance @p cur past whitespace characters (space, tab, CR, LF).
 */
XCAPI_LOCAL(void) xjson_tok_skip_ws(const char **cur, const char *end);

/**
 * @brief Try to match an exact literal at the current position.
 *
 * Whitespace is skipped first.  On success @p cur advances past the literal
 * and the function returns 1; on failure @p cur is unchanged and 0 is
 * returned.
 */
XCAPI_LOCAL(int) xjson_tok_match(const char **cur, const char *end, const char *lit);

/**
 * @brief Parse a JSON string token (surrounded by '"').
 *
 * Handles all standard escape sequences including \\uXXXX (decoded as
 * UTF-8).  The decoded string is allocated from @p arena.
 *
 * @param cur   In/out: current parse position (must point to '"').
 * @param end   One-past-the-end of input.
 * @param arena Arena for output buffer allocation.
 * @param out   Receives the decoded, NUL-terminated string pointer.
 * @param len   Receives the byte length (not counting NUL).
 * @return 0 on success, -1 on parse error or allocation failure.
 */
XCAPI_LOCAL(int) xjson_tok_string(const char **cur, const char *end, xArena *arena,
                                   char **out, size_t *len);

/**
 * @brief Parse a JSON number token.
 *
 * On success @p cur advances past all consumed digits.  The value is
 * returned via @p is_double and either @p int_val or @p double_val.
 *
 * @param cur        In/out: current parse position (must point to '-' or digit).
 * @param end        One-past-the-end of input.
 * @param is_double  Set to 1 for floating-point, 0 for integer.
 * @param int_val    Set when !is_double.
 * @param double_val Set when is_double.
 * @return 0 on success, -1 on parse error.
 */
XCAPI_LOCAL(int) xjson_tok_number(const char **cur, const char *end,
                                   int *is_double, int64_t *int_val,
                                   double *double_val);

#endif /* XJSON_PARSE_H */
