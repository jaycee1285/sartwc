// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "workspaces.h"
#include <assert.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/font.h"
#include "common/graphic-helpers.h"
#include "common/list.h"
#include "common/mem.h"
#include "config/rcxml.h"
#include "input/keyboard.h"
#include "ipc.h"
#include "labwc.h"
#include "output.h"
#include "protocols/cosmic-workspaces.h"
#include "protocols/ext-workspace.h"
#include "theme.h"
#include "view.h"

#define COSMIC_WORKSPACES_VERSION 1
#define EXT_WORKSPACES_VERSION 1
#define WORKSPACE_STATE_FILE "workspaces.txt"

/* Internal helpers */
static void
workspace_config_list_destroy(struct wl_list *list)
{
	struct workspace_config *conf, *tmp;
	wl_list_for_each_safe(conf, tmp, list, link) {
		zfree(conf->name);
		wl_list_remove(&conf->link);
		free(conf);
	}
}

static bool
mkdir_p(const char *path)
{
	if (!path || !*path) {
		return false;
	}

	char *tmp = xstrdup(path);
	for (char *p = tmp + 1; *p; ++p) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		if (*tmp && mkdir(tmp, 0700) < 0 && errno != EEXIST) {
			free(tmp);
			return false;
		}
		*p = '/';
	}

	bool ok = mkdir(tmp, 0700) == 0 || errno == EEXIST;
	free(tmp);
	return ok;
}

static char *
workspace_state_dir_path(void)
{
	const char *xdg_state_home = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");

	if (xdg_state_home && *xdg_state_home) {
		size_t len = strlen(xdg_state_home) + 1 + strlen("sartwc") + 1;
		char *path = xmalloc(len);
		snprintf(path, len, "%s/sartwc", xdg_state_home);
		return path;
	}
	if (home && *home) {
		size_t len = strlen(home) + strlen("/.local/state/sartwc") + 1;
		char *path = xmalloc(len);
		snprintf(path, len, "%s/.local/state/sartwc", home);
		return path;
	}

	return NULL;
}

static char *
workspace_state_file_path(void)
{
	char *dir = workspace_state_dir_path();
	if (!dir) {
		return NULL;
	}

	size_t len = strlen(dir) + 1 + strlen(WORKSPACE_STATE_FILE) + 1;
	char *path = xmalloc(len);
	snprintf(path, len, "%s/%s", dir, WORKSPACE_STATE_FILE);
	free(dir);
	return path;
}

static bool
workspace_config_list_load_persisted(struct wl_list *out)
{
	wl_list_init(out);

	char *path = workspace_state_file_path();
	if (!path) {
		return false;
	}

	FILE *fp = fopen(path, "r");
	if (!fp) {
		if (errno != ENOENT) {
			wlr_log_errno(WLR_ERROR, "Failed to open workspace state file %s", path);
		}
		free(path);
		return false;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t nread;
	int count = 0;
	while ((nread = getline(&line, &cap, fp)) >= 0) {
		while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
			line[--nread] = '\0';
		}
		if (nread <= 0) {
			continue;
		}

		struct workspace_config *conf = znew(*conf);
		conf->name = xstrdup(line);
		wl_list_append(out, &conf->link);
		count++;
	}

	free(line);
	fclose(fp);
	free(path);

	if (count <= 0) {
		workspace_config_list_destroy(out);
		return false;
	}

	return true;
}

static void
workspace_state_persist(struct server *server)
{
	char *dir = workspace_state_dir_path();
	char *path = workspace_state_file_path();
	if (!dir || !path) {
		free(dir);
		free(path);
		return;
	}
	if (!mkdir_p(dir)) {
		wlr_log_errno(WLR_ERROR, "Failed to create workspace state dir %s", dir);
		free(dir);
		free(path);
		return;
	}
	free(dir);

	size_t tmp_len = strlen(path) + strlen(".tmp") + 1;
	char *tmp_path = xmalloc(tmp_len);
	snprintf(tmp_path, tmp_len, "%s.tmp", path);

	FILE *fp = fopen(tmp_path, "w");
	if (!fp) {
		wlr_log_errno(WLR_ERROR, "Failed to write workspace state file %s", tmp_path);
		free(tmp_path);
		free(path);
		return;
	}

	struct workspace *workspace;
	bool ok = true;
	wl_list_for_each(workspace, &server->workspaces.all, link) {
		const char *name = workspace->name ? workspace->name : "";
		if (fputs(name, fp) == EOF || fputc('\n', fp) == EOF) {
			ok = false;
			break;
		}
	}

	if (fclose(fp) != 0) {
		ok = false;
	}

	if (!ok) {
		wlr_log_errno(WLR_ERROR, "Failed while writing workspace state file %s", tmp_path);
		unlink(tmp_path);
		free(tmp_path);
		free(path);
		return;
	}

	if (rename(tmp_path, path) < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to replace workspace state file %s", path);
		unlink(tmp_path);
	}

	free(tmp_path);
	free(path);
}

