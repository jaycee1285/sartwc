// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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
#include "output.h"
#include "view.h"
#include "workspaces.h"

#define IPC_BUF_SIZE 4096
#define IPC_MAX_RECV_BUF (64 * 1024)

struct ipc_client {
	struct wl_list link;
	struct wl_event_source *event_source;
	struct server *server;
	int fd;
	bool subscribed_events;
	struct buf recv_buf;
};

static char *ipc_socket_path;
static struct wl_list ipc_clients;
static bool ipc_clients_initialized;

static void ipc_client_destroy(struct ipc_client *client);

static bool
fd_set_cloexec_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		return false;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return false;
	}

	return true;
}

static bool
ipc_send_raw(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n > 0) {
			data += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		return false;
	}
	return true;
}

static bool
ipc_send_str(int fd, const char *msg)
{
	return ipc_send_raw(fd, msg, strlen(msg));
}

static void
ipc_broadcast_raw(struct server *server, const char *data, size_t len)
{
	if (!ipc_clients_initialized || !data || !len) {
		return;
	}

	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc_clients, link) {
		if (!client->subscribed_events || client->server != server) {
			continue;
		}
		if (!ipc_send_raw(client->fd, data, len)) {
			ipc_client_destroy(client);
		}
	}
}

static void
ipc_broadcast_event(struct server *server, const char *event)
{
	struct buf line = BUF_INIT;
	buf_add(&line, "EVENT ");
	buf_add(&line, event);
	buf_add_char(&line, '\n');
	ipc_broadcast_raw(server, line.data, (size_t)line.len);
	buf_reset(&line);
}

static bool
ipc_pct_is_unreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z')
		|| (c >= 'a' && c <= 'z')
		|| (c >= '0' && c <= '9')
		|| c == '-' || c == '_' || c == '.' || c == '~';
}

static int
ipc_hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

static bool
ipc_pct_decode_inplace(char *s)
{
	if (!s) {
		return false;
	}

	char *src = s;
	char *dst = s;
	while (*src) {
		if (*src != '%') {
			*dst++ = *src++;
			continue;
		}
		if (!src[1] || !src[2]) {
			return false;
		}
		int hi = ipc_hex_nibble(src[1]);
		int lo = ipc_hex_nibble(src[2]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		*dst++ = (char)((hi << 4) | lo);
		src += 3;
	}
	*dst = '\0';
	return true;
}

static void
buf_add_pct_encoded(struct buf *buf, const char *s)
{
	if (!s) {
		return;
	}
	for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
		if (ipc_pct_is_unreserved(*p)) {
			buf_add_char(buf, (char)*p);
		} else {
			buf_add_fmt(buf, "%%%02X", *p);
		}
	}
}

static void
buf_add_json_string(struct buf *buf, const char *s)
{
	if (!s) {
		s = "";
	}

	buf_add_char(buf, '"');
	for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
		switch (*p) {
		case '"':
			buf_add(buf, "\\\"");
			break;
		case '\\':
			buf_add(buf, "\\\\");
			break;
		case '\b':
			buf_add(buf, "\\b");
			break;
		case '\f':
			buf_add(buf, "\\f");
			break;
		case '\n':
			buf_add(buf, "\\n");
			break;
		case '\r':
			buf_add(buf, "\\r");
			break;
		case '\t':
			buf_add(buf, "\\t");
			break;
		default:
			if (*p < 0x20) {
				buf_add_fmt(buf, "\\u%04x", (unsigned)*p);
			} else {
				buf_add_char(buf, (char)*p);
			}
			break;
		}
	}
	buf_add_char(buf, '"');
}

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
	buf_add(&response, "encoding=percent\n");
	buf_add(&response, "current_workspace_name=");
	buf_add_pct_encoded(&response,
		server->workspaces.current ? server->workspaces.current->name : "");
	buf_add_char(&response, '\n');

	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped) {
			continue;
		}
		int ws_idx = workspace_index(server, view->workspace);
		const char *ws_name = view->workspace ? view->workspace->name : "";

		buf_add(&response, "view app_id=");
		buf_add_pct_encoded(&response, view->app_id ? view->app_id : "");
		buf_add(&response, " title=");
		buf_add_pct_encoded(&response, view->title ? view->title : "");
		buf_add_fmt(&response, " workspace=%d workspace_name=", ws_idx);
		buf_add_pct_encoded(&response, ws_name);
		buf_add_fmt(&response,
			" x=%d y=%d w=%d h=%d"
			" maximized=%d minimized=%d fullscreen=%d tiled=%d"
			" focused=%d\n",
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
		(void)ipc_send_raw(client_fd, response.data, (size_t)response.len);
	}
	buf_reset(&response);
}

