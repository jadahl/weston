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
handle_keyboard_key(struct libinput_device *libinput_device,
		    struct libinput_event_keyboard *keyboard_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);

	notify_key(device->seat,
		   libinput_event_keyboard_get_time(keyboard_event),
		   libinput_event_keyboard_get_key(keyboard_event),
		   libinput_event_keyboard_get_key_state(keyboard_event),
		   STATE_UPDATE_AUTOMATIC);
}

static void
handle_pointer_motion(struct libinput_device *libinput_device,
		      struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);

	notify_motion(device->seat,
		      libinput_event_pointer_get_time(pointer_event),
		      libinput_event_pointer_get_dx(pointer_event),
		      libinput_event_pointer_get_dy(pointer_event));
}

static void
handle_pointer_motion_absolute(
	struct libinput_device *libinput_device,
	struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	uint32_t time = libinput_event_pointer_get_time(pointer_event);
	wl_fixed_t x = libinput_event_pointer_get_absolute_x(pointer_event);
	wl_fixed_t y = libinput_event_pointer_get_absolute_y(pointer_event);

	weston_output_transform_coordinate(device->output, x, y, &x, &y);
	notify_motion_absolute(device->seat, time, x, y);
}

static void
handle_pointer_button(struct libinput_device *libinput_device,
		      struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);

	notify_button(device->seat,
		      libinput_event_pointer_get_time(pointer_event),
		      libinput_event_pointer_get_button(pointer_event),
		      libinput_event_pointer_get_button_state(pointer_event));
}

static void
handle_pointer_axis(struct libinput_device *libinput_device,
		    struct libinput_event_pointer *pointer_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);

	notify_axis(device->seat,
		    libinput_event_pointer_get_time(pointer_event),
		    libinput_event_pointer_get_axis(pointer_event),
		    libinput_event_pointer_get_axis_value(pointer_event));
}

static void
handle_touch_touch(struct libinput_device *libinput_device,
		   struct libinput_event_touch *touch_event)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);
	struct weston_seat *master = device->seat;
	wl_fixed_t x = libinput_event_touch_get_x(touch_event);
	wl_fixed_t y = libinput_event_touch_get_y(touch_event);
	uint32_t slot = libinput_event_touch_get_slot(touch_event);
	uint32_t seat_slot;

	switch (libinput_event_touch_get_touch_type(touch_event)) {
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
		     libinput_event_touch_get_time(touch_event),
		     seat_slot,
		     x, y,
		     libinput_event_touch_get_touch_type(touch_event));
}

int
evdev_device_process_event(struct libinput_event *event)
{
	struct libinput_device *libinput_device =
		libinput_event_get_device(event);
	int handled = 1;

	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		handle_keyboard_key(libinput_device,
				    libinput_event_get_keyboard_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION:
		handle_pointer_motion(libinput_device,
				      libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
		handle_pointer_motion_absolute(
			libinput_device,
			libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		handle_pointer_button(libinput_device,
				      libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_POINTER_AXIS:
		handle_pointer_axis(libinput_device,
				    libinput_event_get_pointer_event(event));
		break;
	case LIBINPUT_EVENT_TOUCH_TOUCH:
		handle_touch_touch(libinput_device,
				   libinput_event_get_touch_event(event));
		break;
	default:
		handled = 0;
		weston_log("unknown libinput event %d\n",
			   libinput_event_get_type(event));
	}

	return handled;
}

static void
notify_output_destroy(struct wl_listener *listener, void *data)
{
	struct evdev_device *device =
		container_of(listener,
			     struct evdev_device, output_destroy_listener);
	struct weston_compositor *c = device->seat->compositor;
	struct weston_output *output;

	if (device->output_name) {
		output = container_of(c->output_list.next,
				      struct weston_output, link);
		evdev_device_set_output(device, output);
	} else {
		device->output = NULL;
	}
}

void
evdev_device_set_output(struct evdev_device *device,
			struct weston_output *output)
{
	device->output = output;
	device->output_destroy_listener.notify = notify_output_destroy;
	wl_signal_add(&output->destroy_signal,
		      &device->output_destroy_listener);
}

struct evdev_device *
evdev_device_create(struct libinput_device *libinput_device,
		    struct weston_seat *seat)
{
	struct evdev_device *device;
	struct weston_compositor *ec;

	device = zalloc(sizeof *device);
	if (device == NULL)
		return NULL;

	ec = seat->compositor;
	device->output =
		container_of(ec->output_list.next, struct weston_output, link);
	device->seat = seat;
	wl_list_init(&device->link);
	device->device = libinput_device;

	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_KEYBOARD)) {
		weston_seat_init_keyboard(seat, NULL);
		device->seat_caps |= EVDEV_SEAT_KEYBOARD;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_POINTER)) {
		weston_seat_init_pointer(seat);
		device->seat_caps |= EVDEV_SEAT_POINTER;
	}
	if (libinput_device_has_capability(libinput_device,
					   LIBINPUT_DEVICE_CAP_TOUCH)) {
		weston_seat_init_touch(seat);
		device->seat_caps |= EVDEV_SEAT_TOUCH;
	}

	libinput_device_set_user_data(libinput_device, device);
	libinput_device_ref(libinput_device);

	return device;
}

void
evdev_device_destroy(struct evdev_device *device)
{
	if (device->seat_caps & EVDEV_SEAT_KEYBOARD)
		weston_seat_release_keyboard(device->seat);
	if (device->seat_caps & EVDEV_SEAT_POINTER)
		weston_seat_release_pointer(device->seat);
	if (device->seat_caps & EVDEV_SEAT_TOUCH)
		weston_seat_release_touch(device->seat);

	if (device->output)
		wl_list_remove(&device->output_destroy_listener.link);
	wl_list_remove(&device->link);
	libinput_device_unref(device->device);
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
