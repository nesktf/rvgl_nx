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
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "../util.h"
#include "../hooks.h"
#include "../so_util.h"

static AppletHookCookie s_appletHookCookie;

extern bool s_inSwkbd;
extern bool s_textInputActive;

typedef enum {
  TEXT_FIELD_NONE = 0,
  TEXT_FIELD_HOST,
  TEXT_FIELD_NAME,
} TextFieldType;

static TextFieldType s_currentTextField = TEXT_FIELD_NONE;
static u64 s_lastTextFieldTick = 0;
static void (*orig_DrawHostComputer)(int, int) = NULL;
static void (*orig_DrawEnterName)(int, int) = NULL;

void touch_text_field(void) {
  s_lastTextFieldTick = armGetSystemTick();
}

bool is_text_field_active(void) {
  if (s_lastTextFieldTick == 0) {
    s_currentTextField = TEXT_FIELD_NONE;
    return false;
  }
  u64 diff = armGetSystemTick() - s_lastTextFieldTick;
  if (diff >= (armGetSystemTickFreq() / 10)) {
    s_currentTextField = TEXT_FIELD_NONE;
    return false;
  }
  return true;
}

static void DrawHostComputer_hook(int a1, int a2) {
  s_currentTextField = TEXT_FIELD_HOST;
  touch_text_field();
  if (orig_DrawHostComputer) {
    orig_DrawHostComputer(a1, a2);
  }
}

static void DrawEnterName_hook(int a1, int a2) {
  s_currentTextField = TEXT_FIELD_NAME;
  touch_text_field();
  if (orig_DrawEnterName) {
    orig_DrawEnterName(a1, a2);
  }
}

void apply_text_input(const char *text) {
  if (!text || text[0] == '\0') return;

  if (s_currentTextField == TEXT_FIELD_HOST) {
    uintptr_t settings_addr = so_find_addr_rx(&so_main, "settings");
    if (settings_addr) {
      char *host_ip = (char *)(settings_addr + 0x39c);
      strncpy(host_ip, text, 31);
      host_ip[31] = '\0';
      debugPrintf("apply_text_input: directly set host IP to '%s'\n", host_ip);
    }
  } else if (s_currentTextField == TEXT_FIELD_NAME) {
    uintptr_t ts_addr = so_find_addr_rx(&so_main, "titlescreen_data");
    if (ts_addr) {
      int *name_len = (int *)(ts_addr + 36);
      char *name_buf = (char *)(ts_addr + 40);
      int len = strlen(text);
      if (len > 15) len = 15;
      memcpy(name_buf, text, len);
      name_buf[len] = '\0';
      *name_len = len;
      debugPrintf("apply_text_input: directly set profile name to '%s' (len=%d)\n", name_buf, len);
    }
  }
}

static void onAppletHook(AppletHookType type, void *param) {
  if (s_inSwkbd || (s_textInputActive && is_text_field_active())) return;

  if (type == AppletHookType_OnFocusState) {
    AppletFocusState state = appletGetFocusState();
    debugPrintf("[AppletHook] OnFocusState: %d\n", state);
    if (state == AppletFocusState_Background) {
      debugPrintf("[AppletHook] Lost focus / console sleep -> sending FOCUS_LOST\n");
      SDL_Event ev;
      memset(&ev, 0, sizeof(ev));
      ev.type = SDL_WINDOWEVENT;
      ev.window.type = SDL_WINDOWEVENT;
      ev.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
      SDL_PushEvent(&ev);
    } else if (state == AppletFocusState_InFocus) {
      debugPrintf("[AppletHook] Regained focus -> sending FOCUS_GAINED\n");
      SDL_Event ev;
      memset(&ev, 0, sizeof(ev));
      ev.type = SDL_WINDOWEVENT;
      ev.window.type = SDL_WINDOWEVENT;
      ev.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
      SDL_PushEvent(&ev);
    }
  }
}

static void set_switch_local_ip(void) {
  uintptr_t local_ip_addr = so_find_addr(&so_main, "net_local_ip_string");
  if (!local_ip_addr) return;

  u32 ip = 0;
  Result rc = nifmInitialize(NifmServiceType_User);
  if (R_SUCCEEDED(rc)) {
    nifmGetCurrentIpAddress(&ip);
    nifmExit();
  }
  if (ip == 0) {
    ip = (u32)gethostid();
  }

  if (ip != 0) {
    struct in_addr in;
    in.s_addr = ip;
    inet_ntop(AF_INET, &in, (char *)local_ip_addr, 256);
  } else {
    strncpy((char *)local_ip_addr, "127.0.0.1", 256);
  }
  debugPrintf("[Network] Local IP set to '%s' (addr=%p)\n", (char *)local_ip_addr, (void *)local_ip_addr);
}

static void InitLocalIp_hook(void) {
  // net_local_ip_string was already populated at startup in patch_game().
  // Doing nothing here prevents calling so_find_addr on unmapped memory and nifmExit at runtime!
  debugPrintf("InitLocalIp_hook called (noop, using pre-set IP)\n");
}

static void WriteLogEntry_hook(const char *fmt, ...) {
  char buf[2048];
  va_list list;
  va_start(list, fmt);
  vsnprintf(buf, sizeof(buf), fmt, list);
  va_end(list);

  debugPrintf("[RVGL] %s\n", buf);
}

