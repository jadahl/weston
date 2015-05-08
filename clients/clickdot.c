/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2012 Collabora, Ltd.
 * Copyright © 2012 Jonas Ådahl
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <math.h>
#include <assert.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <linux/input.h>
#include <wayland-client.h>

#include "window.h"

#define NUM_COMPLEX_REGION_RECTS 9

static int32_t option_complex_confine_region;
static int32_t option_help;

struct clickdot {
	struct display *display;
	struct window *window;
	struct widget *widget;

	cairo_surface_t *buffer;

	struct {
		int32_t x, y;
	} dot;

	struct {
		int32_t x, y;
		int32_t old_x, old_y;
	} line;
	bool clear_lines;

	int reset;

	struct input *cursor_timeout_input;
	int cursor_timeout_fd;
	struct task cursor_timeout_task;

	bool pointer_confined;

	bool complex_confine_region_enabled;
	bool complex_confine_region_dirty;
	struct rectangle complex_confine_region[NUM_COMPLEX_REGION_RECTS];
};

static void
draw_line(struct clickdot *clickdot, cairo_t *cr,
	  struct rectangle *allocation)
{
	cairo_t *bcr;
	cairo_surface_t *tmp_buffer = NULL;

	if (clickdot->reset) {
		tmp_buffer = clickdot->buffer;
		clickdot->buffer = NULL;
		clickdot->line.x = -1;
		clickdot->line.y = -1;
		clickdot->line.old_x = -1;
		clickdot->line.old_y = -1;
		clickdot->reset = 0;
	}

	if (clickdot->buffer == NULL) {
		clickdot->buffer =
			cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
						   allocation->width,
						   allocation->height);
		bcr = cairo_create(clickdot->buffer);
		cairo_set_source_rgba(bcr, 0, 0, 0, 0);
		cairo_rectangle(bcr,
				0, 0,
				allocation->width, allocation->height);
		cairo_fill(bcr);
	}
	else
		bcr = cairo_create(clickdot->buffer);

	if (tmp_buffer) {
		cairo_set_source_surface(bcr, tmp_buffer, 0, 0);
		cairo_rectangle(bcr, 0, 0,
				allocation->width, allocation->height);
		cairo_clip(bcr);
		cairo_paint(bcr);

		cairo_surface_destroy(tmp_buffer);
	}

	if (clickdot->line.x != -1 && clickdot->line.y != -1) {
		if (clickdot->line.old_x != -1 &&
		    clickdot->line.old_y != -1) {
			cairo_set_line_width(bcr, 2.0);
			cairo_set_source_rgb(bcr, 1, 1, 1);
			cairo_translate(bcr,
					-allocation->x, -allocation->y);

			cairo_move_to(bcr,
				      clickdot->line.old_x,
				      clickdot->line.old_y);
			cairo_line_to(bcr,
				      clickdot->line.x,
				      clickdot->line.y);

			cairo_stroke(bcr);
		}

		clickdot->line.old_x = clickdot->line.x;
		clickdot->line.old_y = clickdot->line.y;
	}
	cairo_destroy(bcr);

	cairo_set_source_surface(cr, clickdot->buffer,
				 allocation->x, allocation->y);
	cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
	cairo_rectangle(cr,
			allocation->x, allocation->y,
			allocation->width, allocation->height);
	cairo_clip(cr);
	cairo_paint(cr);
}

static void
calculate_complex_confine_region(struct clickdot *clickdot)
{
	struct rectangle allocation;
	struct rectangle *rs = clickdot->complex_confine_region;

	if (!clickdot->complex_confine_region_dirty)
		return;

	widget_get_allocation(clickdot->widget, &allocation);

	/*
	 * The code below constructs a region made up of rectangles that
	 * is then used to set up both an illustrative shaded region in the
	 * widget and a confine region used when confining the pointer.
	 */

	rs[0].x = allocation.x + (int)round(allocation.width * 0.05);
	rs[0].y = allocation.y + (int)round(allocation.height * 0.15);
	rs[0].width = (int)round(allocation.width * 0.35);
	rs[0].height = (int)round(allocation.height * 0.7);

	rs[1].x = rs[0].x + rs[0].width;
	rs[1].y = allocation.y + (int)round(allocation.height * 0.45);
	rs[1].width = (int)round(allocation.width * 0.09);
	rs[1].height = (int)round(allocation.height * 0.1);

	rs[2].x = rs[1].x + rs[1].width;
	rs[2].y = allocation.y + (int)round(allocation.height * 0.48);
	rs[2].width = (int)round(allocation.width * 0.02);
	rs[2].height = (int)round(allocation.height * 0.04);

	rs[3].x = rs[2].x + rs[2].width;
	rs[3].y = allocation.y + (int)round(allocation.height * 0.45);
	rs[3].width = (int)round(allocation.width * 0.09);
	rs[3].height = (int)round(allocation.height * 0.1);

	rs[4].x = rs[3].x + rs[3].width;
	rs[4].y = allocation.y + (int)round(allocation.height * 0.15);
	rs[4].width = (int)round(allocation.width * 0.35);
	rs[4].height = (int)round(allocation.height * 0.7);

	rs[5].x = allocation.x + (int)round(allocation.width * 0.05);
	rs[5].y = allocation.y + (int)round(allocation.height * 0.05);
	rs[5].width = rs[0].width + rs[1].width + rs[2].width +
		rs[3].width + rs[4].width;
	rs[5].height = (int)round(allocation.height * 0.10);

	rs[6].x = allocation.x + (int)round(allocation.width * 0.1);
	rs[6].y = rs[4].y + rs[4].height + (int)round(allocation.height * 0.02);
	rs[6].width = (int)round(allocation.width * 0.8);
	rs[6].height = (int)round(allocation.height * 0.03);

	rs[7].x = allocation.x + (int)round(allocation.width * 0.05);
	rs[7].y = rs[6].y + rs[6].height;
	rs[7].width = (int)round(allocation.width * 0.9);
	rs[7].height = (int)round(allocation.height * 0.03);

	rs[8].x = allocation.x + (int)round(allocation.width * 0.1);
	rs[8].y = rs[7].y + rs[7].height;
	rs[8].width = (int)round(allocation.width * 0.8);
	rs[8].height = (int)round(allocation.height * 0.03);

	clickdot->complex_confine_region_dirty = false;
}

