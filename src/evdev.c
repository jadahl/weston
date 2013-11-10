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

static int
evdev_device_data(int fd, uint32_t mask, void *data)
{
	struct weston_compositor *ec;
	struct evdev_device *device = data;

	ec = device->seat->compositor;
	if (!ec->session_active)
		return 1;

	libinput_device_dispatch(device->device);

	return 0;
}

static void
register_capability(enum libinput_seat_capability capability,
		    void *data)
{
	struct evdev_device *device = data;
	struct weston_seat *seat = device->seat;

	switch (capability) {
	case LIBINPUT_SEAT_CAP_KEYBOARD:
		weston_seat_init_keyboard(seat, NULL);
		break;
	case LIBINPUT_SEAT_CAP_POINTER:
		weston_seat_init_pointer(seat);
		break;
	case LIBINPUT_SEAT_CAP_TOUCH:
		weston_seat_init_touch(seat);
		break;
	};
}

static void
unregister_capability(enum libinput_seat_capability capability,
		      void *data)
{
	struct evdev_device *device = data;
	struct weston_seat *seat = device->seat;

	switch (capability) {
	case LIBINPUT_SEAT_CAP_KEYBOARD:
		weston_seat_release_keyboard(seat);
		break;
	case LIBINPUT_SEAT_CAP_POINTER:
		weston_seat_release_pointer(seat);
		break;
	case LIBINPUT_SEAT_CAP_TOUCH:
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

struct libinput_fd_handle {
	struct wl_event_source *source;
	libinput_fd_callback callback;
	struct evdev_device *device;
};

static int
dispatch_libinput_callback(int fd, uint32_t mask, void *data)
{
	struct libinput_fd_handle *handle = data;

	handle->callback(fd, handle->device->device);

	return 1;
}

static struct libinput_fd_handle *
add_fd(int fd, libinput_fd_callback callback, void *data)
{
	struct evdev_device *device = data;
	struct libinput_fd_handle *handle;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(device->seat->compositor->wl_display);

	handle = malloc(sizeof *handle);
	if (!handle)
		return NULL;

	handle->callback = callback;
	handle->device = device;
	handle->source = wl_event_loop_add_fd(loop,
					      fd,
					      WL_EVENT_READABLE,
					      dispatch_libinput_callback,
					      handle);
	if (!handle->source) {
		free(handle);
		return NULL;
	}

	return handle;
}

static void
remove_fd(struct libinput_fd_handle *handle, void *data)
{
	wl_event_source_remove(handle->source);
	free(handle);
}

static void
device_lost(void *data)
{
	struct evdev_device *device = data;

	wl_event_source_remove(device->source);
	device->source = NULL;
}

static const struct libinput_device_interface device_interface = {
	register_capability,
	unregister_capability,

	get_current_screen_dimensions,

	add_fd,
	remove_fd,

	device_lost,
};

static void
keyboard_notify_key(uint32_t time,
		    uint32_t key,
		    enum libinput_keyboard_key_state state,
		    void *data)
{
	struct evdev_device *device = data;
	enum wl_keyboard_key_state key_state =
		(enum wl_keyboard_key_state) state;

	notify_key(device->seat, time, key, key_state, STATE_UPDATE_AUTOMATIC);
}

static const struct libinput_keyboard_listener keyboard_listener = {
	keyboard_notify_key,
};

static void
pointer_notify_motion(uint32_t time,
		      li_fixed_t dx,
		      li_fixed_t dy,
		      void *data)
{
	struct evdev_device *device = data;

	notify_motion(device->seat, time, dx, dy);
}

static void
pointer_notify_motion_absolute(uint32_t time,
			       li_fixed_t x,
			       li_fixed_t y,
			       void *data)
{
	struct evdev_device *device = data;

	weston_output_transform_coordinate(device->output, x, y, &x, &y);
	notify_motion_absolute(device->seat, time, x, y);
}

static void
pointer_notify_button(uint32_t time,
		      int32_t button,
		      enum libinput_pointer_button_state state,
		      void *data)
{
	struct evdev_device *device = data;
	enum wl_pointer_button_state button_state =
		(enum wl_pointer_button_state) state;

	notify_button(device->seat, time, button, button_state);
}

static void
pointer_notify_axis(uint32_t time,
		    enum libinput_pointer_axis axis,
		    li_fixed_t value,
		    void *data)
{
	struct evdev_device *device = data;

	notify_axis(device->seat, time, (uint32_t) axis, value);
}

static const struct libinput_pointer_listener pointer_listener = {
	pointer_notify_motion,
	pointer_notify_motion_absolute,
	pointer_notify_button,
	pointer_notify_axis,
};

static void
touch_notify_touch(uint32_t time,
		   int32_t slot,
		   li_fixed_t x,
		   li_fixed_t y,
		   enum libinput_touch_type touch_type,
		   void *data)
{
	struct evdev_device *device = data;
	struct weston_seat *master = device->seat;
	uint32_t seat_slot;

	switch (touch_type) {
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
	notify_touch(device->seat, time, seat_slot, x, y, (int) touch_type);
}

static const struct libinput_touch_listener touch_listener = {
	touch_notify_touch,
};

struct evdev_device *
evdev_device_create(struct weston_seat *seat, const char *path, int device_fd)
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

	device->device = libinput_device_create_evdev(path,
						      device_fd,
						      &device_interface,
						      device);
	if (device->device == NULL) {
		free(device);
		return NULL;
	}

	libinput_device_set_keyboard_listener(device->device,
					      &keyboard_listener,
					      device);
	libinput_device_set_pointer_listener(device->device,
					     &pointer_listener,
					     device);
	libinput_device_set_touch_listener(device->device,
					   &touch_listener,
					   device);

	device->source = wl_event_loop_add_fd(ec->input_loop, device->fd,
					      WL_EVENT_READABLE,
					      evdev_device_data, device);
	if (device->source == NULL)
		goto err;

	return device;

err:
	evdev_device_destroy(device);
	return NULL;
}

void
evdev_device_destroy(struct evdev_device *device)
{
	if (device->source)
		wl_event_source_remove(device->source);
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