static void
handle_query_workspaces(struct server *server, int client_fd)
{
	struct buf response = BUF_INIT;
	int current_ws = workspace_index(server, server->workspaces.current);
	buf_add_fmt(&response, "current=%d\n", current_ws);
	buf_add(&response, "encoding=percent\n");

	struct workspace *ws;
	int idx = 1;
	wl_list_for_each(ws, &server->workspaces.all, link) {
		buf_add_fmt(&response, "workspace index=%d name=", idx);
		buf_add_pct_encoded(&response, ws->name);
		buf_add_fmt(&response, " active=%d\n",
			ws == server->workspaces.current ? 1 : 0);
		idx++;
	}
	buf_add(&response, "END\n");

	if (response.len > 0) {
		(void)ipc_send_raw(client_fd, response.data, (size_t)response.len);
	}
	buf_reset(&response);
}

static void
handle_query_views_json(struct server *server, int client_fd)
{
	struct buf response = BUF_INIT;
	struct view *view;
	bool first = true;

	int current_ws = workspace_index(server, server->workspaces.current);
	buf_add(&response, "{");
	buf_add_fmt(&response, "\"current_workspace\":%d,", current_ws);
	buf_add(&response, "\"current_workspace_name\":");
	buf_add_json_string(&response,
		server->workspaces.current ? server->workspaces.current->name : "");
	buf_add(&response, ",\"views\":[");

	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped) {
			continue;
		}

		if (!first) {
			buf_add_char(&response, ',');
		}
		first = false;

		int ws_idx = workspace_index(server, view->workspace);
		const char *ws_name = view->workspace ? view->workspace->name : "";
		const char *output_name = "";
		struct wlr_box usable = {0};
		bool has_output = false;
		if (view->output && view->output->wlr_output) {
			output_name = view->output->wlr_output->name;
			usable = output_usable_area_in_layout_coords(view->output);
			has_output = true;
		}

		buf_add(&response, "{");
		buf_add(&response, "\"app_id\":");
		buf_add_json_string(&response, view->app_id ? view->app_id : "");
		buf_add(&response, ",\"title\":");
		buf_add_json_string(&response, view->title ? view->title : "");
		buf_add_fmt(&response, ",\"workspace\":%d", ws_idx);
		buf_add(&response, ",\"workspace_name\":");
		buf_add_json_string(&response, ws_name);
		buf_add_fmt(&response,
			",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d",
			view->current.x, view->current.y,
			view->current.width, view->current.height);
		buf_add(&response, ",\"output\":");
		buf_add_json_string(&response, output_name);
		if (has_output) {
			buf_add_fmt(&response,
				",\"usable_x\":%d,\"usable_y\":%d,\"usable_w\":%d,\"usable_h\":%d",
				usable.x, usable.y, usable.width, usable.height);
		} else {
			buf_add(&response,
				",\"usable_x\":0,\"usable_y\":0,\"usable_w\":0,\"usable_h\":0");
		}
		buf_add_fmt(&response,
			",\"maximized\":%s,\"minimized\":%s,\"fullscreen\":%s,"
			"\"tiled\":%s,\"focused\":%s",
			view->maximized != VIEW_AXIS_NONE ? "true" : "false",
			view->minimized ? "true" : "false",
			view->fullscreen ? "true" : "false",
			view_is_tiled(view) ? "true" : "false",
			view == server->active_view ? "true" : "false");
		buf_add(&response, "}");
	}

	buf_add(&response, "]}\n");
	if (response.len > 0) {
		(void)ipc_send_raw(client_fd, response.data, (size_t)response.len);
	}
	buf_reset(&response);
}