static void
draw_complex_confine_region_mask(struct clickdot *clickdot, cairo_t *cr)
{
	int i;

	calculate_complex_confine_region(clickdot);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

	for (i = 0; i < NUM_COMPLEX_REGION_RECTS; i++) {
		cairo_rectangle(cr,
				clickdot->complex_confine_region[i].x,
				clickdot->complex_confine_region[i].y,
				clickdot->complex_confine_region[i].width,
				clickdot->complex_confine_region[i].height);
		cairo_set_source_rgba(cr, 0.14, 0.14, 0.14, 0.9);
		cairo_fill(cr);
	}
}

static void
redraw_handler(struct widget *widget, void *data)
{
	static const double r = 10.0;
	struct clickdot *clickdot = data;
	cairo_surface_t *surface;
	cairo_t *cr;
	struct rectangle allocation;

	widget_get_allocation(clickdot->widget, &allocation);

	surface = window_get_surface(clickdot->window);

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_rectangle(cr,
			allocation.x,
			allocation.y,
			allocation.width,
			allocation.height);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
	cairo_fill(cr);

	if (clickdot->complex_confine_region_enabled) {
		draw_complex_confine_region_mask(clickdot, cr);
	}

	draw_line(clickdot, cr, &allocation);

	cairo_translate(cr, clickdot->dot.x + 0.5, clickdot->dot.y + 0.5);
	cairo_set_line_width(cr, 1.0);
	cairo_set_source_rgb(cr, 0.1, 0.9, 0.9);
	cairo_move_to(cr, 0.0, -r);
	cairo_line_to(cr, 0.0, r);
	cairo_move_to(cr, -r, 0.0);
	cairo_line_to(cr, r, 0.0);
	cairo_arc(cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
	cairo_stroke(cr);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	struct clickdot *clickdot = data;

	window_schedule_redraw(clickdot->window);
}

static void
key_handler(struct window *window, struct input *input, uint32_t time,
	    uint32_t key, uint32_t sym,
	    enum wl_keyboard_key_state state, void *data)
{
	struct clickdot *clickdot = data;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	switch (sym) {
	case XKB_KEY_Escape:
		display_exit(clickdot->display);
		break;
	case XKB_KEY_BackSpace:
		cairo_surface_destroy(clickdot->buffer);
		clickdot->buffer = NULL;
		window_schedule_redraw(clickdot->window);
		break;
	}
}

static void
toggle_pointer_confine(struct clickdot *clickdot, struct input *input)
{
	if (clickdot->pointer_confined) {
		window_unconfine_pointer(clickdot->window);
	} else if (clickdot->complex_confine_region_enabled) {
		calculate_complex_confine_region(clickdot);
		window_confine_pointer_to_rectangles(
				clickdot->window,
				input,
				clickdot->complex_confine_region,
				NUM_COMPLEX_REGION_RECTS);
	} else {
		window_confine_pointer_to_widget(clickdot->window,
						 clickdot->widget,
						 input);
	}

	clickdot->pointer_confined = !clickdot->pointer_confined;
}

static void
button_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       uint32_t button,
	       enum wl_pointer_button_state state, void *data)
{
	struct clickdot *clickdot = data;
	bool is_pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

	if (is_pressed && button == BTN_LEFT)
		input_get_position(input, &clickdot->dot.x, &clickdot->dot.y);
	else if (is_pressed && button == BTN_RIGHT)
		toggle_pointer_confine(clickdot, input);
	widget_schedule_redraw(widget);
}

