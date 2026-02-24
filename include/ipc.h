/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_IPC_H
#define LABWC_IPC_H

struct server;
struct view;

/**
 * ipc_init - create IPC socket and add to event loop
 * @server: compositor server
 *
 * Creates a Unix domain socket at $XDG_RUNTIME_DIR/sartwc-<WAYLAND_DISPLAY>.sock
 * Clients can send newline-delimited commands to trigger actions or query state.
 */
void ipc_init(struct server *server);

/**
 * ipc_finish - clean up IPC socket and resources
 * @server: compositor server
 */
void ipc_finish(struct server *server);

/* Broadcast compositor lifecycle events to subscribed IPC clients. */
void ipc_notify_workspace_changed(struct server *server);
void ipc_notify_workspace_list_changed(struct server *server);
void ipc_notify_focus_changed(struct server *server);
void ipc_notify_view_mapped(struct view *view);
void ipc_notify_view_unmapped(struct view *view);

#endif /* LABWC_IPC_H */