static void
handle_query_workspaces_json(struct server *server, int client_fd)
{
	struct buf response = BUF_INIT;
	int current_ws = workspace_index(server, server->workspaces.current);

	buf_add(&response, "{");
	buf_add_fmt(&response, "\"current_workspace\":%d,", current_ws);
	buf_add(&response, "\"current_workspace_name\":");
	buf_add_json_string(&response,
		server->workspaces.current ? server->workspaces.current->name : "");
	buf_add(&response, ",\"workspaces\":[");

	bool first = true;
	struct workspace *ws;
	int idx = 1;
	wl_list_for_each(ws, &server->workspaces.all, link) {
		if (!first) {
			buf_add_char(&response, ',');
		}
		first = false;

		buf_add(&response, "{");
		buf_add_fmt(&response, "\"index\":%d,", idx);
		buf_add(&response, "\"name\":");
		buf_add_json_string(&response, ws->name ? ws->name : "");
		buf_add_fmt(&response, ",\"active\":%s",
			ws == server->workspaces.current ? "true" : "false");
		buf_add(&response, "}");
		idx++;
	}

	buf_add(&response, "]}\n");
	if (response.len > 0) {
		(void)ipc_send_raw(client_fd, response.data, (size_t)response.len);
	}
	buf_reset(&response);
}

/*
 * Parse and execute a single IPC command line.
 *
 * Supported commands:
 *   <ActionName> [key=value ...]   - execute a labwc action
 *   list-views                     - list all mapped views with geometry
 *   list-views-json                - JSON document with mapped views + geometry
 *   list-workspaces                - list workspaces and current index
 *   list-workspaces-json           - JSON document with workspace list + current
 *   workspace-add [name=...]       - add workspace (name may be percent-encoded)
 *   workspace-rename index=N name=... - rename workspace (percent-encoded name)
 *   workspace-remove index=N       - remove workspace by 1-based index
 *   subscribe-events               - stream EVENT lines on compositor changes
 *   ping                           - respond with OK (connection test)
 *
 * Each command line is executed immediately.
 */
