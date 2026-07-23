/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json.h - DOM-Style JSON Parser / Builder / Serializer
 *
 * xJson provides a lightweight, arena-backed DOM API for JSON.  Parse
 * a JSON string into an in-memory tree, query and modify it, then
 * serialise back to a string — all with a consistent xJsonNew* / xJsonFree
 * lifecycle.
 *
 * Two parse modes:
 *   xJsonParse     — zero-copy: string values point into the input buffer.
 *                    The caller must keep the buffer alive while the tree
 *                    is in use.
 *   xJsonParseCopy — safe: all string values are copied into the arena.
 *                    The input buffer can be freed immediately.
 *
 * Manual construction (xJsonNewObject, xJsonNewArray, ...) uses malloc
 * for individual nodes.  Mixing parse trees (arena-backed) with manually
 * constructed trees is unsupported and may lead to use-after-free or
 * double-free.
 *
 * Memory model:
 *   - Parse trees:  one xArena holds every node and (for ParseCopy) every
 *                   string.  xJsonFree() destroys the arena in O(1).
 *   - Manual trees: each node is independently malloc'd.  xJsonFree()
 *                   recursively walks and frees the subtree.
 *
 * Ownership transfer:
 *   xJsonObjectSet / xJsonArraySet / xJsonArrayAppend / xJsonArrayInsert
 *   take ownership of the value node.  The caller must not free it.
 *   Replacing an existing value frees the old one automatically.
 *
 * Typical usage:
 *   xJson *root = xJsonParse(json_str, len);
 *   xJson *name = xJsonObjectGet(root, "name");
 *   printf("Hello, %s!\n", xJsonString(name));
 *   xJsonFree(root);
 */

#ifndef XJSON_JSON_H
#define XJSON_JSON_H

#include <stddef.h>
#include <stdint.h>

#include <x/base/base.h>

/* ───────────────────── Types ───────────────────── */

/** @brief Opaque handle for a JSON node (object, array, string, number,
 *         boolean, or null). */
XDEF_HANDLE(xJson);

/** @brief Opaque handle for an object key-value iterator. */
XDEF_HANDLE(xJsonIterator);

/* ───────────────────── Type flags ───────────────────── */

/**
 * @brief JSON value type (bits 0–3 of the `flags` byte).
 * @ingroup xJson
 */
#define XJSON_NULL      0x00
#define XJSON_BOOL      0x01
#define XJSON_INT       0x02
#define XJSON_DOUBLE    0x03
#define XJSON_STRING    0x04
#define XJSON_ARRAY     0x05
#define XJSON_OBJECT    0x06
#define XJSON_ITERATOR  0x07   /**< internal: iterator handle */

/** @brief Mask to extract the type from the `flags` byte. */
#define XJSON_TYPE_MASK 0x0F

/**
 * @brief Owned flag (bit 4): set when the node has been transferred into
 *        a parent via xJsonObjectSet / xJsonArraySet / xJsonArrayAppend /
 *        xJsonArrayInsert.  Prevents double-free from a subsequent
 *        xJsonFree() on the transferred handle.
 */
#define XJSON_FLAG_OWNED 0x10

/* ───────────────────── Parse ───────────────────── */

/**
 * @brief Parse a JSON string into a DOM tree (zero-copy).
 * @ingroup xJson
 *
 * String values point directly into @p json — the caller must keep the
 * buffer alive for the lifetime of the tree.  All nodes are allocated
 * from a single internal arena; xJsonFree() destroys the tree in O(1).
 *
 * @param json  NUL-terminated or length-delimited JSON string.
 * @param len   Length in bytes.
 * @return Root node, or NULL on parse failure.
 */
XCAPI(xJson *) xJsonParse(const char *json, size_t len);

/**
 * @brief Parse a JSON string into a DOM tree (safe copy).
 * @ingroup xJson
 *
 * Semantically identical to xJsonParse(), but all string values are
 * copied into the arena so @p json can be freed immediately.
 *
 * @param json  NUL-terminated or length-delimited JSON string.
 * @param len   Length in bytes.
 * @return Root node, or NULL on parse failure.
 */
XCAPI(xJson *) xJsonParseCopy(const char *json, size_t len);

/* ───────────────────── Free ───────────────────── */

/**
 * @brief Release a JSON node, iterator, or entire parse tree.
 * @ingroup xJson
 *
 * Dispatch based on the `flags` field:
 *   - XJSON_ITERATOR → free the iterator struct.
 *   - Arena-backed node → xArenaDestroy (O(1) for the whole parse tree).
 *   - malloc-backed node  → recursive free of subtree, then self.
 *
 * Passing NULL is a no-op.  Passing a node whose XJSON_FLAG_OWNED bit
 * is set is also a no-op (it was already transferred into a parent).
 *
 * @param ptr  Node, iterator, or NULL.
 */
XCAPI(void) xJsonFree(void *ptr);

/* ───────────────────── Query ───────────────────── */

/**
 * @brief Return the JSON type of @p node.
 * @ingroup xJson
 *
 * @return One of XJSON_NULL, XJSON_BOOL, XJSON_INT, XJSON_DOUBLE,
 *         XJSON_STRING, XJSON_ARRAY, or XJSON_OBJECT.
 */
