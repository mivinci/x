/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_global.c - Global (process-wide) event loop singleton
 */

#include <x/base/event.h>
#include <x/base/thread.h>

#include <stdlib.h>

static xEventLoop g_global_loop = NULL;
static xOnce      g_global_once = X_ONCE_INIT;

static void global_loop_destroy(void) {
  if (g_global_loop) {
    xEventLoopDestroy(g_global_loop);
    g_global_loop = NULL;
  }
}

static void global_loop_init(void) {
  g_global_loop = xEventLoopCreate();
  if (g_global_loop) {
    atexit(global_loop_destroy);
  }
}

xEventLoop xEventLoopGlobal(void) {
  xOnceCall(&g_global_once, global_loop_init);
  return g_global_loop;
}
