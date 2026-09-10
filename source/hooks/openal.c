/* openal.c -- OpenAL hooks and patches
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "../hooks.h"

void patch_openal(void) {
  // OpenAL symbols are resolved directly via dynlib_functions
}

void deinit_openal(void) {
}