XCAPI(int) xJsonType(const xJson *node);

/**
 * @brief Return the boolean value of a XJSON_BOOL node.
 * @ingroup xJson
 *
 * Behaviour is undefined if @p node is not XJSON_BOOL.
 */
XCAPI(int) xJsonBool(const xJson *node);

/**
 * @brief Return the integer value of a XJSON_INT node.
 * @ingroup xJson
 *
 * Behaviour is undefined if @p node is not XJSON_INT.
 */
XCAPI(int64_t) xJsonInt(const xJson *node);

/**
 * @brief Return the floating-point value of a XJSON_DOUBLE node.
 * @ingroup xJson
 *
 * Behaviour is undefined if @p node is not XJSON_DOUBLE.
 */
XCAPI(double) xJsonDouble(const xJson *node);

/**
 * @brief Return the string value of a XJSON_STRING node.
 * @ingroup xJson
 *
 * The returned pointer is NUL-terminated and valid for the lifetime of
 * the node.  Behaviour is undefined if @p node is not XJSON_STRING.
 */
XCAPI(const char *) xJsonString(const xJson *node);

/**
 * @brief Return the string length of a XJSON_STRING node.
 * @ingroup xJson
 *
 * Unlike xJsonString(), this returns the actual byte length without relying
 * on NUL termination — useful for strings that may contain embedded NULs.
 *
 * Behaviour is undefined if @p node is not XJSON_STRING.
 */
XCAPI(size_t) xJsonStringLength(const xJson *node);

/* ───────────────────── Construct ───────────────────── */

/**
 * @brief Create a new XJSON_NULL node.
 * @ingroup xJson
 */
XCAPI(xJson *) xJsonNewNull(void);

/**
 * @brief Create a new XJSON_BOOL node.
 * @ingroup xJson
 */
XCAPI(xJson *) xJsonNewBool(int v);

/**
 * @brief Create a new XJSON_INT node.
 * @ingroup xJson
 */
XCAPI(xJson *) xJsonNewInt(int64_t v);

/**
 * @brief Create a new XJSON_DOUBLE node.
 * @ingroup xJson
 */
XCAPI(xJson *) xJsonNewDouble(double v);

/**
 * @brief Create a new XJSON_STRING node (NUL-terminated input).
 * @ingroup xJson
 *
 * The string is copied internally.  The caller retains ownership of @p str.
 */
XCAPI(xJson *) xJsonNewString(const char *str);

/**
 * @brief Create a new XJSON_STRING node with explicit length.
 * @ingroup xJson
 *
 * Useful when the string may contain embedded NULs.
 */
XCAPI(xJson *) xJsonNewStringN(const char *str, size_t len);

/**
 * @brief Create a new, empty XJSON_ARRAY node.
 * @ingroup xJson
 */
XCAPI(xJson *) xJsonNewArray(void);

/**
 * @brief Create a new, empty XJSON_OBJECT node.
 * @ingroup xJson
 */
XCAPI(xJson *) xJsonNewObject(void);

/* ───────────────────── Object ───────────────────── */

/**
 * @brief Look up a key in an object node.
 * @ingroup xJson
 *
 * Returns the value node for @p key, or NULL if not found.
 * The returned node is still owned by @p obj — do not free it.
 *
 * @param obj  Object node (must be XJSON_OBJECT).
 * @param key  NUL-terminated key.
 */
XCAPI(xJson *) xJsonObjectGet(const xJson *obj, const char *key);

/**
 * @brief Set a key-value pair in an object node.
 * @ingroup xJson
 *
 * Takes ownership of @p val.  If @p key already exists, the old value
 * is freed and replaced.  Returns 0 on success, non-zero if @p obj is
 * not an object or @p val is NULL.
 *
 * @param obj  Object node (must be XJSON_OBJECT).
 * @param key  NUL-terminated key.
 * @param val  Value node (ownership transferred).
 */
XCAPI(int) xJsonObjectSet(xJson *obj, const char *key, xJson *val);

/**
 * @brief Remove a key from an object node.
 * @ingroup xJson
 *
 * The removed value is freed.  No-op if @p key does not exist.
 *
 * @param obj  Object node (must be XJSON_OBJECT).
 * @param key  NUL-terminated key.
 */
XCAPI(void) xJsonObjectDel(xJson *obj, const char *key);

/**
 * @brief Return the number of key-value pairs in an object.
 * @ingroup xJson
 *
 * Behaviour is undefined if @p obj is not XJSON_OBJECT.
 */
XCAPI(int) xJsonObjectSize(const xJson *obj);

/* ───────────────────── Object Iterator ───────────────────── */

/**
 * @brief Create an iterator over the key-value pairs of an object.
 * @ingroup xJson
 *
 * The iterator is independent of the object's lifetime — the caller must
 * free it with xJsonFree() when done.  Modifying the object during
 * iteration (xJsonObjectSet / xJsonObjectDel) on the key returned by the
 * iterator invalidates the iterator.
 *
 * @param obj  Object node (must be XJSON_OBJECT).
 * @return New iterator, or NULL if @p obj is not an object.
 */
