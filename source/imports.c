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

extern uintptr_t __cxa_atexit;
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

static SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y, int w, int h, Uint32 flags) {
  debugPrintf("SDL_CreateWindow: title='%s', %dx%d, flags=0x%08x\n", title, w, h, flags);
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
static ALCcontext *al_ctx = NULL;
static ALCdevice *al_dev = NULL;

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

static void *alcGetProcAddress_hook(ALCdevice *dev, const ALCchar *funcname) {
  void *res = alcGetProcAddress(dev, funcname);
  if (!res && funcname) {
    if (strcmp(funcname, "alcSetThreadContext") == 0) return (void *)&alcSetThreadContext;
    if (strcmp(funcname, "alcGetThreadContext") == 0) return (void *)&alcGetThreadContext;
  }
  return res;
}

static void alSourcePlay_hook(ALuint source) {
  debugPrintf("alSourcePlay(source=%u)\n", source);
  alSourcePlay(source);
}

static void alSourcef_hook(ALuint source, ALenum param, ALfloat value) {
  if (param == AL_GAIN) {
    debugPrintf("alSourcef(source=%u, AL_GAIN, %f)\n", source, value);
  }
  alSourcef(source, param, value);
}

static void alBufferData_hook(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq) {
  alBufferData(buffer, format, data, size, freq);
  ALenum err = alGetError();
  if (err != AL_NO_ERROR) {
    debugPrintf("alBufferData(buf=%u, fmt=0x%x, size=%d, freq=%d) ERROR 0x%x\n", buffer, format, size, freq, err);
  }
}

static void alSourceQueueBuffers_hook(ALuint source, ALsizei nb, const ALuint *buffers) {
  alSourceQueueBuffers(source, nb, buffers);
  ALenum err = alGetError();
  if (err != AL_NO_ERROR) {
    debugPrintf("alSourceQueueBuffers(source=%u, nb=%d, buf0=%u) ERROR 0x%x\n", source, nb, nb > 0 && buffers ? buffers[0] : 0, err);
  } else {
    static int qcount = 0;
    if ((++qcount % 20) == 1) {
      debugPrintf("alSourceQueueBuffers(source=%u, nb=%d, buf0=%u) OK (count=%d)\n", source, nb, nb > 0 && buffers ? buffers[0] : 0, qcount);
    }
  }
}

static void alSourceUnqueueBuffers_hook(ALuint source, ALsizei nb, ALuint *buffers) {
  alSourceUnqueueBuffers(source, nb, buffers);
  ALenum err = alGetError();
  if (err != AL_NO_ERROR) {
    debugPrintf("alSourceUnqueueBuffers(source=%u, nb=%d) ERROR 0x%x\n", source, nb, err);
  }
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
  const int recursive = (mutexattr && *mutexattr == 1);
  *m = recursive ? PTHREAD_RECURSIVE_MUTEX_INITIALIZER : PTHREAD_MUTEX_INITIALIZER;
  int ret = pthread_mutex_init(m, NULL);
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
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
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

static PadState s_switchPad;
static bool s_switchPadInited = false;
static uint64_t s_prevKeys = 0;

typedef struct {
  uint64_t mask;
  SDL_Scancode scancode;
  SDL_Keycode keycode;
} SwitchKeyMap;

static const SwitchKeyMap s_keymap[] = {
  { HidNpadButton_AnyUp,                             SDL_SCANCODE_UP,     SDLK_UP },
  { HidNpadButton_AnyDown,                           SDL_SCANCODE_DOWN,   SDLK_DOWN },
  { HidNpadButton_AnyLeft,                           SDL_SCANCODE_LEFT,   SDLK_LEFT },
  { HidNpadButton_AnyRight,                          SDL_SCANCODE_RIGHT,  SDLK_RIGHT },
  { HidNpadButton_A,                                 SDL_SCANCODE_RETURN, SDLK_RETURN },
  { HidNpadButton_B,                                 SDL_SCANCODE_ESCAPE, SDLK_ESCAPE },
  { HidNpadButton_X,                                 SDL_SCANCODE_SPACE,  SDLK_SPACE },
  { HidNpadButton_Y,                                 SDL_SCANCODE_R,      SDLK_r },
  { HidNpadButton_Plus,                              SDL_SCANCODE_ESCAPE, SDLK_ESCAPE },
  { HidNpadButton_Minus,                             SDL_SCANCODE_TAB,    SDLK_TAB },
  { HidNpadButton_ZL | HidNpadButton_L,              SDL_SCANCODE_DOWN,   SDLK_DOWN },
  { HidNpadButton_ZR | HidNpadButton_R,              SDL_SCANCODE_UP,     SDLK_UP },
};
#define NUM_KEYMAPS (sizeof(s_keymap) / sizeof(s_keymap[0]))

#define MAX_SYNTH_EVENTS 32
static SDL_Event s_synthQueue[MAX_SYNTH_EVENTS];
static int s_synthHead = 0;
static int s_synthTail = 0;

static void queue_key_event(Uint32 type, SDL_Scancode scancode, SDL_Keycode sym) {
  int next = (s_synthTail + 1) % MAX_SYNTH_EVENTS;
  if (next == s_synthHead) return;
  SDL_Event *ev = &s_synthQueue[s_synthTail];
  memset(ev, 0, sizeof(*ev));
  ev->type = type;
  ev->key.type = type;
  ev->key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
  ev->key.repeat = 0;
  ev->key.keysym.scancode = scancode;
  ev->key.keysym.sym = sym;
  s_synthTail = next;
}

static uint64_t get_switch_pad_buttons(void) {
  if (!s_switchPadInited) {
    padInitializeAny(&s_switchPad);
    s_switchPadInited = true;
  }
  padUpdate(&s_switchPad);
  uint64_t curKeys = padGetButtons(&s_switchPad);
  HidAnalogStickState l_stick = padGetStickPos(&s_switchPad, 0);
  if (l_stick.x < -16000) curKeys |= HidNpadButton_StickLLeft;
  if (l_stick.x > 16000)  curKeys |= HidNpadButton_StickLRight;
  if (l_stick.y < -16000) curKeys |= HidNpadButton_StickLDown;
  if (l_stick.y > 16000)  curKeys |= HidNpadButton_StickLUp;
  return curKeys;
}

static Uint8 s_customKeyState[SDL_NUM_SCANCODES];

static const Uint8 *SDL_GetKeyboardState_hook(int *numkeys) {
  if (numkeys) *numkeys = SDL_NUM_SCANCODES;

  const Uint8 *real = SDL_GetKeyboardState(NULL);
  if (real) {
    memcpy(s_customKeyState, real, SDL_NUM_SCANCODES);
  } else {
    memset(s_customKeyState, 0, SDL_NUM_SCANCODES);
  }

  uint64_t curKeys = get_switch_pad_buttons();

  for (size_t i = 0; i < NUM_KEYMAPS; i++) {
    if (curKeys & s_keymap[i].mask) {
      s_customKeyState[s_keymap[i].scancode] = 1;
    }
  }

  return s_customKeyState;
}

static int SDL_PollEvent_hook(SDL_Event *event) {
  uint64_t curKeys = get_switch_pad_buttons();

  for (size_t i = 0; i < NUM_KEYMAPS; i++) {
    bool wasDown = (s_prevKeys & s_keymap[i].mask) != 0;
    bool isDown  = (curKeys   & s_keymap[i].mask) != 0;
    if (isDown && !wasDown) {
      queue_key_event(SDL_KEYDOWN, s_keymap[i].scancode, s_keymap[i].keycode);
    } else if (!isDown && wasDown) {
      queue_key_event(SDL_KEYUP, s_keymap[i].scancode, s_keymap[i].keycode);
    }
  }
  s_prevKeys = curKeys;

  if (s_synthHead != s_synthTail) {
    if (event) *event = s_synthQueue[s_synthHead];
    s_synthHead = (s_synthHead + 1) % MAX_SYNTH_EVENTS;
    return 1;
  }

  return SDL_PollEvent(event);
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
  if (al_ctx) {
    alcMakeContextCurrent(al_ctx);
    alcSetThreadContext(al_ctx);
  }
  int ret = fn(data);
  debugPrintf("[SDL Thread %p] returned cleanly with %d\n", (void *)pthread_self(), ret);
  return ret;
}

static SDL_Thread *SDL_CreateThread_hook(SDL_ThreadFunction fn, const char *name, void *data) {
  debugPrintf("SDL_CreateThread('%s', fn=%p, data=%p)\n", name ? name : "unnamed", fn, data);
  SDLThreadWrapperArgs *t = malloc(sizeof(*t));
  t->fn = fn;
  t->data = data;
  return SDL_CreateThreadWithStackSize(sdl_thread_wrapper, name, 2 * 1024 * 1024, t);
}

static uint32_t s_swapCount = 0;
static void SDL_GL_SwapWindow_hook(SDL_Window *window) {
  s_swapCount++;
  if ((s_swapCount % 300) == 0) {
    debugPrintf("[Heartbeat] SDL_GL_SwapWindow frame %u\n", s_swapCount);
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
  { "SDL_GL_SetSwapInterval", (uintptr_t)&SDL_GL_SetSwapInterval },
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
  { "SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState_hook },
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
  { "SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput },
  { "SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput },
  { "SDL_ThreadID", (uintptr_t)&SDL_ThreadID },
  { "SDL_TryLockMutex", (uintptr_t)&SDL_TryLockMutex },
  { "SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex },
  { "SDL_WaitThread", (uintptr_t)&SDL_WaitThread },
  { "SDL_free", (uintptr_t)&SDL_free },
  { "SDL_setenv", (uintptr_t)&SDL_setenv },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
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
  { "alBufferData", (uintptr_t)&alBufferData_hook },
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
  { "alSourcePlay", (uintptr_t)&alSourcePlay_hook },
  { "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers_hook },
  { "alSourceStop", (uintptr_t)&alSourceStop },
  { "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers_hook },
  { "alSourcef", (uintptr_t)&alSourcef_hook },
  { "alSourcei", (uintptr_t)&alSourcei },
  { "alcCloseDevice", (uintptr_t)&alcCloseDevice },
  { "alcCreateContext", (uintptr_t)&alcCreateContextHook },
  { "alcDestroyContext", (uintptr_t)&alcDestroyContext },
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
  { "bind", (uintptr_t)&bind },
  { "btowc", (uintptr_t)&btowc },
  { "calloc", (uintptr_t)&calloc },
  { "chdir", (uintptr_t)&chdir },
  { "close", (uintptr_t)&close },
  { "closedir", (uintptr_t)&closedir },
  { "connect", (uintptr_t)&connect },
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
  { "getsockname", (uintptr_t)&getsockname },
  { "getsockopt", (uintptr_t)&getsockopt },
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
  { "pthread_join", (uintptr_t)&pthread_join },
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
  { "recvmsg", (uintptr_t)&recvmsg },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "rewind", (uintptr_t)&rewind },
  { "rmdir", (uintptr_t)&rmdir },
  { "select", (uintptr_t)&select },
  { "sendmsg", (uintptr_t)&sendmsg },
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
