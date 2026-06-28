/*
 * bus.h - Pub/sub notification bus for dlproxy internal communication
 */
#ifndef DLP_BUS_H
#define DLP_BUS_H

#include <x/base/error.h>

/** Opaque bus handle. */
typedef struct dlp_bus *dlp_bus_t;

/** Callback type. */
typedef void (*dlp_bus_cb)(void *arg);

/**
 * @brief Create a bus instance.
 */
dlp_bus_t dlp_bus_create(void);

/**
 * @brief Destroy a bus and free all subscribers.
 */
void dlp_bus_destroy(dlp_bus_t b);

/**
 * @brief Subscribe to a key. cb is invoked on publish.
 */
xErrno dlp_bus_subscribe(dlp_bus_t b, const char *key, dlp_bus_cb cb, void *arg);

/**
 * @brief Publish to a key. All subscribers are invoked synchronously.
 */
void dlp_bus_publish(dlp_bus_t b, const char *key);

#endif
