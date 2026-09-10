#ifndef __HOOKS_H__
#define __HOOKS_H__

#include "so_util.h"

extern so_module so_main;
extern so_module so_sndfile;
extern so_module so_unistring;

void patch_opengl(void);
void patch_openal(void);
void patch_game(void);
void patch_io(void);

void deinit_opengl(void);
void deinit_openal(void);

#endif