static size_t
parse_workspace_index(const char *name)
{
	/*
	 * We only want to get positive numbers which span the whole string.
	 *
	 * More detailed requirement:
	 *  .---------------.--------------.
	 *  |     Input     | Return value |
	 *  |---------------+--------------|
	 *  | "2nd desktop" |      0       |
	 *  |    "-50"      |      0       |
	 *  |     "0"       |      0       |
	 *  |    "124"      |     124      |
	 *  |    "1.24"     |      0       |
	 *  `------------------------------´
	 *
	 * As atoi() happily parses any numbers until it hits a non-number we
	 * can't really use it for this case. Instead, we use strtol() combined
	 * with further checks for the endptr (remaining non-number characters)
	 * and returned negative numbers.
	 */
	long index;
	char *endptr;
	errno = 0;
	index = strtol(name, &endptr, 10);
	if (errno || *endptr != '\0' || index < 0) {
		return 0;
	}
	return index;
}

static void
_osd_update(struct server *server)
{
	struct theme *theme = server->theme;

	/* Settings */
	uint16_t margin = 10;
	uint16_t padding = 2;
	uint16_t rect_height = theme->osd_workspace_switcher_boxes_height;
	uint16_t rect_width = theme->osd_workspace_switcher_boxes_width;
	bool hide_boxes = theme->osd_workspace_switcher_boxes_width == 0 ||
		theme->osd_workspace_switcher_boxes_height == 0;

	/* Dimensions */
	size_t workspace_count = wl_list_length(&server->workspaces.all);
	uint16_t marker_width = workspace_count * (rect_width + padding) - padding;
	uint16_t width = margin * 2 + (marker_width < 200 ? 200 : marker_width);
	uint16_t height = margin * (hide_boxes ? 2 : 3) + rect_height + font_height(&rc.font_osd);

	cairo_t *cairo;
	cairo_surface_t *surface;
	struct workspace *workspace;

	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		struct lab_data_buffer *buffer = buffer_create_cairo(width, height,
			output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for workspace OSD");
			continue;
		}

		cairo = cairo_create(buffer->surface);

		/* Background */
		set_cairo_color(cairo, theme->osd_bg_color);
		cairo_rectangle(cairo, 0, 0, width, height);
		cairo_fill(cairo);

		/* Border */
		set_cairo_color(cairo, theme->osd_border_color);
		struct wlr_fbox border_fbox = {
			.width = width,
			.height = height,
		};
		draw_cairo_border(cairo, border_fbox, theme->osd_border_width);

		/* Boxes */
		uint16_t x;
		if (!hide_boxes) {
			x = (width - marker_width) / 2;
			wl_list_for_each(workspace, &server->workspaces.all, link) {
				bool active =  workspace == server->workspaces.current;
				set_cairo_color(cairo, server->theme->osd_label_text_color);
				struct wlr_fbox fbox = {
					.x = x,
					.y = margin,
					.width = rect_width,
					.height = rect_height,
				};
				draw_cairo_border(cairo, fbox,
					theme->osd_workspace_switcher_boxes_border_width);
				if (active) {
					cairo_rectangle(cairo, x, margin,
						rect_width, rect_height);
					cairo_fill(cairo);
				}
				x += rect_width + padding;
			}
		}

		/* Text */
		set_cairo_color(cairo, server->theme->osd_label_text_color);
		PangoLayout *layout = pango_cairo_create_layout(cairo);
		pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* Center workspace indicator on the x axis */
		int req_width = font_width(&rc.font_osd, server->workspaces.current->name);
		req_width = MIN(req_width, width - 2 * margin);
		x = (width - req_width) / 2;
		if (!hide_boxes) {
			cairo_move_to(cairo, x, margin * 2 + rect_height);
		} else {
			cairo_move_to(cairo, x, (height - font_height(&rc.font_osd)) / 2.0);
		}
		PangoFontDescription *desc = font_to_pango_desc(&rc.font_osd);
		//pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, req_width * PANGO_SCALE);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, server->workspaces.current->name, -1);
		pango_cairo_show_layout(cairo, layout);

		g_object_unref(layout);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		if (!output->workspace_osd) {
			output->workspace_osd = wlr_scene_buffer_create(
				&server->scene->tree, NULL);
		}
		/* Position the whole thing */
		struct wlr_box output_box;
		wlr_output_layout_get_box(output->server->output_layout,
			output->wlr_output, &output_box);
		int lx = output_box.x + (output_box.width - width) / 2;
		int ly = output_box.y + (output_box.height - height) / 2;
		wlr_scene_node_set_position(&output->workspace_osd->node, lx, ly);
		wlr_scene_buffer_set_buffer(output->workspace_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(output->workspace_osd,
			buffer->logical_width, buffer->logical_height);

		/* And finally drop the buffer so it will get destroyed on OSD hide */
		wlr_buffer_drop(&buffer->base);
	}
}

