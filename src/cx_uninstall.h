/*
 * cx_uninstall.h — clean uninstall (SPEC §17.16) and the ghost icon cleaner
 * for /data/homebrew folders.
 */
#ifndef XENO_CX_UNINSTALL_H
#define XENO_CX_UNINSTALL_H

#include "json.h"
#include <stddef.h>

/* Performs the selected deletions (exact paths only) and writes "removed",
 * "kept", "failed". homebrew covers <homebrew>/PPSA99500.ffpkg, the same app
 * as a folder (<homebrew>/PPSA99500 with an eboot.bin), the old
 * <homebrew>/XENO-STORE folder and the PPSA99500 and XENO00002 home-screen
 * titles (uninstalled, reported as <user/app>/<id>). Returns 0; the caller
 * stops the server afterwards. */
int cx_uninstall(int data, int homebrew, int payload_manager, int downloads, jw *w);
/* Writes "dirs": [{name,path,size,hasEboot,hasIcon,isXenoStore,mtime}]. */
void cx_homebrew_dirs_json(jw *w);
/* Deletes one listed folder; confirm must equal the folder name. */
int cx_homebrew_dir_delete(const char *name, const char *confirm, char *code, size_t ccap, char *err, size_t errcap);

#endif
