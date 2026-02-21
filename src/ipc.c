// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include "action.h"
#include "common/buf.h"
#include "common/mem.h"
#include "common/string-helpers.h"
#include "labwc.h"
#include "view.h"
#include "workspaces.h"

#define IPC_BUF_SIZE 4096

struct ipc_client {
	struct wl_list link;
	struct wl_event_source *event_source;
	struct server *server;
	int fd;
	struct buf recv_buf;
};

static char *ipc_socket_path;
static struct wl_list ipc_clients;

/*
 * Get workspace index (1-based) by walking the workspace list.
 * Returns 0 if workspace is NULL.
 */
static int
workspace_index(struct server *server, struct workspace *ws)
{
	if (!ws) {
		return 0;
	}
	int idx = 1;
	struct workspace *iter;
	wl_list_for_each(iter, &server->workspaces.all, link) {
		if (iter == ws) {
			return idx;
		}
		idx++;
	}
	return 0;
}

static void
handle_query_views(struct server *server, int client_fd)
{
	struct buf response = BUF_INIT;
	struct view *view;

	int current_ws = workspace_index(server, server->workspaces.current);
	buf_add_fmt(&response, "current_workspace=%d\n", current_ws);
	buf_add_fmt(&response, "current_workspace_name=%s\n",
		server->workspaces.current ? server->workspaces.current->name : "");

	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped) {
			continue;
		}
		int ws_idx = workspace_index(server, view->workspace);
		const char *ws_name = view->workspace ? view->workspace->name : "";

		buf_add_fmt(&response,
			"view app_id=%s title=%s workspace=%d workspace_name=%s"
			" x=%d y=%d w=%d h=%d"
			" maximized=%d minimized=%d fullscreen=%d tiled=%d"
			" focused=%d\n",
			view->app_id ? view->app_id : "",
			view->title ? view->title : "",
			ws_idx,
			ws_name,
			view->current.x, view->current.y,
			view->current.width, view->current.height,
			view->maximized != VIEW_AXIS_NONE ? 1 : 0,
			view->minimized ? 1 : 0,
			view->fullscreen ? 1 : 0,
			view_is_tiled(view) ? 1 : 0,
			view == server->active_view ? 1 : 0);
	}
	buf_add(&response, "END\n");

	if (response.len > 0) {
		write(client_fd, response.data, response.len);
	}
	buf_reset(&response);
}

static void
handle_query_workspaces(struct server *server, int client_fd)
{
	struct buf response = BUF_INIT;
	int current_ws = workspace_index(server, server->workspaces.current);
	buf_add_fmt(&response, "current=%d\n", current_ws);

	struct workspace *ws;
	int idx = 1;
	wl_list_for_each(ws, &server->workspaces.all, link) {
		buf_add_fmt(&response, "workspace index=%d name=%s active=%d\n",
			idx, ws->name, ws == server->workspaces.current ? 1 : 0);
		idx++;
	}
	buf_add(&response, "END\n");

	if (response.len > 0) {
		write(client_fd, response.data, response.len);
	}
	buf_reset(&response);
}

/*
 * Parse and execute a single IPC command line.
 *
 * Supported commands:
 *   <ActionName> [key=value ...]   - execute a labwc action
 *   list-views                     - list all mapped views with geometry
 *   list-workspaces                - list workspaces and current index
 *   ping                           - respond with OK (connection test)
 *
 * Actions are the same names used in rc.xml (case-insensitive):
 *   MoveTo x=0 y=0
 *   ResizeTo width=50% height=100%
 *   GoToDesktop to=right wrap=yes
 *   SendToDesktop to=3 follow=no
 *   Close
 *   ToggleMaximize
 *   etc.
 *
 * Multiple actions can be sent as separate lines. A blank line or
 * connection close triggers execution of any queued batch.
 */
static void
handle_command(struct server *server, int client_fd, char *line)
{
	line = string_strip(line);
	if (!line || !*line) {
		return;
	}

	if (!strcasecmp(line, "ping")) {
		write(client_fd, "OK\n", 3);
		return;
	}

	if (!strcasecmp(line, "list-views")) {
		handle_query_views(server, client_fd);
		return;
	}

	if (!strcasecmp(line, "list-workspaces")) {
		handle_query_workspaces(server, client_fd);
		return;
	}

	/* Parse: ActionName [key=value ...] */
	char *saveptr = NULL;
	char *action_name = strtok_r(line, " \t", &saveptr);
	if (!action_name) {
		write(client_fd, "ERROR no action\n", 16);
		return;
	}

	struct action *action = action_create(action_name);
	if (!action_is_valid(action)) {
		action_free(action);
		const char *msg = "ERROR unknown action\n";
		write(client_fd, msg, strlen(msg));
		return;
	}

	/* Parse remaining key=value pairs as string args */
	char *token;
	while ((token = strtok_r(NULL, " \t", &saveptr))) {
		char *eq = strchr(token, '=');
		if (eq) {
			*eq = '\0';
			action_arg_add_str(action, token, eq + 1);
		}
	}

	/* Build a temporary action list and run it */
	struct wl_list actions;
	wl_list_init(&actions);
	wl_list_insert(&actions, &action->link);

	actions_run(NULL, server, &actions, NULL);

	/*
	 * Remove action from our local list before freeing
	 * (actions_run doesn't take ownership)
	 */
	wl_list_remove(&action->link);
	action_free(action);

	write(client_fd, "OK\n", 3);
}