XCAPI(xJsonIterator *) xJsonNewIterator(const xJson *obj);

/**
 * @brief Advance the iterator to the next key-value pair.
 * @ingroup xJson
 *
 * @return Non-zero if another pair is available, 0 if iteration is complete.
 */
XCAPI(int) xJsonIteratorNext(xJsonIterator *it);

/**
 * @brief Return the key of the current pair.
 * @ingroup xJson
 *
 * @param len  If non-NULL, receives the key length in bytes.
 * @return NUL-terminated key string.  Valid until the next mutating call.
 */
XCAPI(const char *) xJsonIteratorKey(xJsonIterator *it, size_t *len);

/**
 * @brief Return the value of the current pair.
 * @ingroup xJson
 *
 * The returned node is still owned by the object — do not free it.
 */
XCAPI(xJson *) xJsonIteratorValue(xJsonIterator *it);

/* ───────────────────── Array ───────────────────── */

/**
 * @brief Return the element at index @p idx.
 * @ingroup xJson
 *
 * Negative indices count from the end (-1 = last element).
 * Returns NULL if @p idx is out of bounds.
 *
 * @param arr  Array node (must be XJSON_ARRAY).
 * @param idx  Zero-based index, may be negative.
 */
XCAPI(xJson *) xJsonArrayGet(const xJson *arr, int idx);

/**
 * @brief Replace the element at index @p idx.
 * @ingroup xJson
 *
 * Takes ownership of @p val.  The old element at @p idx is freed.
 * Negative indices count from the end.  Returns 0 on success, non-zero
 * if @p idx is out of bounds or @p arr is not an array.
 *
 * @param arr  Array node (must be XJSON_ARRAY).
 * @param idx  Zero-based index, may be negative.
 * @param val  Value node (ownership transferred).
 */
XCAPI(int) xJsonArraySet(xJson *arr, int idx, xJson *val);

/**
 * @brief Append a value to the end of an array.
 * @ingroup xJson
 *
 * Takes ownership of @p val.  Returns 0 on success, non-zero on failure.
 *
 * @param arr  Array node (must be XJSON_ARRAY).
 * @param val  Value node (ownership transferred).
 */
XCAPI(int) xJsonArrayAppend(xJson *arr, xJson *val);

/**
 * @brief Insert a value at index @p idx, shifting existing elements right.
 * @ingroup xJson
 *
 * Takes ownership of @p val.  @p idx must be in [0, size].  Returns 0 on
 * success, non-zero if @p idx is out of bounds or @p arr is not an array.
 *
 * @param arr  Array node (must be XJSON_ARRAY).
 * @param idx  Insertion position, may be size (append).
 * @param val  Value node (ownership transferred).
 */
XCAPI(int) xJsonArrayInsert(xJson *arr, int idx, xJson *val);

/**
 * @brief Remove and free the element at index @p idx.
 * @ingroup xJson
 *
 * Negative indices count from the end.  No-op if @p idx is out of bounds.
 *
 * @param arr  Array node (must be XJSON_ARRAY).
 * @param idx  Zero-based index, may be negative.
 */
XCAPI(void) xJsonArrayRemove(xJson *arr, int idx);

/**
 * @brief Return the number of elements in an array.
 * @ingroup xJson
 *
 * Behaviour is undefined if @p arr is not XJSON_ARRAY.
 */
XCAPI(int) xJsonArraySize(const xJson *arr);

/* ───────────────────── Serialise ───────────────────── */

/**
 * @brief Serialise a JSON tree to a compact string.
 * @ingroup xJson
 *
 * Returns a malloc'd, NUL-terminated string: `{"a":1,"b":[2,3]}`.
 * The caller must free the string with free().
 *
 * @param node  Root node.
 * @return JSON string, or NULL on allocation failure.
 */
XCAPI(char *) xJsonStringify(const xJson *node);

/**
 * @brief Serialise a JSON tree to a pretty-printed string (2-space indent).
 * @ingroup xJson
 *
 * Returns a malloc'd, NUL-terminated, multi-line string.  The caller
 * must free the string with free().
 *
 * @param node  Root node.
 * @return JSON string, or NULL on allocation failure.
 */
XCAPI(char *) xJsonStringifyPretty(const xJson *node);

/**
 * @brief Serialise a JSON tree into a caller-supplied buffer.
 * @ingroup xJson
 *
 * On entry, *len must hold the buffer capacity.  On return, *len is the
 * number of bytes that would have been written (including NUL), even if
 * the buffer was too small.
 *
 * @param node   Root node.
 * @param pretty Non-zero for pretty-printing.
 * @param buf    Output buffer (may be NULL if len is 0).
 * @param len    On entry: buffer size.  On return: required bytes.
 * @return 0 on success (buffer large enough), non-zero if truncated.
 */
XCAPI(int) xJsonStringifyTo(const xJson *node, int pretty, char *buf, size_t *len);

#endif /* XJSON_JSON_H */
