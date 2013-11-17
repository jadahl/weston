/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <mtdev.h>
#include <assert.h>
#include <libinput.h>

#include "compositor.h"
#include "evdev.h"

#define DEFAULT_AXIS_STEP_DISTANCE wl_fixed_from_int(10)

void
evdev_led_update(struct evdev_device *device, enum weston_led weston_leds)
{
	enum libinput_led leds = 0;

	if (weston_leds & LED_NUM_LOCK)
		leds |= LIBINPUT_LED_NUM_LOCK;
	if (weston_leds & LED_CAPS_LOCK)
		leds |= LIBINPUT_LED_CAPS_LOCK;
	if (weston_leds & LED_SCROLL_LOCK)
		leds |= LIBINPUT_LED_SCROLL_LOCK;

	libinput_device_led_update(device->device, leds);
}

static void
handle_keyboard_key(struct evdev_device *device,
		    struct libinput_event_keyboard_key *key_event)
{
	notify_key(device->seat,
		   key_event->time,
		   key_event->key,
		   key_event->state,
		   STATE_UPDATE_AUTOMATIC);
}

static void
handle_pointer_motion(struct evdev_device *device,
		      struct libinput_event_pointer_motion *motion_event)
{
	notify_motion(device->seat,
		      motion_event->time,
		      motion_event->dx,
		      motion_event->dy);
}

static void
handle_pointer_motion_absolute(
	struct evdev_device *device,
	struct libinput_event_pointer_motion_absolute *motion_absolute_event)
{
	wl_fixed_t x = motion_absolute_event->x;
	wl_fixed_t y = motion_absolute_event->y;

	weston_output_transform_coordinate(device->output, x, y, &x, &y);
	notify_motion_absolute(device->seat,
			       motion_absolute_event->time,
			       x, y);
}

static void
handle_pointer_button(struct evdev_device *device,
		      struct libinput_event_pointer_button *button_event)
{
	notify_button(device->seat,
		      button_event->time,
		      button_event->button,
		      button_event->state);
}

static void
handle_pointer_axis(struct evdev_device *device,
		    struct libinput_event_pointer_axis *axis_event)
{
	notify_axis(device->seat,
		    axis_event->time,
		    axis_event->axis,
		    axis_event->value);
}

static void
handle_touch_touch(struct evdev_device *device,
		   struct libinput_event_touch_touch *touch_event)
{
	struct weston_seat *master = device->seat;
	wl_fixed_t x = touch_event->x;
	wl_fixed_t y = touch_event->y;
	uint32_t slot = touch_event->slot;
	uint32_t seat_slot;

	switch (touch_event->touch_type) {
	case LIBINPUT_TOUCH_TYPE_DOWN:
		seat_slot = ffs(~master->slot_map) - 1;
		device->mt_slots[slot] = seat_slot;
		master->slot_map |= 1 << seat_slot;
		break;
	case LIBINPUT_TOUCH_TYPE_UP:
		seat_slot = device->mt_slots[slot];
		master->slot_map &= ~(1 << seat_slot);
		break;
	default:
		seat_slot = device->mt_slots[slot];
		break;
	}

	weston_output_transform_coordinate(device->output,
					   x, y, &x, &y);
	notify_touch(device->seat,
		     touch_event->time,
		     seat_slot,
		     x, y,
		     touch_event->touch_type);
}

void
evdev_device_process_event(struct libinput_event *event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(event->device);

	switch (event->type) {
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		handle_keyboard_key(
			device,
			(struct libinput_event_keyboard_key *) event);
		break;
	case LIBINPUT_EVENT_POINTER_MOTION:
		handle_pointer_motion(
			device,
			(struct libinput_event_pointer_motion *) event);
		break;
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
		handle_pointer_motion_absolute(
			device,
			(struct libinput_event_pointer_motion_absolute *) event);
		break;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		handle_pointer_button(
			device,
			(struct libinput_event_pointer_button *) event);
		break;
	case LIBINPUT_EVENT_POINTER_AXIS:
		handle_pointer_axis(
			device,
			(struct libinput_event_pointer_axis *) event);
		break;
	case LIBINPUT_EVENT_TOUCH_TOUCH:
		handle_touch_touch(
			device,
			(struct libinput_event_touch_touch *) event);
		break;
	default:
		weston_log("unknown libinput event %d\n", event->type);
	}
}