static void
handle_command(struct ipc_client *client, char *line)
{
	struct server *server = client->server;
	int client_fd = client->fd;

	line = string_strip(line);
	if (!line || !*line) {
		return;
	}

	if (!strcasecmp(line, "ping")) {
		(void)ipc_send_str(client_fd, "OK\n");
		return;
	}

	if (!strcasecmp(line, "subscribe-events")) {
		client->subscribed_events = true;
		(void)ipc_send_str(client_fd, "OK subscribed-events\n");
		return;
	}

	if (!strcasecmp(line, "list-views")) {
		handle_query_views(server, client_fd);
		return;
	}

	if (!strcasecmp(line, "list-views-json")) {
		handle_query_views_json(server, client_fd);
		return;
	}

	if (!strcasecmp(line, "list-workspaces")) {
		handle_query_workspaces(server, client_fd);
		return;
	}

	if (!strcasecmp(line, "list-workspaces-json")) {
		handle_query_workspaces_json(server, client_fd);
		return;
	}

	if (!strncasecmp(line, "workspace-add", strlen("workspace-add"))) {
		char *save = NULL;
		char *cmd = strtok_r(line, " \t", &save);
		(void)cmd;

		char *name = NULL;
		char *tok;
		while ((tok = strtok_r(NULL, " \t", &save))) {
			char *eq = strchr(tok, '=');
			if (!eq) {
				continue;
			}
			*eq = '\0';
			if (!strcasecmp(tok, "name")) {
				name = eq + 1;
			}
		}

		char generated[32];
		if (!name || !*name) {
			snprintf(generated, sizeof(generated), "%d",
				wl_list_length(&server->workspaces.all) + 1);
			name = generated;
		} else if (!ipc_pct_decode_inplace(name)) {
			(void)ipc_send_str(client_fd, "ERROR invalid percent-encoding in name\n");
			return;
		}

		if (!workspaces_add_named(server, name)) {
			(void)ipc_send_str(client_fd, "ERROR failed to add workspace\n");
			return;
		}
		(void)ipc_send_str(client_fd, "OK\n");
		return;
	}

	if (!strncasecmp(line, "workspace-rename", strlen("workspace-rename"))) {
		char *save = NULL;
		char *cmd = strtok_r(line, " \t", &save);
		(void)cmd;

		char *name = NULL;
		int index = 0;
		char *tok;
		while ((tok = strtok_r(NULL, " \t", &save))) {
			char *eq = strchr(tok, '=');
			if (!eq) {
				continue;
			}
			*eq = '\0';
			if (!strcasecmp(tok, "name")) {
				name = eq + 1;
			} else if (!strcasecmp(tok, "index")) {
				index = atoi(eq + 1);
			}
		}

		if (index < 1 || !name || !*name) {
			(void)ipc_send_str(client_fd, "ERROR usage: workspace-rename index=N name=...\n");
			return;
		}
		if (!ipc_pct_decode_inplace(name)) {
			(void)ipc_send_str(client_fd, "ERROR invalid percent-encoding in name\n");
			return;
		}
		if (!workspaces_rename_index(server, index, name)) {
			(void)ipc_send_str(client_fd, "ERROR failed to rename workspace\n");
			return;
		}
		(void)ipc_send_str(client_fd, "OK\n");
		return;
	}

	if (!strncasecmp(line, "workspace-remove", strlen("workspace-remove"))) {
		char *save = NULL;
		char *cmd = strtok_r(line, " \t", &save);
		(void)cmd;

		int index = 0;
		char *tok;
		while ((tok = strtok_r(NULL, " \t", &save))) {
			char *eq = strchr(tok, '=');
			if (!eq) {
				continue;
			}
			*eq = '\0';
			if (!strcasecmp(tok, "index")) {
				index = atoi(eq + 1);
			}
		}

		if (index < 1) {
			(void)ipc_send_str(client_fd, "ERROR usage: workspace-remove index=N\n");
			return;
		}
		if (!workspaces_remove_index(server, index)) {
			(void)ipc_send_str(client_fd, "ERROR failed to remove workspace\n");
			return;
		}
		(void)ipc_send_str(client_fd, "OK\n");
		return;
	}

	/* Parse: ActionName [key=value ...] */
	char *saveptr = NULL;
	char *action_name = strtok_r(line, " \t", &saveptr);
	if (!action_name) {
		(void)ipc_send_str(client_fd, "ERROR no action\n");
		return;
	}

	struct action *action = action_create(action_name);
	if (!action) {
		(void)ipc_send_str(client_fd, "ERROR unknown action\n");
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

	if (!action_is_valid(action)) {
		action_free(action);
		(void)ipc_send_str(client_fd, "ERROR missing required argument\n");
		return;
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

	(void)ipc_send_str(client_fd, "OK\n");
}

static void
ipc_client_destroy(struct ipc_client *client)
{
	if (client->event_source) {
		wl_event_source_remove(client->event_source);
		client->event_source = NULL;
	}
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
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
	if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
		return 0;
	}
	if (n <= 0) {
		ipc_client_destroy(client);
		return 0;
	}
	buf[n] = '\0';

	/* Append to receive buffer and process complete lines */
	buf_add(&client->recv_buf, buf);
	if (client->recv_buf.len > IPC_MAX_RECV_BUF) {
		(void)ipc_send_str(client->fd, "ERROR line too long\n");
		ipc_client_destroy(client);
		return 0;
	}

	char *start = client->recv_buf.data;
	char *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = '\0';
		handle_command(client, start);
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
	(void)fd;
	(void)mask;
	struct server *server = data;

	int client_fd = accept(server->ipc_fd, NULL, NULL);
	if (client_fd < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}
		wlr_log_errno(WLR_ERROR, "IPC accept failed");
		return 0;
	}
	if (!fd_set_cloexec_nonblock(client_fd)) {
		wlr_log_errno(WLR_ERROR, "IPC: failed to configure client fd");
		close(client_fd);
		return 0;
	}

	struct ipc_client *client = znew(*client);
	wl_list_init(&client->link);
	client->server = server;
	client->fd = client_fd;
	client->recv_buf = BUF_INIT;

	client->event_source = wl_event_loop_add_fd(
		server->wl_event_loop, client_fd,
		WL_EVENT_READABLE, handle_client_readable, client);
	if (!client->event_source) {
		wlr_log(WLR_ERROR, "IPC: failed to add client fd to event loop");
		ipc_client_destroy(client);
		return 0;
	}

	wl_list_insert(&ipc_clients, &client->link);
	return 0;
}

