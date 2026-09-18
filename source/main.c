/* main.c
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module so_sndfile;
so_module so_unistring;
so_module so_main;

// provide replacement heap init function to separate newlib heap from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // only allocate a fixed amount for the newlib heap
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = umin(size, MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000); // align to page size
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}


static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0) {
    fatal_error("Could not find\n%s.\nCheck your /switch/rvgl installation.", SO_NAME);
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void read_ini_resolution(const char *path, int *w, int *h) {
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int val = 0;
    if (sscanf(line, " ScreenWidth = %d", &val) == 1 || sscanf(line, "ScreenWidth = %d", &val) == 1) {
      if (val > 0) *w = val;
    } else if (sscanf(line, " ScreenHeight = %d", &val) == 1 || sscanf(line, "ScreenHeight = %d", &val) == 1) {
      if (val > 0) *h = val;
    }
  }
  fclose(f);
}

static void set_screen_size(int w, int h) {
  bool is_docked = (appletGetOperationMode() == AppletOperationMode_Console);

  // Check user-configured ScreenWidth / ScreenHeight from profiles/rvgl.ini or rvgl.ini
  int ini_w = 0, ini_h = 0;
  read_ini_resolution("/switch/rvgl/profiles/rvgl.ini", &ini_w, &ini_h);
  if (ini_w <= 0 || ini_h <= 0) {
    read_ini_resolution("/switch/rvgl/rvgl.ini", &ini_w, &ini_h);
  }

  int target_w = (w > 0) ? w : ini_w;
  int target_h = (h > 0) ? h : ini_h;

  if (target_h == 480) {
    // 480p
    screen_width = (target_w > 0 && target_w <= 854) ? target_w : 854;
    screen_height = 480;
  } else if (!is_docked) {
    // In Handheld mode: native 720p
    screen_width = 1280;
    screen_height = 720;
  } else {
    // In Docked mode:
    if (target_h == 720) {
      screen_width = 1280;
      screen_height = 720;
    } else {
      screen_width = 1920;
      screen_height = 1080;
    }
  }

  debugPrintf("Switch mode: %s, Selected rendering resolution: %dx%d\n",
              is_docked ? "Docked" : "Handheld",
              screen_width, screen_height);
}

static void write_rvgl_ini(const char *path) {
  int default_w = (appletGetOperationMode() == AppletOperationMode_Console) ? 1920 : 1280;
  int default_h = (appletGetOperationMode() == AppletOperationMode_Console) ? 1080 : 720;
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f,
      "[Game]\n"
      "Language = 3\n"
      "\n"
      "[Video]\n"
      "ScreenWidth = %d\n"
      "ScreenHeight = %d\n"
      "Shaders = 1\n"
      "Threaded = 0\n"
      "\n"
      "[Audio]\n"
      "MusicOn = 1\n"
      "MusicVol = 100\n"
      "SfxVol = 100\n"
      "SfxChannels = 16\n"
      "SampleRate = 48000\n"
      "\n"
      "[Misc]\n"
      "DemoTimeout = 0\n",
      default_w, default_h
    );
    fclose(f);
    debugPrintf("Wrote config to %s\n", path);
  }
}

int main(int argc, char *argv[]) {
  // Ensure working directory is the game folder
  chdir("/switch/rvgl");

  // Create required directories if they don't exist
  mkdir("/switch/rvgl/profiles", 0777);
  mkdir("/switch/rvgl/profiles/default", 0777);
  mkdir("/switch/rvgl/replays", 0777);
  mkdir("/switch/rvgl/cache", 0777);
  mkdir("/switch/rvgl/cache/shaders", 0777);

  // Check and create default configs if missing
  struct stat st_ini;
  if (stat("/switch/rvgl/profiles/rvgl.ini", &st_ini) < 0) {
    write_rvgl_ini("/switch/rvgl/profiles/rvgl.ini");
  }
  if (stat("/switch/rvgl/rvgl.ini", &st_ini) < 0) {
    write_rvgl_ini("/switch/rvgl/rvgl.ini");
  }

  // Init default profile
  struct stat st_prof;
  if (stat("/switch/rvgl/profiles/default/profile.ini", &st_prof) < 0) {
    FILE *fp = fopen("/switch/rvgl/profiles/default/profile.ini", "w");
    if (fp) {
      fprintf(fp,
        "[Audio]\n"
        "MusicOn = 1\n"
        "MusicVol = 100\n"
        "SfxVol = 100\n"
        "SfxChannels = 16\n"
        "SampleRate = 48000\n"
        "\n"
        "[Controller1]\n"
        "ButtonOpacity = 0\n"
        "Joystick = 0\n"
        "KeyPause = 0x01ff0006\n" // map pause to start
        "\n"
        "[Joystick]\n"
        "Controller1 = 0\n"
      );
      fclose(fp);
    }
  }


  check_syscalls();
  check_data();

  set_screen_size(config.screen_width, config.screen_height);

  printf("heap size = %u KB\n", (u32)(MEMORY_MB * 1024));
  printf(" lib base = %p\n", heap_so_base);
  printf("  lib max = %u KB\n", (u32)(heap_so_limit / 1024));


  void *cur_so_base = heap_so_base;
  size_t cur_so_limit = heap_so_limit;

  // 1. Load libsndfile.so if available
  int has_sndfile = (so_load(&so_sndfile, "libsndfile.so", cur_so_base, cur_so_limit) == 0);
  if (has_sndfile) {
    printf("Loaded libsndfile.so\n");
    cur_so_base = (char *)cur_so_base + so_sndfile.load_size;
    cur_so_limit -= so_sndfile.load_size;
  } else {
    fprintf(stderr, "libsndfile.so not found (optional)\n");
  }

  // 2. Load libunistring.so if available
  int has_unistring = (so_load(&so_unistring, "libunistring.so", cur_so_base, cur_so_limit) == 0);
  if (has_unistring) {
    printf("Loaded libunistring.so\n");
    cur_so_base = (char *)cur_so_base + so_unistring.load_size;
    cur_so_limit -= so_unistring.load_size;
  } else {
    fprintf(stderr, "libunistring.so not found (optional)\n");
  }

  // 3. Load main game binary (libmain.so)
  if (so_load(&so_main, SO_NAME, cur_so_base, cur_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
  printf("Loaded %s\n", SO_NAME);

  // Relocate loaded modules
  printf("Relocating modules...\n");
  if (has_sndfile) so_relocate(&so_sndfile);
  if (has_unistring) so_relocate(&so_unistring);
  so_relocate(&so_main);

  // Resolve imports
  printf("Resolving imports...\n");
  if (has_sndfile) so_resolve(&so_sndfile, dynlib_functions, dynlib_numfunctions, 0);
  if (has_unistring) so_resolve(&so_unistring, dynlib_functions, dynlib_numfunctions, 0);
  so_resolve(&so_main, dynlib_functions, dynlib_numfunctions, 1);

  // Apply patches / hooks
  printf("Patching hooks...\n");
  patch_openal();
  patch_opengl();
  patch_game();

  // Find game entrypoint
  printf("Resolving SDL_main entrypoint...\n");
  int (*SDL_main_func)(int argc, char *argv[]) = (void *)so_find_addr_rx(&so_main, "SDL_main");
  if (!SDL_main_func)
    fatal_error("Could not find SDL_main in\n%s.", SO_NAME);
  printf("Found SDL_main at %p\n", SDL_main_func);

  // Map memory permissions (RX for code, RW for data)
  printf("Finalizing memory mappings...\n");
  if (has_sndfile) so_finalize(&so_sndfile);
  if (has_unistring) so_finalize(&so_unistring);
  so_finalize(&so_main);

  // Flush instruction caches
  printf("Flushing caches...\n");
  if (has_sndfile) so_flush_caches(&so_sndfile);
  if (has_unistring) so_flush_caches(&so_unistring);
  so_flush_caches(&so_main);

  // Execute C++ global constructors (.init_array)
  printf("Executing init_arrays...\n");
  if (has_sndfile) so_execute_init_array(&so_sndfile);
  if (has_unistring) so_execute_init_array(&so_unistring);
  so_execute_init_array(&so_main);

  // Free temp ELF images
  printf("Freeing temp images...\n");
  if (has_sndfile) so_free_temp(&so_sndfile);
  if (has_unistring) so_free_temp(&so_unistring);
  so_free_temp(&so_main);

  printf("Starting SDL_main at %p...\n", SDL_main_func);

  // Determine basepath: if assets are in /switch/rvgl/assets, point to that
  struct stat st;
  const char *basepath = "/switch/rvgl";
  if (stat("/switch/rvgl/assets/models/go2.m", &st) == 0 || stat("assets/models/go2.m", &st) == 0) {
    basepath = "/switch/rvgl/assets";
  }

  char str_w[16], str_h[16];
  snprintf(str_w, sizeof(str_w), "%d", screen_width);
  snprintf(str_h, sizeof(str_h), "%d", screen_height);

  char *game_argv[] = {
    "rvgl",
    "-basepath",
    (char *)basepath,
    "-prefpath",
    "/switch/rvgl",
    "-res",
    str_w,
    str_h,
    "32",
    NULL
  };
  int game_argc = 8;

  printf("Running with basepath: %s\n", basepath);

  int ret = SDL_main_func(game_argc, game_argv);
  printf("SDL_main returned %d\n", ret);

  void (*ReleaseNetwork_func)(void) = (void *)so_try_find_addr_rx(&so_main, "_Z14ReleaseNetworkv");
  if (ReleaseNetwork_func) {
    debugPrintf("main: calling ReleaseNetwork before exit\n");
    ReleaseNetwork_func();
  }

  deinit_openal();
  deinit_opengl();
  unpatch_game();
  deinit_network();

  printf("Unloading modules...\n");
  so_unload(&so_main);
  if (has_unistring) so_unload(&so_unistring);
  if (has_sndfile) so_unload(&so_sndfile);

  return ret;
}