static void
register_capability(enum libinput_device_capability capability,
		    void *data)
{
	struct evdev_device *device = data;
	struct weston_seat *seat = device->seat;

	switch (capability) {
	case LIBINPUT_DEVICE_CAP_KEYBOARD:
		weston_seat_init_keyboard(seat, NULL);
		break;
	case LIBINPUT_DEVICE_CAP_POINTER:
		weston_seat_init_pointer(seat);
		break;
	case LIBINPUT_DEVICE_CAP_TOUCH:
		weston_seat_init_touch(seat);
		break;
	};
}

static void
unregister_capability(enum libinput_device_capability capability,
		      void *data)
{
	struct evdev_device *device = data;
	struct weston_seat *seat = device->seat;

	switch (capability) {
	case LIBINPUT_DEVICE_CAP_KEYBOARD:
		weston_seat_release_keyboard(seat);
		break;
	case LIBINPUT_DEVICE_CAP_POINTER:
		weston_seat_release_pointer(seat);
		break;
	case LIBINPUT_DEVICE_CAP_TOUCH:
		weston_seat_release_touch(seat);
		break;
	};
}

static void
get_current_screen_dimensions(int *width,
			      int *height,
			      void *data)
{
	struct evdev_device *device = data;

	*width = device->output->current_mode->width;
	*height = device->output->current_mode->height;
}

static const struct libinput_device_interface device_interface = {
	register_capability,
	unregister_capability,

	get_current_screen_dimensions,
};

struct evdev_device *
evdev_device_create(struct libinput *libinput,
		    struct weston_seat *seat,
		    const char *path,
		    int device_fd)
{
	struct evdev_device *device;
	struct weston_compositor *ec;

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;

	device->devnode = strdup(path);
	device->fd = device_fd;
	ec = seat->compositor;
	device->output =
		container_of(ec->output_list.next, struct weston_output, link);
	device->seat = seat;
	wl_list_init(&device->link);

	device->device = libinput_device_create_evdev(libinput,
						      path,
						      device_fd,
						      &device_interface,
						      device);
	if (device->device == NULL) {
		free(device);
		return NULL;
	}

	return device;
}

void
evdev_device_destroy(struct evdev_device *device)
{
	wl_list_remove(&device->link);

	libinput_device_destroy(device->device);

	free(device->devnode);
	free(device->output_name);
	free(device);
}

void
evdev_notify_keyboard_focus(struct weston_seat *seat,
			    struct wl_list *evdev_devices)
{
	struct evdev_device *device;
	struct wl_array keys;
	unsigned int i, set;
	char evdev_keys[(KEY_CNT + 7) / 8];
	char all_keys[(KEY_CNT + 7) / 8];
	uint32_t *k;
	int ret;

	if (!seat->keyboard_device_count > 0)
		return;

	memset(all_keys, 0, sizeof all_keys);
	wl_list_for_each(device, evdev_devices, link) {
		memset(evdev_keys, 0, sizeof evdev_keys);
		ret = libinput_device_get_keys(device->device,
					       evdev_keys,
					       sizeof evdev_keys);
		if (ret < 0) {
			weston_log("failed to get keys for device %s\n",
				device->devnode);
			continue;
		}
		for (i = 0; i < ARRAY_LENGTH(evdev_keys); i++)
			all_keys[i] |= evdev_keys[i];
	}

	wl_array_init(&keys);
	for (i = 0; i < KEY_CNT; i++) {
		set = all_keys[i >> 3] & (1 << (i & 7));
		if (set) {
			k = wl_array_add(&keys, sizeof *k);
			*k = i;
		}
	}

	notify_keyboard_focus_in(seat, &keys, STATE_UPDATE_AUTOMATIC);

	wl_array_release(&keys);
}
