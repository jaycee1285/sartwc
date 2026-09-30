/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PLACEMENT_REGISTERS_H
#define LABWC_PLACEMENT_REGISTERS_H

#include <stdbool.h>
#include <wlr/util/box.h>

int placement_register_shape(int reg);
int placement_register_from_key(const char *key);
struct wlr_box placement_register_box(struct wlr_box usable, int reg);
bool placement_boxes_overlap(struct wlr_box a, struct wlr_box b);

#endif /* LABWC_PLACEMENT_REGISTERS_H */
