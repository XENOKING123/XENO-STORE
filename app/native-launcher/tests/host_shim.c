/* XENO STORE - host stand-ins for the console calls the launcher makes.
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Lets tests/run-host-test.py build src/launcher.c for Linux and drive it
 * against a fake store and a fake ELF loader. The kernel log goes to stderr;
 * sceSystemServiceLoadExec("exit") ends the process. main() plays
 * app_crt.cpp's part: run the launcher's main, then catchReturnFromMain().
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int launcher_main(void);
void catchReturnFromMain(int status);

int sceKernelDebugOutText(int channel, const char *text) {
  (void)channel;
  fputs(text, stderr);
  fflush(stderr);
  return 0;
}

int sceKernelUsleep(unsigned int microseconds) { return usleep(microseconds); }

int sceSystemServiceLoadExec(const char *path, char *const argv[]) {
  (void)argv;
  fprintf(stderr, "HOST: sceSystemServiceLoadExec(\"%s\")\n", path);
  fflush(stderr);
  if (strcmp(path, "exit") == 0) _exit(0);
  return -1;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);   /* Linux has no SO_NOSIGPIPE (host_compat.h) */
  catchReturnFromMain(launcher_main());
  return 99;   /* not reached */
}
