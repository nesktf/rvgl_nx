/* openal.c -- OpenAL hooks and patches
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include "../util.h"
#include "../hooks.h"

extern ALCcontext *al_ctx;
extern ALCdevice *al_dev;

void patch_openal(void) {
  // OpenAL symbols are resolved directly via dynlib_functions
}

void deinit_openal(void) {
  if (al_ctx) {
    alcMakeContextCurrent(NULL);
    alcDestroyContext(al_ctx);
    al_ctx = NULL;
  }
  if (al_dev) {
    alcCloseDevice(al_dev);
    al_dev = NULL;
  }
  debugPrintf("deinit_openal: closed OpenAL audio device and context\n");
}