static struct workspace *
workspace_find_by_name(struct server *server, const char *name)
{
	struct workspace *workspace;

	/* by index */
	size_t parsed_index = parse_workspace_index(name);
	if (parsed_index) {
		size_t index = 0;
		wl_list_for_each(workspace, &server->workspaces.all, link) {
			if (parsed_index == ++index) {
				return workspace;
			}
		}
	}

	/* by name */
	wl_list_for_each(workspace, &server->workspaces.all, link) {
		if (!strcmp(workspace->name, name)) {
			return workspace;
		}
	}

	wlr_log(WLR_ERROR, "Workspace '%s' not found", name);
	return NULL;
}

static struct workspace *
workspace_by_index(struct server *server, int index)
{
	if (index < 1) {
		return NULL;
	}

	struct workspace *workspace;
	int i = 1;
	wl_list_for_each(workspace, &server->workspaces.all, link) {
		if (i++ == index) {
			return workspace;
		}
	}
	return NULL;
}

/* cosmic workspace handlers */
static void
handle_cosmic_workspace_activate(struct wl_listener *listener, void *data)
{
	struct workspace *workspace = wl_container_of(listener, workspace, on_cosmic.activate);
	workspaces_switch_to(workspace, /* update_focus */ true);
	wlr_log(WLR_INFO, "cosmic activating workspace %s", workspace->name);
}

/* ext workspace handlers */
static void
handle_ext_workspace_activate(struct wl_listener *listener, void *data)
{
	struct workspace *workspace = wl_container_of(listener, workspace, on_ext.activate);
	workspaces_switch_to(workspace, /* update_focus */ true);
	wlr_log(WLR_INFO, "ext activating workspace %s", workspace->name);
}

/* Internal API */
static void
add_workspace(struct server *server, const char *name)
{
	struct workspace *workspace = znew(*workspace);
	workspace->server = server;
	workspace->name = xstrdup(name);
	workspace->tree = wlr_scene_tree_create(server->workspace_tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_BOTTOM] =
		wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_NORMAL] =
		wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_TOP] =
		wlr_scene_tree_create(workspace->tree);
	wl_list_append(&server->workspaces.all, &workspace->link);
	wlr_scene_node_set_enabled(&workspace->tree->node, false);

	/* cosmic */
	workspace->cosmic_workspace = lab_cosmic_workspace_create(server->workspaces.cosmic_group);
	lab_cosmic_workspace_set_name(workspace->cosmic_workspace, name);

	workspace->on_cosmic.activate.notify = handle_cosmic_workspace_activate;
	wl_signal_add(&workspace->cosmic_workspace->events.activate,
		&workspace->on_cosmic.activate);

	/* ext */
	workspace->ext_workspace = lab_ext_workspace_create(
		server->workspaces.ext_manager, /*id*/ NULL);
	lab_ext_workspace_assign_to_group(workspace->ext_workspace, server->workspaces.ext_group);
	lab_ext_workspace_set_name(workspace->ext_workspace, name);

	workspace->on_ext.activate.notify = handle_ext_workspace_activate;
	wl_signal_add(&workspace->ext_workspace->events.activate,
		&workspace->on_ext.activate);
}

