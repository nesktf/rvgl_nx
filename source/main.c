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

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

static void write_rvgl_ini(const char *path) {
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f,
      "[Game]\n"
      "Language = 3\n"
      "\n"
      "[Video]\n"
      "ScreenWidth = 1280\n"
      "ScreenHeight = 720\n"
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
      "DemoTimeout = 0\n"
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
  mkdir("/switch/rvgl/profiles/t", 0777);
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

  // Ensure profiles/t/profile.ini has audio settings
  struct stat st_prof;
  if (stat("/switch/rvgl/profiles/t/profile.ini", &st_prof) < 0) {
    FILE *fp = fopen("/switch/rvgl/profiles/t/profile.ini", "w");
    if (fp) {
      fprintf(fp,
        "[Audio]\n"
        "MusicOn = 1\n"
        "MusicVol = 100\n"
        "SfxVol = 100\n"
        "SfxChannels = 16\n"
        "SampleRate = 48000\n"
      );
      fclose(fp);
    }
  }

  check_syscalls();
  check_data();

  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("heap size = %u KB\n", (u32)(MEMORY_MB * 1024));
  debugPrintf(" lib base = %p\n", heap_so_base);
  debugPrintf("  lib max = %u KB\n", (u32)(heap_so_limit / 1024));


  void *cur_so_base = heap_so_base;
  size_t cur_so_limit = heap_so_limit;

  // 1. Load libsndfile.so if available
  int has_sndfile = (so_load(&so_sndfile, "libsndfile.so", cur_so_base, cur_so_limit) == 0);
  if (has_sndfile) {
    debugPrintf("Loaded libsndfile.so\n");
    cur_so_base = (char *)cur_so_base + so_sndfile.load_size;
    cur_so_limit -= so_sndfile.load_size;
  } else {
    debugPrintf("libsndfile.so not found (optional)\n");
  }

  // 2. Load libunistring.so if available
  int has_unistring = (so_load(&so_unistring, "libunistring.so", cur_so_base, cur_so_limit) == 0);
  if (has_unistring) {
    debugPrintf("Loaded libunistring.so\n");
    cur_so_base = (char *)cur_so_base + so_unistring.load_size;
    cur_so_limit -= so_unistring.load_size;
  } else {
    debugPrintf("libunistring.so not found (optional)\n");
  }

  // 3. Load main game binary (libmain.so)
  if (so_load(&so_main, SO_NAME, cur_so_base, cur_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
  debugPrintf("Loaded %s\n", SO_NAME);

  // Relocate loaded modules
  debugPrintf("Relocating modules...\n");
  if (has_sndfile) so_relocate(&so_sndfile);
  if (has_unistring) so_relocate(&so_unistring);
  so_relocate(&so_main);

  // Resolve imports
  debugPrintf("Resolving imports...\n");
  if (has_sndfile) so_resolve(&so_sndfile, dynlib_functions, dynlib_numfunctions, 0);
  if (has_unistring) so_resolve(&so_unistring, dynlib_functions, dynlib_numfunctions, 0);
  so_resolve(&so_main, dynlib_functions, dynlib_numfunctions, 1);

  // Apply patches / hooks
  debugPrintf("Patching hooks...\n");
  patch_openal();
  patch_opengl();
  patch_game();

  // Find game entrypoint
  debugPrintf("Resolving SDL_main entrypoint...\n");
  int (*SDL_main_func)(int argc, char *argv[]) = (void *)so_find_addr_rx(&so_main, "SDL_main");
  if (!SDL_main_func)
    fatal_error("Could not find SDL_main in\n%s.", SO_NAME);
  debugPrintf("Found SDL_main at %p\n", SDL_main_func);

  // Map memory permissions (RX for code, RW for data)
  debugPrintf("Finalizing memory mappings...\n");
  if (has_sndfile) so_finalize(&so_sndfile);
  if (has_unistring) so_finalize(&so_unistring);
  so_finalize(&so_main);

  // Flush instruction caches
  debugPrintf("Flushing caches...\n");
  if (has_sndfile) so_flush_caches(&so_sndfile);
  if (has_unistring) so_flush_caches(&so_unistring);
  so_flush_caches(&so_main);

  // Execute C++ global constructors (.init_array)
  debugPrintf("Executing init_arrays...\n");
  if (has_sndfile) so_execute_init_array(&so_sndfile);
  if (has_unistring) so_execute_init_array(&so_unistring);
  so_execute_init_array(&so_main);

  // Free temp ELF images
  debugPrintf("Freeing temp images...\n");
  if (has_sndfile) so_free_temp(&so_sndfile);
  if (has_unistring) so_free_temp(&so_unistring);
  so_free_temp(&so_main);

  debugPrintf("Starting SDL_main at %p...\n", SDL_main_func);

  // Determine basepath: if assets are in /switch/rvgl/assets, point to that
  struct stat st;
  const char *basepath = "/switch/rvgl";
  if (stat("/switch/rvgl/assets/models/go2.m", &st) == 0 || stat("assets/models/go2.m", &st) == 0) {
    basepath = "/switch/rvgl/assets";
  }

  char *game_argv[] = {
    "rvgl",
    "-basepath",
    (char *)basepath,
    "-prefpath",
    "/switch/rvgl",
    NULL
  };
  int game_argc = 5;

  debugPrintf("Running with basepath: %s\n", basepath);

  int ret = SDL_main_func(game_argc, game_argv);
  debugPrintf("SDL_main returned %d\n", ret);

  return ret;
}