static void
cursor_timeout_reset(struct clickdot *clickdot)
{
	const long cursor_timeout = 500;
	struct itimerspec its;

	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = cursor_timeout / 1000;
	its.it_value.tv_nsec = (cursor_timeout % 1000) * 1000 * 1000;
	timerfd_settime(clickdot->cursor_timeout_fd, 0, &its, NULL);
}

static int
motion_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       float x, float y, void *data)
{
	struct clickdot *clickdot = data;
	clickdot->line.x = x;
	clickdot->line.y = y;

	window_schedule_redraw(clickdot->window);

	cursor_timeout_reset(clickdot);
	clickdot->cursor_timeout_input = input;

	return CURSOR_BLANK;
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height,
	       void *data)
{
	struct clickdot *clickdot = data;

	clickdot->reset = 1;
	clickdot->complex_confine_region_dirty = true;
}

static void
leave_handler(struct widget *widget,
	      struct input *input, void *data)
{
	struct clickdot *clickdot = data;

	clickdot->reset = 1;
}

static void
cursor_timeout_func(struct task *task, uint32_t events)
{
	struct clickdot *clickdot =
		container_of(task, struct clickdot, cursor_timeout_task);
	uint64_t exp;

	if (read(clickdot->cursor_timeout_fd, &exp, sizeof (uint64_t)) !=
	    sizeof(uint64_t))
		abort();

	input_set_pointer_image(clickdot->cursor_timeout_input,
				CURSOR_LEFT_PTR);
}

static void
pointer_unconfined(struct window *window, struct input *input, void *data)
{
	struct clickdot *clickdot = data;

	clickdot->pointer_confined = false;
}

static struct clickdot *
clickdot_create(struct display *display)
{
	struct clickdot *clickdot;

	clickdot = xzalloc(sizeof *clickdot);
	clickdot->window = window_create(display);
	clickdot->widget = window_frame_create(clickdot->window, clickdot);
	window_set_title(clickdot->window, "Wayland ClickDot");
	clickdot->display = display;
	clickdot->buffer = NULL;

	window_set_key_handler(clickdot->window, key_handler);
	window_set_user_data(clickdot->window, clickdot);
	window_set_keyboard_focus_handler(clickdot->window,
					  keyboard_focus_handler);
	window_set_pointer_unconfined_handler(clickdot->window,
					      pointer_unconfined);

	widget_set_redraw_handler(clickdot->widget, redraw_handler);
	widget_set_button_handler(clickdot->widget, button_handler);
	widget_set_motion_handler(clickdot->widget, motion_handler);
	widget_set_resize_handler(clickdot->widget, resize_handler);
	widget_set_leave_handler(clickdot->widget, leave_handler);

	widget_schedule_resize(clickdot->widget, 500, 400);
	clickdot->dot.x = 250;
	clickdot->dot.y = 200;
	clickdot->line.x = -1;
	clickdot->line.y = -1;
	clickdot->line.old_x = -1;
	clickdot->line.old_y = -1;
	clickdot->reset = 0;

	clickdot->cursor_timeout_fd =
		timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	clickdot->cursor_timeout_task.run = cursor_timeout_func;
	display_watch_fd(window_get_display(clickdot->window),
			 clickdot->cursor_timeout_fd,
			 EPOLLIN, &clickdot->cursor_timeout_task);

	return clickdot;
}

static void
clickdot_destroy(struct clickdot *clickdot)
{
	display_unwatch_fd(window_get_display(clickdot->window),
			   clickdot->cursor_timeout_fd);
	close(clickdot->cursor_timeout_fd);
	if (clickdot->buffer)
		cairo_surface_destroy(clickdot->buffer);
	widget_destroy(clickdot->widget);
	window_destroy(clickdot->window);
	free(clickdot);
}

static const struct weston_option clickdot_options[] = {
	{ WESTON_OPTION_BOOLEAN, "complex-confine-region", 0, &option_complex_confine_region },
	{ WESTON_OPTION_BOOLEAN, "help", 0, &option_help },
};

static void
print_help(const char *argv0)
{
	printf("Usage: %s [--complex-confine-region]\n", argv0);
}

int
main(int argc, char *argv[])
{
	struct display *display;
	struct clickdot *clickdot;

	if (parse_options(clickdot_options,
			  ARRAY_LENGTH(clickdot_options),
			  &argc, argv) > 1 ||
	    option_help) {
		print_help(argv[0]);
		return 0;
	}

	display = display_create(&argc, argv);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	clickdot = clickdot_create(display);

	if (option_complex_confine_region) {
		clickdot->complex_confine_region_dirty = true;
		clickdot->complex_confine_region_enabled = true;
	}

	display_run(display);

	clickdot_destroy(clickdot);
	display_destroy(display);

	return 0;
}