static struct workspace *
get_prev(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.prev;
	if (target_link == workspaces) {
		/* Current workspace is the first one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->prev;
	}
	return wl_container_of(target_link, current, link);
}

static struct workspace *
get_next(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.next;
	if (target_link == workspaces) {
		/* Current workspace is the last one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->next;
	}
	return wl_container_of(target_link, current, link);
}

static bool
workspace_has_views(struct workspace *workspace, struct server *server)
{
	struct view *view;

	for_each_view(view, &server->views, LAB_VIEW_CRITERIA_NO_OMNIPRESENT) {
		if (view->workspace == workspace) {
			return true;
		}
	}
	return false;
}

static struct workspace *
get_adjacent_occupied(struct workspace *current, struct wl_list *workspaces,
		bool wrap, bool reverse)
{
	struct server *server = current->server;
	struct wl_list *start = &current->link;
	struct wl_list *link = reverse ? start->prev : start->next;
	bool has_wrapped = false;

	while (true) {
		/* Handle list boundaries */
		if (link == workspaces) {
			if (!wrap) {
				break;  /* No wrapping allowed - stop searching */
			}
			if (has_wrapped) {
				break;  /* Already wrapped once - stop to prevent infinite loop */
			}
			/* Wrap around */
			link = reverse ? workspaces->prev : workspaces->next;
			has_wrapped = true;
			continue;
		}

		/* Get the workspace */
		struct workspace *target = wl_container_of(link, target, link);

		/* Check if we've come full circle */
		if (link == start) {
			break;
		}

		/* Check if it's occupied (and not current) */
		if (target != current && workspace_has_views(target, server)) {
			return target;
		}

		/* Move to next/prev */
		link = reverse ? link->prev : link->next;
	}

	return NULL;  /* No occupied workspace found */
}

static struct workspace *
get_prev_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, true);
}

static struct workspace *
get_next_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, false);
}

static int
_osd_handle_timeout(void *data)
{
	struct seat *seat = data;
	workspaces_osd_hide(seat);
	/* Don't re-check */
	return 0;
}

static void
_osd_show(struct server *server)
{
	if (!rc.workspace_config.popuptime) {
		return;
	}

	_osd_update(server);
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output_is_usable(output) && output->workspace_osd) {
			wlr_scene_node_set_enabled(&output->workspace_osd->node, true);
		}
	}
	if (keyboard_get_all_modifiers(&server->seat)) {
		/* Hidden by release of all modifiers */
		server->seat.workspace_osd_shown_by_modifier = true;
	} else {
		/* Hidden by timer */
		if (!server->seat.workspace_osd_timer) {
			server->seat.workspace_osd_timer = wl_event_loop_add_timer(
				server->wl_event_loop, _osd_handle_timeout, &server->seat);
		}
		wl_event_source_timer_update(server->seat.workspace_osd_timer,
			rc.workspace_config.popuptime);
	}
}

/* Public API */
void
workspaces_init(struct server *server)
{
	server->workspaces.cosmic_manager = lab_cosmic_workspace_manager_create(
		server->wl_display, /* capabilities */ CW_CAP_WS_ACTIVATE,
		COSMIC_WORKSPACES_VERSION);

	server->workspaces.ext_manager = lab_ext_workspace_manager_create(
		server->wl_display, /* capabilities */ WS_CAP_WS_ACTIVATE,
		EXT_WORKSPACES_VERSION);

	server->workspaces.cosmic_group = lab_cosmic_workspace_group_create(
		server->workspaces.cosmic_manager);

	server->workspaces.ext_group = lab_ext_workspace_group_create(
		server->workspaces.ext_manager);

	wl_list_init(&server->workspaces.all);

	/*
	 * Startup policy: always begin a fresh session with a single workspace.
	 * Runtime add/remove/rename persists and is used during reconfigure,
	 * but a compositor launch intentionally resets to "1".
	 */
	add_workspace(server, "1");

	/*
	 * Startup policy ignores rc.xml initial workspace selection because the
	 * live session always starts with a single workspace.
	 */
	struct workspace *first = wl_container_of(
		server->workspaces.all.next, first, link);
	struct workspace *initial = first;

	server->workspaces.current = initial;
	wlr_scene_node_set_enabled(&initial->tree->node, true);
	lab_cosmic_workspace_set_active(initial->cosmic_workspace, true);
	lab_ext_workspace_set_active(initial->ext_workspace, true);

	/* Overwrite any previous session's persisted workspace list at launch. */
	workspace_state_persist(server);
}

/*
 * update_focus should normally be set to true. It is set to false only
 * when this function is called from desktop_focus_view(), in order to
 * avoid unnecessary extra focus changes and possible recursion.
 */