static void
ipc_client_destroy(struct ipc_client *client)
{
	wl_event_source_remove(client->event_source);
	close(client->fd);
	wl_list_remove(&client->link);
	buf_reset(&client->recv_buf);
	free(client);
}

static int
handle_client_readable(int fd, uint32_t mask, void *data)
{
	struct ipc_client *client = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_client_destroy(client);
		return 0;
	}

	char buf[IPC_BUF_SIZE];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		ipc_client_destroy(client);
		return 0;
	}
	buf[n] = '\0';

	/* Append to receive buffer and process complete lines */
	buf_add(&client->recv_buf, buf);

	char *start = client->recv_buf.data;
	char *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = '\0';
		handle_command(client->server, client->fd, start);
		start = nl + 1;
	}

	/* Keep any remaining partial line */
	if (*start) {
		struct buf remainder = BUF_INIT;
		buf_add(&remainder, start);
		buf_reset(&client->recv_buf);
		buf_move(&client->recv_buf, &remainder);
	} else {
		buf_clear(&client->recv_buf);
	}

	return 0;
}

static int
handle_new_connection(int fd, uint32_t mask, void *data)
{
	struct server *server = data;

	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0) {
		wlr_log_errno(WLR_ERROR, "IPC accept failed");
		return 0;
	}

	struct ipc_client *client = znew(*client);
	client->server = server;
	client->fd = client_fd;
	client->recv_buf = BUF_INIT;

	client->event_source = wl_event_loop_add_fd(
		server->wl_event_loop, client_fd,
		WL_EVENT_READABLE, handle_client_readable, client);

	wl_list_insert(&ipc_clients, &client->link);
	return 0;
}

void
ipc_init(struct server *server)
{
	wl_list_init(&ipc_clients);

	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		wlr_log(WLR_ERROR, "IPC: XDG_RUNTIME_DIR not set");
		return;
	}

	const char *wayland_display = getenv("WAYLAND_DISPLAY");
	if (!wayland_display) {
		wlr_log(WLR_ERROR, "IPC: WAYLAND_DISPLAY not set");
		return;
	}

	ipc_socket_path = strdup_printf("%s/sartwc-%s.sock",
		runtime_dir, wayland_display);
	if (!ipc_socket_path) {
		wlr_log(WLR_ERROR, "IPC: failed to allocate socket path");
		return;
	}

	/* Remove stale socket if it exists */
	unlink(ipc_socket_path);

	int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		wlr_log_errno(WLR_ERROR, "IPC: socket creation failed");
		free(ipc_socket_path);
		ipc_socket_path = NULL;
		return;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, ipc_socket_path, sizeof(addr.sun_path) - 1);

	if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		wlr_log_errno(WLR_ERROR, "IPC: bind failed on %s", ipc_socket_path);
		close(sock_fd);
		free(ipc_socket_path);
		ipc_socket_path = NULL;
		return;
	}

	if (listen(sock_fd, 4) < 0) {
		wlr_log_errno(WLR_ERROR, "IPC: listen failed");
		close(sock_fd);
		unlink(ipc_socket_path);
		free(ipc_socket_path);
		ipc_socket_path = NULL;
		return;
	}

	server->ipc_fd = sock_fd;
	server->ipc_event_source = wl_event_loop_add_fd(
		server->wl_event_loop, sock_fd,
		WL_EVENT_READABLE, handle_new_connection, server);

	if (setenv("SARTWC_IPC_SOCKET", ipc_socket_path, true) < 0) {
		wlr_log_errno(WLR_ERROR, "IPC: unable to set SARTWC_IPC_SOCKET");
	}

	wlr_log(WLR_INFO, "IPC: listening on %s", ipc_socket_path);
}

void
ipc_finish(struct server *server)
{
	/* Destroy all connected clients */
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc_clients, link) {
		ipc_client_destroy(client);
	}

	if (server->ipc_event_source) {
		wl_event_source_remove(server->ipc_event_source);
		server->ipc_event_source = NULL;
	}

	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		server->ipc_fd = -1;
	}

	if (ipc_socket_path) {
		unlink(ipc_socket_path);
		free(ipc_socket_path);
		ipc_socket_path = NULL;
	}
}
