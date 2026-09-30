// SPDX-License-Identifier: GPL-2.0-only
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "placement-registers.h"

static void
test_letter_routes(void **state)
{
	(void)state;
	static const char *keys[] = {"u", "i", "h", "j", "k", "o", "p", "l", ";"};
	for (int i = 0; i < 9; i++) {
		assert_int_equal(placement_register_from_key(keys[i]), i + 1);
	}
	assert_int_equal(placement_register_from_key("1"), 1);
	assert_int_equal(placement_register_from_key("9"), 9);
	assert_int_equal(placement_register_from_key(""), 0);
	assert_int_equal(placement_register_from_key("ij"), 0);
}

static void
test_shapes_and_odd_area(void **state)
{
	(void)state;
	struct wlr_box area = {10, 20, 101, 77};
	assert_int_equal(placement_register_shape(0), 0);
	assert_int_equal(placement_register_shape(10), 0);
	for (int reg = 1; reg <= 9; reg++) {
		assert_int_equal(placement_register_shape(reg), reg <= 2 ? 2 : reg <= 5 ? 3 : 4);
	}
	struct wlr_box a = placement_register_box(area, 1);
	struct wlr_box b = placement_register_box(area, 2);
	assert_int_equal(a.x, 10);
	assert_int_equal(a.width, 50);
	assert_int_equal(b.x, 60);
	assert_int_equal(b.width, 51);
	assert_int_equal(a.height, 77);
	struct wlr_box c = placement_register_box(area, 3);
	struct wlr_box d = placement_register_box(area, 4);
	struct wlr_box e = placement_register_box(area, 5);
	assert_int_equal(c.width + d.width + e.width, 101);
	assert_int_equal(c.x + c.width, d.x);
	assert_int_equal(d.x + d.width, e.x);
	struct wlr_box q1 = placement_register_box(area, 6);
	struct wlr_box q2 = placement_register_box(area, 7);
	struct wlr_box q3 = placement_register_box(area, 8);
	struct wlr_box q4 = placement_register_box(area, 9);
	assert_int_equal(q1.width + q2.width, 101);
	assert_int_equal(q1.height + q3.height, 77);
	assert_int_equal(q1.x + q1.width, q2.x);
	assert_int_equal(q1.y + q1.height, q3.y);
	assert_int_equal(q4.x + q4.width, 111);
	assert_int_equal(q4.y + q4.height, 97);
}

static void
test_symmetric_conflicts(void **state)
{
	(void)state;
	/* Bit n describes actual overlap with register n+1 in the PRD. */
	const unsigned conflicts[9] = {
		0x0ad, 0x15a, 0x0a5, 0x1eb, 0x152,
		0x02d, 0x05a, 0x08d, 0x11a,
	};
	struct wlr_box area = {3, 5, 101, 77};
	for (int i = 0; i < 9; i++) {
		struct wlr_box a = placement_register_box(area, i + 1);
		for (int j = 0; j < 9; j++) {
			struct wlr_box b = placement_register_box(area, j + 1);
			bool expected = (conflicts[i] & (1u << j)) != 0;
			assert_int_equal(placement_boxes_overlap(a, b), expected);
			assert_int_equal(placement_boxes_overlap(a, b),
				placement_boxes_overlap(b, a));
		}
	}
}

static void
test_mixed_canvas_examples(void **state)
{
	(void)state;
	struct wlr_box area = {3, 5, 101, 77};
	struct wlr_box third_left = placement_register_box(area, 3);
	struct wlr_box quarter_upper_right = placement_register_box(area, 7);
	struct wlr_box quarter_lower_right = placement_register_box(area, 9);
	struct wlr_box half_left = placement_register_box(area, 1);
	struct wlr_box half_right = placement_register_box(area, 2);
	struct wlr_box third_right = placement_register_box(area, 5);
	struct wlr_box quarter_upper_left = placement_register_box(area, 6);
	struct wlr_box quarter_lower_left = placement_register_box(area, 8);
	struct wlr_box third_middle = placement_register_box(area, 4);
	assert_false(placement_boxes_overlap(third_left, quarter_upper_right));
	assert_false(placement_boxes_overlap(third_left, quarter_lower_right));
	assert_false(placement_boxes_overlap(quarter_upper_right, quarter_lower_right));
	assert_false(placement_boxes_overlap(half_left, third_right));
	assert_false(placement_boxes_overlap(half_left, quarter_upper_right));
	assert_false(placement_boxes_overlap(half_left, quarter_lower_right));
	assert_false(placement_boxes_overlap(half_right, third_left));
	assert_false(placement_boxes_overlap(half_right, quarter_upper_left));
	assert_false(placement_boxes_overlap(half_right, quarter_lower_left));
	assert_true(placement_boxes_overlap(half_left, third_middle));
	assert_true(placement_boxes_overlap(third_left,
		placement_register_box(area, 6)));
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_letter_routes),
		cmocka_unit_test(test_shapes_and_odd_area),
		cmocka_unit_test(test_symmetric_conflicts),
		cmocka_unit_test(test_mixed_canvas_examples),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
