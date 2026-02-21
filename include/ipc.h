/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_IPC_H
#define LABWC_IPC_H

struct server;

/**
 * ipc_init - create IPC socket and add to event loop
 * @server: compositor server
 *
 * Creates a Unix domain socket at $XDG_RUNTIME_DIR/labwc-<WAYLAND_DISPLAY>.sock
 * Clients can send newline-delimited commands to trigger actions or query state.
 */
void ipc_init(struct server *server);

/**
 * ipc_finish - clean up IPC socket and resources
 * @server: compositor server
 */
void ipc_finish(struct server *server);

#endif /* LABWC_IPC_H */