void
workspaces_switch_to(struct workspace *target, bool update_focus)
{
	assert(target);
	struct server *server = target->server;
	if (target == server->workspaces.current) {
		return;
	}

	/* Disable the old workspace */
	wlr_scene_node_set_enabled(
		&server->workspaces.current->tree->node, false);

	lab_cosmic_workspace_set_active(
		server->workspaces.current->cosmic_workspace, false);
	lab_ext_workspace_set_active(
		server->workspaces.current->ext_workspace, false);

	/*
	 * Move Omnipresent views to new workspace.
	 * Not using for_each_view() since it skips views that
	 * view_is_focusable() returns false (e.g. Conky).
	 */
	struct view *view;
	wl_list_for_each_reverse(view, &server->views, link) {
		if (view->visible_on_all_workspaces) {
			view_move_to_workspace(view, target);
		}
	}

	/* Enable the new workspace */
	wlr_scene_node_set_enabled(&target->tree->node, true);

	/* Save the last visited workspace */
	server->workspaces.last = server->workspaces.current;

	/* Make sure new views will spawn on the new workspace */
	server->workspaces.current = target;

	struct view *grabbed_view = server->grabbed_view;
	if (grabbed_view) {
		view_move_to_workspace(grabbed_view, target);
	}

	/*
	 * Make sure we are focusing what the user sees. Only refocus if
	 * the focus is not already on an omnipresent view.
	 */
	if (update_focus) {
		struct view *active_view = server->active_view;
		if (!(active_view && active_view->visible_on_all_workspaces)) {
			desktop_focus_topmost_view(server);
		}
	}

	/* And finally show the OSD */
	_osd_show(server);

	/*
	 * Make sure we are not carrying around a
	 * cursor image from the previous desktop
	 */
	cursor_update_focus(server);

	/* Ensure that only currently visible fullscreen windows hide the top layer */
	desktop_update_top_layer_visibility(server);

	lab_cosmic_workspace_set_active(target->cosmic_workspace, true);
	lab_ext_workspace_set_active(target->ext_workspace, true);

	ipc_notify_workspace_changed(server);
}

void
workspaces_osd_hide(struct seat *seat)
{
	assert(seat);
	struct output *output;
	struct server *server = seat->server;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output->workspace_osd) {
			continue;
		}
		wlr_scene_node_set_enabled(&output->workspace_osd->node, false);
		wlr_scene_buffer_set_buffer(output->workspace_osd, NULL);
	}
	seat->workspace_osd_shown_by_modifier = false;

	/* Update the cursor focus in case it was on top of the OSD before */
	cursor_update_focus(server);
}

