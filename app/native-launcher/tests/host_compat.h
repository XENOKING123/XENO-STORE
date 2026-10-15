/* XENO STORE - Linux compatibility for the launcher's host test build.
 * Copyright (C) 2026 XENO STORE contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Force-included (-include) by tests/run-host-test.py only: the BSD
 * sockaddr_in.sin_len field and SO_NOSIGPIPE do not exist on Linux.
 */
#define sin_len sin_zero[7]
#define SO_NOSIGPIPE SO_KEEPALIVE
#define main launcher_main