void patch_game(void) {
  // Register console suspend / focus pause hook
  appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
  appletHook(&s_appletHookCookie, onAppletHook, NULL);
  debugPrintf("Registered applet suspend/focus hook\n");

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

  // Auto-assign controllers to player slots in GetProfileSettings:
  // Changes `str w26, [x27, #4]` (where w26 is -1) to `str w28, [x27, #4]` (where w28 is player index 0..3)
  uintptr_t get_prof_settings = so_find_addr(&so_main, "_Z18GetProfileSettingsP11PROFILEINFO");
  if (get_prof_settings) {
    uint32_t *instr = (uint32_t *)(get_prof_settings + 0x16c);
    if (*instr == 0xb900077a) {
      *instr = 0xb900077c;
      debugPrintf("Patched default controller assignment in GetProfileSettings\n");
    } else {
      debugPrintf("Warning: instruction at GetProfileSettings + 0x16c is 0x%08x (expected 0xb900077a)\n", *instr);
    }
  }

  // Pre-set local IP in net_local_ip_string
  set_switch_local_ip();

  // Hook internal local IP query at 0x158b5c
  uintptr_t init_net = so_find_addr(&so_main, "_Z11InitNetworkb");
  if (init_net) {
    uintptr_t func_158b5c = init_net - 0x15e660 + 0x158b5c;
    hook_arm64(func_158b5c, (uintptr_t)InitLocalIp_hook);
    debugPrintf("Hooked InitLocalIp at %p\n", (void *)func_158b5c);
  }

  // Prevent HandleWindowEventsv from auto-unpausing when FOCUS_GAINED arrives on console wake-up.
  // This leaves the game safely paused in the Pause Menu until the player selects Resume.
  uintptr_t handle_win_events = so_find_addr(&so_main, "_Z18HandleWindowEventsv");
  if (handle_win_events) {
    uint32_t *unpause_instr = (uint32_t *)(handle_win_events + 0x534);
    uint32_t *resume_sfx_instr = (uint32_t *)(handle_win_events + 0x538);
    if (*unpause_instr == 0x3900d01f) {
      *unpause_instr = 0xd503201f;   // nop
      *resume_sfx_instr = 0xd503201f; // nop
      debugPrintf("Patched HandleWindowEventsv to prevent auto-unpause on FOCUS_GAINED\n");
    } else {
      debugPrintf("Warning: HandleWindowEventsv + 0x534 is 0x%08x (expected 0x3900d01f)\n", *unpause_instr);
    }
  }

  // Hook DrawHostComputer and DrawEnterName draw callbacks in .data
  uintptr_t expected_host = so_try_find_addr_rx(&so_main, "_Z16DrawHostComputerii");
  uintptr_t expected_enter = so_try_find_addr_rx(&so_main, "_Z13DrawEnterNameii");

  uintptr_t *draw_host_ptr = (uintptr_t *)((uintptr_t)so_main.load_base + 0x2f4138);
  uintptr_t *draw_enter_ptr = (uintptr_t *)((uintptr_t)so_main.load_base + 0x2f6038);

  if (*draw_host_ptr != expected_host) {
    for (int off = -0x100; off <= 0x100; off += 8) {
      uintptr_t *p = (uintptr_t *)((uintptr_t)so_main.load_base + 0x2f4138 + off);
      if (*p == expected_host) {
        draw_host_ptr = p;
        break;
      }
    }
  }

  if (*draw_enter_ptr != expected_enter) {
    for (int off = -0x100; off <= 0x100; off += 8) {
      uintptr_t *p = (uintptr_t *)((uintptr_t)so_main.load_base + 0x2f6038 + off);
      if (*p == expected_enter) {
        draw_enter_ptr = p;
        break;
      }
    }
  }

  if (*draw_host_ptr == expected_host) {
    orig_DrawHostComputer = (void (*)(int, int))*draw_host_ptr;
    *draw_host_ptr = (uintptr_t)&DrawHostComputer_hook;
    debugPrintf("Hooked DrawHostComputer in .data at %p (orig=%p, hook=%p)\n",
                draw_host_ptr, orig_DrawHostComputer, DrawHostComputer_hook);
  } else {
    debugPrintf("WARNING: could not locate DrawHostComputer in .data (expected %p, found %p)\n",
                (void *)expected_host, (void *)*draw_host_ptr);
  }

  if (*draw_enter_ptr == expected_enter) {
    orig_DrawEnterName = (void (*)(int, int))*draw_enter_ptr;
    *draw_enter_ptr = (uintptr_t)&DrawEnterName_hook;
    debugPrintf("Hooked DrawEnterName in .data at %p (orig=%p, hook=%p)\n",
                draw_enter_ptr, orig_DrawEnterName, DrawEnterName_hook);
  } else {
    debugPrintf("WARNING: could not locate DrawEnterName in .data (expected %p, found %p)\n",
                (void *)expected_enter, (void *)*draw_enter_ptr);
  }
}

void unpatch_game(void) {
  static bool unpatched = false;
  if (unpatched) return;
  unpatched = true;
  appletUnhook(&s_appletHookCookie);
  debugPrintf("unpatch_game: unhooked applet hook\n");
}

