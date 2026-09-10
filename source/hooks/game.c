/* game.c -- hooks and patches for game runtime
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#include "../util.h"
#include "../hooks.h"
#include "../so_util.h"

static void WriteLogEntry_hook(const char *fmt, ...) {
  char buf[2048];
  va_list list;
  va_start(list, fmt);
  vsnprintf(buf, sizeof(buf), fmt, list);
  va_end(list);

  debugPrintf("[RVGL] %s\n", buf);
}

void patch_game(void) {
  uintptr_t wle = so_find_addr(&so_main, "_Z13WriteLogEntryPKcz");
  if (wle) {
    hook_arm64(wle, (uintptr_t)WriteLogEntry_hook);
    debugPrintf("Hooked _Z13WriteLogEntryPKcz at %p\n", (void *)wle);
  }

  uintptr_t demo_timeout_addr = so_find_addr(&so_main, "demo_timeout");
  if (demo_timeout_addr) {
    *(int *)demo_timeout_addr = 0;
    debugPrintf("Set demo_timeout = 0 at %p\n", (void *)demo_timeout_addr);
  }

  uintptr_t no_demo_addr = so_find_addr(&so_main, "no_demo");
  if (no_demo_addr) {
    *(uint8_t *)no_demo_addr = 1;
    debugPrintf("Set no_demo = 1 at %p\n", (void *)no_demo_addr);
  }
}
