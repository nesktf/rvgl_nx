/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#define DEBUG_LOG
#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    s_nxlinkSock = -1;
  }
  socketExit();
}

static FILE *s_logFile = NULL;

void userAppInit(void) {
  initNxLink();
  s_logFile = fopen(LOG_NAME, "w");
}

void userAppExit(void) {
  if (s_logFile) {
    fclose(s_logFile);
    s_logFile = NULL;
  }
  deinitNxLink();
}

#endif

int debugPrintf(char *text, ...) {
#ifdef DEBUG_LOG
  va_list list;

  if (s_logFile) {
    va_start(list, text);
    vfprintf(s_logFile, text, list);
    va_end(list);
    fflush(s_logFile);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  fflush(stdout);
#endif
  return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int retm1(void) { return -1; }
