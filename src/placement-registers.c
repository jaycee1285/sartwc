// SPDX-License-Identifier: GPL-2.0-only
#include "placement-registers.h"
#include <stdint.h>

int
placement_register_from_key(const char *key)
{
	if (!key || !key[0] || key[1]) {
		return 0;
	}
	static const char keys[] = "uihjkopl;";
	for (int i = 0; i < 9; i++) {
		if (key[0] == keys[i]) {
			return i + 1;
		}
	}
	return key[0] >= '1' && key[0] <= '9' ? key[0] - '0' : 0;
}

int
placement_register_shape(int reg)
{
	if (reg >= 1 && reg <= 2) {
		return 2;
	}
	if (reg >= 3 && reg <= 5) {
		return 3;
	}
	if (reg >= 6 && reg <= 9) {
		return 4;
	}
	return 0;
}

struct wlr_box
placement_register_box(struct wlr_box usable, int reg)
{
	struct wlr_box box = {0};
	if (!placement_register_shape(reg) || usable.width <= 0 || usable.height <= 0) {
		return box;
	}
	int x0 = usable.x;
	int x1 = usable.x + usable.width / 2;
	int x2 = usable.x + (int)((int64_t)usable.width * 2 / 3);
	int x3 = usable.x + usable.width;
	int y0 = usable.y;
	int y1 = usable.y + usable.height / 2;
	int y2 = usable.y + usable.height;
	switch (reg) {
	case 1: box = (struct wlr_box){x0, y0, x1 - x0, y2 - y0}; break;
	case 2: box = (struct wlr_box){x1, y0, x3 - x1, y2 - y0}; break;
	case 3: box = (struct wlr_box){x0, y0, usable.width / 3, y2 - y0}; break;
	case 4: box = (struct wlr_box){x0 + usable.width / 3, y0,
		x2 - (x0 + usable.width / 3), y2 - y0}; break;
	case 5: box = (struct wlr_box){x2, y0, x3 - x2, y2 - y0}; break;
	case 6: box = (struct wlr_box){x0, y0, x1 - x0, y1 - y0}; break;
	case 7: box = (struct wlr_box){x1, y0, x3 - x1, y1 - y0}; break;
	case 8: box = (struct wlr_box){x0, y1, x1 - x0, y2 - y1}; break;
	case 9: box = (struct wlr_box){x1, y1, x3 - x1, y2 - y1}; break;
	}
	return box;
}

bool
placement_boxes_overlap(struct wlr_box a, struct wlr_box b)
{
	return a.x < b.x + b.width && b.x < a.x + a.width
		&& a.y < b.y + b.height && b.y < a.y + a.height;
}