void
ipc_init(struct server *server)
{
	wl_list_init(&ipc_clients);
	ipc_clients_initialized = true;

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
	if (!fd_set_cloexec_nonblock(sock_fd)) {
		wlr_log_errno(WLR_ERROR, "IPC: failed to configure socket fd");
		close(sock_fd);
		free(ipc_socket_path);
		ipc_socket_path = NULL;
		return;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(ipc_socket_path) >= sizeof(addr.sun_path)) {
		wlr_log(WLR_ERROR, "IPC: socket path too long: %s", ipc_socket_path);
		close(sock_fd);
		free(ipc_socket_path);
		ipc_socket_path = NULL;
		return;
	}
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
	if (!server->ipc_event_source) {
		wlr_log(WLR_ERROR, "IPC: failed to add socket fd to event loop");
		close(sock_fd);
		server->ipc_fd = -1;
		unlink(ipc_socket_path);
		free(ipc_socket_path);
		ipc_socket_path = NULL;
		return;
	}

	if (setenv("SARTWC_IPC_SOCKET", ipc_socket_path, true) < 0) {
		wlr_log_errno(WLR_ERROR, "IPC: unable to set SARTWC_IPC_SOCKET");
	}

	wlr_log(WLR_INFO, "IPC: listening on %s", ipc_socket_path);
}

void
ipc_finish(struct server *server)
{
	if (!ipc_clients_initialized) {
		return;
	}

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

	ipc_clients_initialized = false;
}

void
ipc_notify_workspace_changed(struct server *server)
{
	int current_ws = workspace_index(server, server->workspaces.current);
	struct buf event = BUF_INIT;
	buf_add_fmt(&event, "workspace-changed current=%d", current_ws);
	ipc_broadcast_event(server, event.data);
	buf_reset(&event);
}

void
ipc_notify_workspace_list_changed(struct server *server)
{
	int current_ws = workspace_index(server, server->workspaces.current);
	size_t count = wl_list_length(&server->workspaces.all);
	struct buf event = BUF_INIT;
	buf_add_fmt(&event, "workspace-list-changed current=%d count=%zu",
		current_ws, count);
	ipc_broadcast_event(server, event.data);
	buf_reset(&event);
}

void
ipc_notify_focus_changed(struct server *server)
{
	int current_ws = workspace_index(server, server->workspaces.current);
	struct view *view = server->active_view;

	struct buf event = BUF_INIT;
	if (!view) {
		buf_add_fmt(&event, "focus-changed current=%d focused=0", current_ws);
	} else {
		int view_ws = workspace_index(server, view->workspace);
		buf_add_fmt(&event,
			"focus-changed current=%d focused=1 view=%p workspace=%d x=%d y=%d w=%d h=%d",
			current_ws, (void *)view, view_ws,
			view->current.x, view->current.y,
			view->current.width, view->current.height);
	}
	ipc_broadcast_event(server, event.data);
	buf_reset(&event);
}

static void
ipc_notify_view_event(struct view *view, const char *kind)
{
	if (!view || !view->server) {
		return;
	}

	struct server *server = view->server;
	int current_ws = workspace_index(server, server->workspaces.current);
	int view_ws = workspace_index(server, view->workspace);

	struct buf event = BUF_INIT;
	buf_add_fmt(&event, "%s current=%d view=%p workspace=%d x=%d y=%d w=%d h=%d",
		kind, current_ws, (void *)view, view_ws,
		view->current.x, view->current.y,
		view->current.width, view->current.height);
	ipc_broadcast_event(server, event.data);
	buf_reset(&event);
}

void
ipc_notify_view_mapped(struct view *view)
{
	ipc_notify_view_event(view, "view-mapped");
}

void
ipc_notify_view_unmapped(struct view *view)
{
	ipc_notify_view_event(view, "view-unmapped");
}