struct workspace *
workspaces_find(struct workspace *anchor, const char *name, bool wrap)
{
	assert(anchor);
	if (!name) {
		return NULL;
	}
	struct server *server = anchor->server;
	struct wl_list *workspaces = &server->workspaces.all;

	if (!strcasecmp(name, "current")) {
		return anchor;
	} else if (!strcasecmp(name, "last")) {
		return server->workspaces.last;
	} else if (!strcasecmp(name, "left")) {
		return get_prev(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right")) {
		return get_next(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "left-occupied")) {
		return get_prev_occupied(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right-occupied")) {
		return get_next_occupied(anchor, workspaces, wrap);
	}
	return workspace_find_by_name(server, name);
}

static void
destroy_workspace(struct workspace *workspace)
{
	wlr_scene_node_destroy(&workspace->tree->node);
	zfree(workspace->name);
	wl_list_remove(&workspace->link);
	wl_list_remove(&workspace->on_cosmic.activate.link);
	wl_list_remove(&workspace->on_ext.activate.link);

	lab_cosmic_workspace_destroy(workspace->cosmic_workspace);
	lab_ext_workspace_destroy(workspace->ext_workspace);
	free(workspace);
}

void
workspaces_reconfigure(struct server *server)
{
	/*
	 * Compare actual workspace list with the new desired configuration to:
	 *   - Update names
	 *   - Add workspaces if more workspaces are desired
	 *   - Destroy workspaces if fewer workspace are desired
	 */

	struct wl_list *workspace_link = server->workspaces.all.next;
	bool list_changed = false;
	struct wl_list persisted_workspaces;
	bool have_persisted = workspace_config_list_load_persisted(&persisted_workspaces);
	struct wl_list *workspace_source = have_persisted
		? &persisted_workspaces
		: &rc.workspace_config.workspaces;

	struct workspace_config *conf;
	wl_list_for_each(conf, workspace_source, link) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		if (workspace_link == &server->workspaces.all) {
			/* # of configured workspaces increased */
			wlr_log(WLR_DEBUG, "Adding workspace \"%s\"",
				conf->name);
			add_workspace(server, conf->name);
			list_changed = true;
			continue;
		}
		if (strcmp(workspace->name, conf->name)) {
			/* Workspace is renamed */
			wlr_log(WLR_DEBUG, "Renaming workspace \"%s\" to \"%s\"",
				workspace->name, conf->name);
			xstrdup_replace(workspace->name, conf->name);
			lab_cosmic_workspace_set_name(
				workspace->cosmic_workspace, workspace->name);
			lab_ext_workspace_set_name(
				workspace->ext_workspace, workspace->name);
			list_changed = true;
		}
		workspace_link = workspace_link->next;
	}

	if (workspace_link == &server->workspaces.all) {
		if (list_changed) {
			workspace_state_persist(server);
		}
		if (list_changed) {
			ipc_notify_workspace_list_changed(server);
		}
		if (have_persisted) {
			workspace_config_list_destroy(&persisted_workspaces);
		}
		return;
	}

	/* # of configured workspaces decreased */
	overlay_finish(&server->seat);
	struct workspace *first_workspace =
		wl_container_of(server->workspaces.all.next, first_workspace, link);

	while (workspace_link != &server->workspaces.all) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		wlr_log(WLR_DEBUG, "Destroying workspace \"%s\"",
			workspace->name);

		struct view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->workspace == workspace) {
				view_move_to_workspace(view, first_workspace);
			}
		}

		if (server->workspaces.current == workspace) {
			workspaces_switch_to(first_workspace,
				/* update_focus */ true);
		}
		if (server->workspaces.last == workspace) {
			server->workspaces.last = first_workspace;
		}

		workspace_link = workspace_link->next;
		destroy_workspace(workspace);
		list_changed = true;
	}

	if (list_changed) {
		workspace_state_persist(server);
		ipc_notify_workspace_list_changed(server);
	}
	if (have_persisted) {
		workspace_config_list_destroy(&persisted_workspaces);
	}
}

bool
workspaces_add_named(struct server *server, const char *name)
{
	if (!server || !name || !*name) {
		return false;
	}

	add_workspace(server, name);
	workspace_state_persist(server);
	ipc_notify_workspace_list_changed(server);
	return true;
}

bool
workspaces_rename_index(struct server *server, int index, const char *name)
{
	if (!server || !name || !*name) {
		return false;
	}

	struct workspace *workspace = workspace_by_index(server, index);
	if (!workspace) {
		return false;
	}

	if (strcmp(workspace->name, name)) {
		xstrdup_replace(workspace->name, name);
		lab_cosmic_workspace_set_name(workspace->cosmic_workspace, workspace->name);
		lab_ext_workspace_set_name(workspace->ext_workspace, workspace->name);
		workspace_state_persist(server);
		ipc_notify_workspace_list_changed(server);
	}

	return true;
}

bool
workspaces_remove_index(struct server *server, int index)
{
	if (!server) {
		return false;
	}

	size_t count = wl_list_length(&server->workspaces.all);
	if (count <= 1) {
		return false;
	}

	struct workspace *workspace = workspace_by_index(server, index);
	if (!workspace) {
		return false;
	}

	struct workspace *fallback = NULL;
	if (workspace->link.next != &server->workspaces.all) {
		fallback = wl_container_of(workspace->link.next, fallback, link);
	} else {
		fallback = wl_container_of(server->workspaces.all.next, fallback, link);
	}
	if (!fallback || fallback == workspace) {
		return false;
	}

	overlay_finish(&server->seat);

	struct view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->workspace == workspace) {
			view_move_to_workspace(view, fallback);
		}
	}

	if (server->workspaces.current == workspace) {
		workspaces_switch_to(fallback, /* update_focus */ true);
	}
	if (server->workspaces.last == workspace) {
		server->workspaces.last = fallback;
	}

	destroy_workspace(workspace);
	workspace_state_persist(server);
	ipc_notify_workspace_list_changed(server);
	return true;
}

void
workspaces_destroy(struct server *server)
{
	struct workspace *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &server->workspaces.all, link) {
		destroy_workspace(workspace);
	}
	assert(wl_list_empty(&server->workspaces.all));
}
