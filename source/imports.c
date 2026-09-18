/* imports.c -- .so import resolution for RVGL on Nintendo Switch
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <locale.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <mpg123.h>

#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "imports.h"
#include "hooks.h"

extern uintptr_t __stack_chk_fail;

static char *__ctype_ = (char *)&_ctype_;

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

FILE *stderr_fake = (FILE *)&fake_sF[2];

void __assert2(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
  assert(0);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list list;
  char string[1024];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("%s: %s\n", tag, string);
  return 0;
}

int __isnanf_fake(float x) {
  return isnan(x);
}

// Android SDL stubs
static const char *SDL_AndroidGetExternalStoragePath_fake(void) {
  return "/switch/rvgl";
}

static int SDL_AndroidRequestPermission_fake(const char *permission) {
  (void)permission;
  return 1;
}

extern int screen_width;
extern int screen_height;

static SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y, int w, int h, Uint32 flags) {
  if (w <= 0 || h <= 0) {
    w = screen_width;
    h = screen_height;
  }
  debugPrintf("SDL_CreateWindow: title='%s', requested=%dx%d, flags=0x%08x\n", title, w, h, flags);
  flags |= SDL_WINDOW_OPENGL;
  SDL_Window *win = SDL_CreateWindow(title, x, y, w, h, flags);
  debugPrintf("SDL_CreateWindow -> %p (error: %s)\n", win, SDL_GetError());
  return win;
}

static SDL_GLContext SDL_GL_CreateContext_hook(SDL_Window *window) {
  debugPrintf("SDL_GL_CreateContext for window %p\n", window);
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  debugPrintf("SDL_GL_CreateContext -> %p (error: %s)\n", ctx, SDL_GetError());
  return ctx;
}

static char *SDL_GetBasePath_hook(void) {
  struct stat st;
  if (stat("/switch/rvgl/assets/models/go2.m", &st) == 0 || stat("assets/models/go2.m", &st) == 0) {
    return SDL_strdup("/switch/rvgl/assets/");
  }
  return SDL_strdup("/switch/rvgl/");
}

static char *SDL_GetPrefPath_hook(const char *org, const char *app) {
  (void)org; (void)app;
  return SDL_strdup("/switch/rvgl/");
}

// OpenAL hooks
ALCcontext *al_ctx = NULL;
ALCdevice *al_dev = NULL;

static ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *attrList) {
  debugPrintf("alcCreateContextHook(dev=%p, attrList=%p)\n", dev, attrList);
  al_ctx = alcCreateContext(dev, attrList);
  if (!al_ctx) {
    debugPrintf("alcCreateContext with attrList failed, trying NULL\n");
    al_ctx = alcCreateContext(dev, NULL);
  }
  debugPrintf("alcCreateContext -> %p\n", al_ctx);
  if (al_ctx) {
    alcMakeContextCurrent(al_ctx);
    alcSetThreadContext(al_ctx);
  }
  return al_ctx;
}

static ALCdevice *alcOpenDeviceHook(const char *name) {
  al_dev = alcOpenDevice(name);
  debugPrintf("alcOpenDevice(%s) -> %p\n", name ? name : "(default)", al_dev);
  return al_dev;
}

static void alcDestroyContextHook(ALCcontext *context) {
  debugPrintf("alcDestroyContextHook(%p)\n", context);
  if (context == al_ctx) {
    al_ctx = NULL;
  }
  alcDestroyContext(context);
}

static ALCboolean alcCloseDeviceHook(ALCdevice *device) {
  debugPrintf("alcCloseDeviceHook(%p)\n", device);
  if (device == al_dev) {
    al_dev = NULL;
  }
  return alcCloseDevice(device);
}

static void *alcGetProcAddress_hook(ALCdevice *dev, const ALCchar *funcname) {
  void *res = alcGetProcAddress(dev, funcname);
  if (!res && funcname) {
    if (strcmp(funcname, "alcSetThreadContext") == 0) return (void *)&alcSetThreadContext;
    if (strcmp(funcname, "alcGetThreadContext") == 0) return (void *)&alcGetThreadContext;
  }
  return res;
}

static int SDL_GL_SetSwapInterval_hook(int interval) {
  debugPrintf("SDL_GL_SetSwapInterval(%d)\n", interval);
  int res = SDL_GL_SetSwapInterval(interval);
  debugPrintf("SDL_GL_SetSwapInterval returned %d (error: %s)\n", res, SDL_GetError());
  return res;
}


static int dl_iterate_phdr_fake(int (*callback)(void *info, size_t size, void *data), void *data) {
  return so_dl_iterate_phdr(callback, data);
}

struct bionic_iovec {
  void *iov_base;
  size_t iov_len;
};

static ssize_t writev_fake(int fd, const struct bionic_iovec *iov, int iovcnt) {
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (r < 0) return r;
    total += r;
  }
  return total;
}

// pthread wrappers for Bionic struct compatibility
int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  int ret = pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  if (ret < 0) {
    free(m);
    return -1;
  }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (!uid) return -1;
  if (!*uid || (uintptr_t)*uid <= 0x8000) {
    pthread_mutex_t *m = NULL;
    pthread_mutex_init_fake(&m, NULL);
    if (!__sync_bool_compare_and_swap(uid, 0, m)) {
      if ((uintptr_t)*uid <= 0x8000) {
        *uid = m;
      } else {
        pthread_mutex_destroy(m);
        free(m);
      }
    }
  }
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  if (!uid) return -1;
  if (!*uid || (uintptr_t)*uid <= 0x8000) {
    pthread_mutex_t *m = NULL;
    pthread_mutex_init_fake(&m, NULL);
    if (!__sync_bool_compare_and_swap(uid, 0, m)) {
      if ((uintptr_t)*uid <= 0x8000) {
        *uid = m;
      } else {
        pthread_mutex_destroy(m);
        free(m);
      }
    }
  }
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  *c = PTHREAD_COND_INITIALIZER;
  int ret = pthread_cond_init(c, NULL);
  if (ret < 0) {
    free(c);
    return -1;
  }
  *cnd = c;
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0)
    return -1;
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0)
    return -1;
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0)
    return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0)
    return -1;
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

static FILE *freopen_hook(const char *path, const char *mode, FILE *stream) {
  debugPrintf("freopen(path='%s', mode='%s', stream=%p [stdout=%p, stderr=%p])\n",
              path ? path : "(null)", mode ? mode : "(null)", stream, stdout, stderr);
  if (stream == stdout || stream == stderr) {
    FILE *f = fopen(path, mode);
    return f ? f : stream;
  }
  return freopen(path, mode, stream);
}

bool s_textInputActive = false;
bool s_inSwkbd = false;

static void trigger_swkbd(void) {
  if (s_inSwkbd) return;
  s_inSwkbd = true;
  debugPrintf("trigger_swkbd launching...\n");

  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    swkbdConfigMakePresetDefault(&kbd);
    char out_text[256] = {0};
    rc = swkbdShow(&kbd, out_text, sizeof(out_text));
    swkbdClose(&kbd);
    if (R_SUCCEEDED(rc) && out_text[0] != '\0') {
      debugPrintf("swkbd returned: '%s'\n", out_text);
      apply_text_input(out_text);
    }
  }

  // Drain any lingering button presses (especially button A from clicking OK in swkbd)
  // so they do not leak into RVGL and trigger accidental "Connect" or menu advance.
  Uint32 flush_start = SDL_GetTicks();
  while (SDL_GetTicks() - flush_start < 250) {
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    svcSleepThread(10000000ULL); // 10ms
  }

  touch_text_field();
  s_inSwkbd = false;
}

static void SDL_StartTextInput_hook(void) {
  debugPrintf("SDL_StartTextInput_hook called\n");
  s_textInputActive = true;
  // Do NOT open swkbd automatically; wait for user to press (X) or (Y)
}

static void SDL_StopTextInput_hook(void) {
  debugPrintf("SDL_StopTextInput_hook called\n");
  s_textInputActive = false;
  SDL_StopTextInput();
}

static int SDL_PollEvent_hook(SDL_Event *event) {
  // If the text field has stopped being drawn (user left text input screen),
  // immediately deactivate text input mode.
  if (s_textInputActive && !is_text_field_active()) {
    s_textInputActive = false;
  }

  int ret = SDL_PollEvent(event);
  if (ret) {
    if (event) {
      // If text input is active AND the text field is currently visible on screen,
      // and user presses (X) or (Y) on controller, open swkbd on demand:
      // In SDL2 on Switch: BUTTON_X is West button (Switch Y), BUTTON_Y is North button (Switch X).
      if (s_textInputActive && is_text_field_active() && !s_inSwkbd) {
        if (event->type == SDL_CONTROLLERBUTTONDOWN &&
            (event->cbutton.button == SDL_CONTROLLER_BUTTON_X || event->cbutton.button == SDL_CONTROLLER_BUTTON_Y)) {
          trigger_swkbd();
          memset(event, 0, sizeof(*event));
          return SDL_PollEvent(event);
        }
      }

      // If user presses Backspace on physical keyboard:
      // Turn SDL_KEYDOWN with Backspace into SDL_TEXTINPUT with '\b'
      // so repeat keystrokes and single presses reliably delete characters in RVGL!
      if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
        event->type = SDL_TEXTINPUT;
        event->text.type = SDL_TEXTINPUT;
        event->text.text[0] = '\b';
        event->text.text[1] = '\0';
      }
      // If user presses Return on physical keyboard:
      if (event->type == SDL_KEYDOWN &&
          (event->key.keysym.scancode == SDL_SCANCODE_RETURN || event->key.keysym.scancode == SDL_SCANCODE_KP_ENTER)) {
        event->type = SDL_TEXTINPUT;
        event->text.type = SDL_TEXTINPUT;
        event->text.text[0] = '\r';
        event->text.text[1] = '\0';
      }
    }
    return 1;
  }

  return 0;
}

static int *__errno_hook(void) {
  int *real_err = __errno();
  if (real_err) {
    switch (*real_err) {
      case 112: *real_err = 98;  break; // EADDRINUSE (Newlib 112 -> Linux 98)
      case 116: *real_err = 110; break; // ETIMEDOUT (Newlib 116 -> Linux 110)
      case 119: *real_err = 115; break; // EINPROGRESS (Newlib 119 -> Linux 115)
      case 120: *real_err = 114; break; // EALREADY (Newlib 120 -> Linux 114)
      case 127: *real_err = 106; break; // EISCONN (Newlib 127 -> Linux 106)
      case 128: *real_err = 107; break; // ENOTCONN (Newlib 128 -> Linux 107)
      default: break;
    }
  }
  return real_err;
}

static int SDL_NumJoysticks_hook(void) {
  int count = SDL_NumJoysticks();
  debugPrintf("SDL_NumJoysticks() -> %d\n", count);
  return count;
}

static SDL_Joystick *SDL_JoystickOpen_hook(int device_index) {
  SDL_Joystick *joy = SDL_JoystickOpen(device_index);
  debugPrintf("SDL_JoystickOpen(%d) -> %p (%s)\n", device_index, joy, SDL_JoystickNameForIndex(device_index));
  return joy;
}

static SDL_GameController *SDL_GameControllerOpen_hook(int joystick_index) {
  SDL_GameController *pad = SDL_GameControllerOpen(joystick_index);
  debugPrintf("SDL_GameControllerOpen(%d) -> %p (%s)\n", joystick_index, pad, SDL_GameControllerNameForIndex(joystick_index));
  return pad;
}

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadWrapperArgs;

static void *thread_wrapper_func(void *param) {
  ThreadWrapperArgs *t = (ThreadWrapperArgs *)param;
  void *(*entry)(void *) = t->entry;
  void *arg = t->arg;
  free(t);

  debugPrintf("[Thread %p] started execution\n", (void *)pthread_self());
  if (al_ctx) {
    alcMakeContextCurrent(al_ctx);
    alcSetThreadContext(al_ctx);
  }
  void *ret = entry(arg);
  debugPrintf("[Thread %p] returned cleanly with %p\n", (void *)pthread_self(), ret);
  return ret;
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  debugPrintf("pthread_create: starting thread %p with 2MB stack\n", entry);
  ThreadWrapperArgs *targs = malloc(sizeof(*targs));
  targs->entry = (void *(*)(void *))entry;
  targs->arg = arg;
  int res = pthread_create(thread, &a, thread_wrapper_func, targs);
  debugPrintf("pthread_create -> %d (thread %p)\n", res, thread ? (void *)*thread : NULL);
  return res;
}

static int pthread_join_hook(pthread_t thread, void **retval) {
  debugPrintf("pthread_join(thread=%p) called\n", (void *)thread);
  int res = pthread_join(thread, retval);
  debugPrintf("pthread_join(thread=%p) returned %d\n", (void *)thread, res);
  return res;
}

typedef struct {
  int (*fn)(void *);
  void *data;
} SDLThreadWrapperArgs;

static int sdl_thread_wrapper(void *param) {
  SDLThreadWrapperArgs *t = (SDLThreadWrapperArgs *)param;
  int (*fn)(void *) = t->fn;
  void *data = t->data;
  free(t);

  debugPrintf("[SDL Thread %p] started execution\n", (void *)pthread_self());
  int ret = fn(data);
  debugPrintf("[SDL Thread %p] returned cleanly with %d\n", (void *)pthread_self(), ret);
  return ret;
}

static SDL_Thread *SDL_CreateThread_hook(SDL_ThreadFunction fn, const char *name, void *data) {
  debugPrintf("SDL_CreateThread('%s', fn=%p, data=%p)\n", name ? name : "unnamed", fn, data);
  if (name && (strcmp(name, "IP Thread") == 0 || strstr(name, "IP") != NULL)) {
    debugPrintf("SDL_CreateThread_hook: blocked '%s' to avoid exit hang\n", name);
    return NULL;
  }
  SDLThreadWrapperArgs *t = malloc(sizeof(*t));
  t->fn = fn;
  t->data = data;
  return SDL_CreateThreadWithStackSize(sdl_thread_wrapper, name, 2 * 1024 * 1024, t);
}

static uint32_t s_swapCount = 0;
static uint64_t s_lastFpsTime = 0;
static uint32_t s_fpsFrames = 0;

static void SDL_GL_SwapWindow_hook(SDL_Window *window) {
  s_swapCount++;
  s_fpsFrames++;
  uint64_t now = armGetSystemTick();
  uint64_t freq = armGetSystemTickFreq();
  if (s_lastFpsTime == 0) {
    s_lastFpsTime = now;
  } else if (now - s_lastFpsTime >= freq) {
    float fps = (float)s_fpsFrames * freq / (float)(now - s_lastFpsTime);
    debugPrintf("[FPS] %.1f fps (frame %u, res %dx%d)\n", fps, s_swapCount, screen_width, screen_height);
    s_fpsFrames = 0;
    s_lastFpsTime = now;
  }
  SDL_GL_SwapWindow(window);
}

// DynLib imports table
DynLibFunction dynlib_functions[] = {
  { "IMG_Init", (uintptr_t)&IMG_Init },
  { "IMG_Load_RW", (uintptr_t)&IMG_Load_RW },
  { "IMG_Quit", (uintptr_t)&IMG_Quit },
  { "IMG_SavePNG_RW", (uintptr_t)&IMG_SavePNG_RW },
  { "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath_fake },
  { "SDL_AndroidRequestPermission", (uintptr_t)&SDL_AndroidRequestPermission_fake },
  { "SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat },
  { "SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex },
  { "SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface },
  { "SDL_CreateSemaphore", (uintptr_t)&SDL_CreateSemaphore },
  { "SDL_CreateSystemCursor", (uintptr_t)&SDL_CreateSystemCursor },
  { "SDL_CreateThread", (uintptr_t)&SDL_CreateThread_hook },
  { "SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow_hook },
  { "SDL_Delay", (uintptr_t)&SDL_Delay },
  { "SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex },
  { "SDL_DestroySemaphore", (uintptr_t)&SDL_DestroySemaphore },
  { "SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow },
  { "SDL_DetachThread", (uintptr_t)&SDL_DetachThread },
  { "SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface },
  { "SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext_hook },
  { "SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext },
  { "SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute },
  { "SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize },
  { "SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress },
  { "SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent },
  { "SDL_GL_ResetAttributes", (uintptr_t)&SDL_GL_ResetAttributes },
  { "SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute },
  { "SDL_GL_SetSwapInterval", (uintptr_t)&SDL_GL_SetSwapInterval_hook },
  { "SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow_hook },
  { "SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW },
  { "SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose },
  { "SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis },
  { "SDL_GameControllerGetButton", (uintptr_t)&SDL_GameControllerGetButton },
  { "SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick },
  { "SDL_GameControllerMappingForGUID", (uintptr_t)&SDL_GameControllerMappingForGUID },
  { "SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex },
  { "SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen_hook },
  { "SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath_hook },
  { "SDL_GetClipboardText", (uintptr_t)&SDL_GetClipboardText },
  { "SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode },
  { "SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode },
  { "SDL_GetError", (uintptr_t)&SDL_GetError },
  { "SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode },
  { "SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState },
  { "SDL_GetModState", (uintptr_t)&SDL_GetModState },
  { "SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes },
  { "SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter },
  { "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
  { "SDL_GetPlatform", (uintptr_t)&SDL_GetPlatform },
  { "SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath_hook },
  { "SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName },
  { "SDL_GetVersion", (uintptr_t)&SDL_GetVersion },
  { "SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize },
  { "SDL_HapticClose", (uintptr_t)&SDL_HapticClose },
  { "SDL_HapticDestroyEffect", (uintptr_t)&SDL_HapticDestroyEffect },
  { "SDL_HapticIndex", (uintptr_t)&SDL_HapticIndex },
  { "SDL_HapticName", (uintptr_t)&SDL_HapticName },
  { "SDL_HapticNewEffect", (uintptr_t)&SDL_HapticNewEffect },
  { "SDL_HapticNumAxes", (uintptr_t)&SDL_HapticNumAxes },
  { "SDL_HapticNumEffects", (uintptr_t)&SDL_HapticNumEffects },
  { "SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick },
  { "SDL_HapticQuery", (uintptr_t)&SDL_HapticQuery },
  { "SDL_HapticRunEffect", (uintptr_t)&SDL_HapticRunEffect },
  { "SDL_HapticSetAutocenter", (uintptr_t)&SDL_HapticSetAutocenter },
  { "SDL_HapticSetGain", (uintptr_t)&SDL_HapticSetGain },
  { "SDL_HapticUpdateEffect", (uintptr_t)&SDL_HapticUpdateEffect },
  { "SDL_HasClipboardText", (uintptr_t)&SDL_HasClipboardText },
  { "SDL_Init", (uintptr_t)&SDL_Init },
  { "SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem },
  { "SDL_IsGameController", (uintptr_t)&SDL_IsGameController },
  { "SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive },
  { "SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose },
  { "SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis },
  { "SDL_JoystickGetButton", (uintptr_t)&SDL_JoystickGetButton },
  { "SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID },
  { "SDL_JoystickGetGUIDString", (uintptr_t)&SDL_JoystickGetGUIDString },
  { "SDL_JoystickIsHaptic", (uintptr_t)&SDL_JoystickIsHaptic },
  { "SDL_JoystickNameForIndex", (uintptr_t)&SDL_JoystickNameForIndex },
  { "SDL_JoystickNumAxes", (uintptr_t)&SDL_JoystickNumAxes },
  { "SDL_JoystickNumButtons", (uintptr_t)&SDL_JoystickNumButtons },
  { "SDL_JoystickNumHats", (uintptr_t)&SDL_JoystickNumHats },
  { "SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen_hook },
  { "SDL_LockMutex", (uintptr_t)&SDL_LockMutex },
  { "SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks_hook },
  { "SDL_PollEvent", (uintptr_t)&SDL_PollEvent_hook },
  { "SDL_Quit", (uintptr_t)&SDL_Quit },
  { "SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile },
  { "SDL_RWclose", (uintptr_t)&SDL_RWclose },
  { "SDL_RWread", (uintptr_t)&SDL_RWread },
  { "SDL_SemPost", (uintptr_t)&SDL_SemPost },
  { "SDL_SemWait", (uintptr_t)&SDL_SemWait },
  { "SDL_SetClipboardText", (uintptr_t)&SDL_SetClipboardText },
  { "SDL_SetCursor", (uintptr_t)&SDL_SetCursor },
  { "SDL_SetHint", (uintptr_t)&SDL_SetHint },
  { "SDL_SetRelativeMouseMode", (uintptr_t)&SDL_SetRelativeMouseMode },
  { "SDL_SetWindowBrightness", (uintptr_t)&SDL_SetWindowBrightness },
  { "SDL_SetWindowGammaRamp", (uintptr_t)&SDL_SetWindowGammaRamp },
  { "SDL_SetWindowIcon", (uintptr_t)&SDL_SetWindowIcon },
  { "SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox },
  { "SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox },
  { "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput_hook },
  { "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput_hook },
  { "SDL_ThreadID", (uintptr_t)&SDL_ThreadID },
  { "SDL_TryLockMutex", (uintptr_t)&SDL_TryLockMutex },
  { "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
  { "SDL_WaitThread", (uintptr_t)&SDL_WaitThread },
  { "SDL_free", (uintptr_t)&SDL_free },
  { "SDL_setenv", (uintptr_t)&SDL_setenv },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&ret0 },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno_hook },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
  { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
  { "__isnanf", (uintptr_t)&__isnanf_fake },
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "_ctype_", (uintptr_t)&__ctype_ },
  { "abort", (uintptr_t)&abort },
  { "accept", (uintptr_t)&accept },
  { "access", (uintptr_t)&access },
  { "acos", (uintptr_t)&acos },
  { "alBufferData", (uintptr_t)&alBufferData },
  { "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
  { "alDeleteSources", (uintptr_t)&alDeleteSources },
  { "alDistanceModel", (uintptr_t)&alDistanceModel },
  { "alGenBuffers", (uintptr_t)&alGenBuffers },
  { "alGenSources", (uintptr_t)&alGenSources },
  { "alGetEnumValue", (uintptr_t)&alGetEnumValue },
  { "alGetError", (uintptr_t)&alGetError },
  { "alGetSourcei", (uintptr_t)&alGetSourcei },
  { "alGetString", (uintptr_t)&alGetString },
  { "alIsBuffer", (uintptr_t)&alIsBuffer },
  { "alIsExtensionPresent", (uintptr_t)&alIsExtensionPresent },
  { "alIsSource", (uintptr_t)&alIsSource },
  { "alSource3f", (uintptr_t)&alSource3f },
  { "alSourcePause", (uintptr_t)&alSourcePause },
  { "alSourcePlay", (uintptr_t)&alSourcePlay },
  { "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
  { "alSourceStop", (uintptr_t)&alSourceStop },
  { "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
  { "alSourcef", (uintptr_t)&alSourcef },
  { "alSourcei", (uintptr_t)&alSourcei },
  { "alcCloseDevice", (uintptr_t)&alcCloseDeviceHook },
  { "alcCreateContext", (uintptr_t)&alcCreateContextHook },
  { "alcDestroyContext", (uintptr_t)&alcDestroyContextHook },
  { "alcGetContextsDevice", (uintptr_t)&alcGetContextsDevice },
  { "alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext },
  { "alcGetError", (uintptr_t)&alcGetError },
  { "alcGetIntegerv", (uintptr_t)&alcGetIntegerv },
  { "alcGetProcAddress", (uintptr_t)&alcGetProcAddress_hook },
  { "alcGetString", (uintptr_t)&alcGetString },
  { "alcIsExtensionPresent", (uintptr_t)&alcIsExtensionPresent },
  { "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
  { "alcOpenDevice", (uintptr_t)&alcOpenDeviceHook },
  { "alcSetThreadContext", (uintptr_t)&alcSetThreadContext },
  { "atan2", (uintptr_t)&atan2 },
  { "atof", (uintptr_t)&atof },
  { "atol", (uintptr_t)&atol },
  { "bind", (uintptr_t)&bind_fake },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&calloc },
  { "chdir", (uintptr_t)&chdir },
  { "close", (uintptr_t)&close },
  { "closedir", (uintptr_t)&closedir },
  { "connect", (uintptr_t)&connect_fake },
  { "cos", (uintptr_t)&cos },
  { "dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake },
  { "fclose", (uintptr_t)&fclose },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror },
  { "fflush", (uintptr_t)&fflush },
  { "fgetc", (uintptr_t)&fgetc },
  { "fgetpos", (uintptr_t)&fgetpos },
  { "fgets", (uintptr_t)&fgets },
  { "fileno", (uintptr_t)&fileno },
  { "fmod", (uintptr_t)&fmod },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf },
  { "fputc", (uintptr_t)&fputc },
  { "fputs", (uintptr_t)&fputs },
  { "fread", (uintptr_t)&fread },
  { "free", (uintptr_t)&free },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
  { "freopen", (uintptr_t)&freopen_hook },
  { "frexp", (uintptr_t)&frexp },
  { "fscanf", (uintptr_t)&fscanf },
  { "fseek", (uintptr_t)&fseek },
  { "fseeko", (uintptr_t)&fseeko },
  { "fsetpos", (uintptr_t)&fsetpos },
  { "fstat", (uintptr_t)&fstat_fake },
  { "fsync", (uintptr_t)&fsync },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "fwrite", (uintptr_t)&fwrite },
  { "getaddrinfo", (uintptr_t)&getaddrinfo },
  { "getc", (uintptr_t)&getc },
  { "getc_unlocked", (uintptr_t)&getc_unlocked },
  { "getenv", (uintptr_t)&getenv },
  { "getnameinfo", (uintptr_t)&getnameinfo },
  { "getsockname", (uintptr_t)&getsockname_fake },
  { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "getwc", (uintptr_t)&getwc },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "inet_ntop", (uintptr_t)&inet_ntop },
  { "inet_pton", (uintptr_t)&inet_pton },
  { "ioctl", (uintptr_t)&retm1 },
  { "isalpha", (uintptr_t)&isalpha },
  { "isprint", (uintptr_t)&isprint },
  { "isspace", (uintptr_t)&isspace },
  { "iswcntrl", (uintptr_t)&iswcntrl },
  { "iswctype", (uintptr_t)&iswctype },
  { "link", (uintptr_t)&retm1 },
  { "listen", (uintptr_t)&listen },
  { "localtime", (uintptr_t)&localtime },
  { "log", (uintptr_t)&log },
  { "lrint", (uintptr_t)&lrint },
  { "lrintf", (uintptr_t)&lrintf },
  { "lround", (uintptr_t)&lround },
  { "lseek", (uintptr_t)&lseek },
  { "malloc", (uintptr_t)&malloc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbsinit", (uintptr_t)&mbsinit },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "mkdir", (uintptr_t)&mkdir },
  { "mpg123_decode", (uintptr_t)&mpg123_decode },
  { "mpg123_delete", (uintptr_t)&mpg123_delete },
  { "mpg123_exit", (uintptr_t)&mpg123_exit },
  { "mpg123_feed", (uintptr_t)&mpg123_feed },
  { "mpg123_format", (uintptr_t)&mpg123_format },
  { "mpg123_format_none", (uintptr_t)&mpg123_format_none },
  { "mpg123_getformat", (uintptr_t)&mpg123_getformat },
  { "mpg123_init", (uintptr_t)&mpg123_init },
  { "mpg123_new", (uintptr_t)&mpg123_new },
  { "mpg123_open_feed", (uintptr_t)&mpg123_open_feed },
  { "mpg123_read", (uintptr_t)&mpg123_read },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "open", (uintptr_t)&open_fake },
  { "opendir", (uintptr_t)&opendir },
  { "poll", (uintptr_t)&poll },
  { "pow", (uintptr_t)&pow },
  { "printf", (uintptr_t)&debugPrintf },
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_join", (uintptr_t)&pthread_join_hook },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_mutexattr_init", (uintptr_t)&ret0 },
  { "pthread_mutexattr_settype", (uintptr_t)&ret0 },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "putc", (uintptr_t)&putc },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "putwc", (uintptr_t)&putwc },
  { "qsort", (uintptr_t)&qsort },
  { "read", (uintptr_t)&read },
  { "readdir", (uintptr_t)&readdir_fake },
  { "realloc", (uintptr_t)&realloc },
  { "recvmsg", (uintptr_t)&recvmsg_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "rewind", (uintptr_t)&rewind },
  { "rmdir", (uintptr_t)&rmdir },
  { "select", (uintptr_t)&select },
  { "sendmsg", (uintptr_t)&sendmsg_fake },
  { "setlocale", (uintptr_t)&setlocale },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "shutdown", (uintptr_t)&shutdown },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "snprintf", (uintptr_t)&snprintf },
  { "socket", (uintptr_t)&socket },
  { "sprintf", (uintptr_t)&sprintf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "sscanf", (uintptr_t)&sscanf },
  { "stat", (uintptr_t)&stat_fake },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strnlen", (uintptr_t)&strnlen },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "syscall", (uintptr_t)&syscall_fake },
  { "time", (uintptr_t)&time },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "ungetc", (uintptr_t)&ungetc },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "uselocale", (uintptr_t)&uselocale },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wctob", (uintptr_t)&wctob },
  { "wctype", (uintptr_t)&wctype },
  { "wcwidth", (uintptr_t)&wcwidth },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "write", (uintptr_t)&write },
  { "writev", (uintptr_t)&writev_fake },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
}
