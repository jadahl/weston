/*
 * Copyright © 2013 Intel Corporation
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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libinput.h>

#include "compositor.h"
#include "launcher-util.h"
#include "evdev.h"
#include "udev-seat.h"

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

static void
process_events(struct udev_input *input);
static struct udev_seat *
udev_seat_create(struct udev_input *input, const char *seat_name);
static void
udev_seat_destroy(struct udev_seat *seat);

static void
device_added(struct udev_input *input, struct libinput_device *libinput_device)
{
	struct weston_compositor *c;
	struct evdev_device *device;
	struct weston_output *output;
	const char *seat_name;
	const char *output_name;
	struct libinput_seat *libinput_seat;
	struct udev_seat *seat;

	c = input->compositor;
	libinput_seat = libinput_device_get_seat(libinput_device);

	seat_name = libinput_seat_get_name(libinput_seat);
	seat = udev_seat_get_named(input, seat_name);
	if (!seat)
		return;

	device = evdev_device_create(libinput_device, &seat->base);
	if (device == NULL)
		return;

	wl_list_insert(seat->devices_list.prev, &device->link);

	if (seat->base.output && seat->base.pointer)
		weston_pointer_clamp(seat->base.pointer,
				     &seat->base.pointer->x,
				     &seat->base.pointer->y);

	output_name = libinput_device_get_output_name(libinput_device);
	if (output_name) {
		device->output_name = strdup(output_name);
		wl_list_for_each(output, &c->output_list, link)
			if (strcmp(output->name, device->output_name) == 0)
				device->output = output;
	}

	if (!input->suspended)
		weston_seat_repick(&seat->base);
}

static void
udev_seat_remove_devices(struct udev_seat *seat)
{
	struct evdev_device *device, *next;

	wl_list_for_each_safe(device, next, &seat->devices_list, link) {
		evdev_device_destroy(device);
	}
}

void
udev_input_disable(struct udev_input *input)
{
	if (input->suspended)
		return;

	libinput_suspend(input->libinput);
	process_events(input);
	input->suspended = 1;
}

static int
udev_input_process_event(struct libinput_event *event)
{
	struct libinput *libinput = libinput_event_get_target(event).libinput;
	struct libinput_device *libinput_device;
	struct udev_input *input = libinput_get_user_data(libinput);
	struct evdev_device *device;
	int handled = 1;
	struct libinput_event_added_device *added_device_event;
	struct libinput_event_removed_device *removed_device_event;

	switch (libinput_event_get_type(event)) {
	case LIBINPUT_EVENT_ADDED_DEVICE:
		added_device_event =
			(struct libinput_event_added_device *) event;
		libinput_device =
			libinput_event_added_device_get_device(
				added_device_event);
		device_added(input, libinput_device);
		break;
	case LIBINPUT_EVENT_REMOVED_DEVICE:
		removed_device_event =
			(struct libinput_event_removed_device *) event;
		libinput_device =
			libinput_event_removed_device_get_device(
				removed_device_event);
		device = libinput_device_get_user_data(libinput_device);
		evdev_device_destroy(device);
		break;
	default:
		handled = 0;
	}

	return handled;
}

static void
process_event(struct libinput_event *event)
{
	if (udev_input_process_event(event))
		return;
	if (evdev_device_process_event(event))
		return;
}

static void
process_events(struct udev_input *input)
{
	struct libinput_event *event;

	while ((event = libinput_get_event(input->libinput))) {
		process_event(event);
		libinput_event_destroy(event);
	}
}

static int
udev_input_dispatch(struct udev_input *input)
{
	if (libinput_dispatch(input->libinput) != 0)
		fprintf(stderr, "libinput: Failed to dispatch libinput\n");

	process_events(input);

	return 0;
}

static int
libinput_source_dispatch(int fd, uint32_t mask, void *data)
{
	struct udev_input *input = data;

	return udev_input_dispatch(input) != 0;
}

static void
get_current_screen_dimensions(struct libinput_device *libinput_device,
			      int *width,
			      int *height,
			      void *user_data)
{
	struct evdev_device *device =
		libinput_device_get_user_data(libinput_device);

	*width = device->output->current_mode->width;
	*height = device->output->current_mode->height;
}

static int
open_restricted(const char *path, int flags, void *user_data)
{
	struct udev_input *input = user_data;
	struct weston_launcher *launcher = input->compositor->launcher;

	return weston_launcher_open(launcher, path, O_RDWR | O_NONBLOCK);
}

static void
close_restricted(int fd, void *user_data)
{
	struct udev_input *input = user_data;
	struct weston_launcher *launcher = input->compositor->launcher;

	weston_launcher_close(launcher, fd);
}

const struct libinput_interface libinput_interface = {
	open_restricted,
	close_restricted,

	get_current_screen_dimensions,
};

int
udev_input_enable(struct udev_input *input)
{
	struct wl_event_loop *loop;
	struct weston_compositor *c = input->compositor;
	int fd;
	struct udev_seat *seat;
	int devices_found = 0;

	loop = wl_display_get_event_loop(c->wl_display);
	fd = libinput_get_fd(input->libinput);
	input->libinput_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     libinput_source_dispatch, input);
	if (!input->libinput_source) {
		return -1;
	}

	if (input->suspended) {
		if (libinput_resume(input->libinput) != 0) {
			wl_event_source_remove(input->libinput_source);
			input->libinput_source = NULL;
			return -1;
		}
		input->suspended = 0;
		process_events(input);
	}


	wl_list_for_each(seat, &input->compositor->seat_list, base.link) {
		evdev_notify_keyboard_focus(&seat->base, &seat->devices_list);

		if (!wl_list_empty(&seat->devices_list))
			devices_found = 1;
	}

	if (devices_found == 0) {
		weston_log(
			"warning: no input devices on entering Weston. "
			"Possible causes:\n"
			"\t- no permissions to read /dev/input/event*\n"
			"\t- seats misconfigured "
			"(Weston backend option 'seat', "
			"udev device property ID_SEAT)\n");
		return -1;
	}

	return 0;
}

int
udev_input_init(struct udev_input *input, struct weston_compositor *c, struct udev *udev,
		const char *seat_id)
{
	memset(input, 0, sizeof *input);

	input->compositor = c;

	input->libinput = libinput_create_from_udev(&libinput_interface, input,
						    udev, seat_id);
	if (!input->libinput) {
		return -1;
	}
	process_events(input);

	return udev_input_enable(input);
}

void
udev_input_destroy(struct udev_input *input)
{
	struct udev_seat *seat, *next;

	wl_event_source_remove(input->libinput_source);
	wl_list_for_each_safe(seat, next, &input->compositor->seat_list, base.link)
		udev_seat_destroy(seat);
	libinput_destroy(input->libinput);
}

static void
udev_seat_led_update(struct weston_seat *seat_base, enum weston_led leds)
{
	struct udev_seat *seat = (struct udev_seat *) seat_base;
	struct evdev_device *device;

	wl_list_for_each(device, &seat->devices_list, link)
		evdev_led_update(device, leds);
}

static void
notify_output_create(struct wl_listener *listener, void *data)
{
	struct udev_seat *seat = container_of(listener, struct udev_seat,
					      output_create_listener);
	struct evdev_device *device;
	struct weston_output *output = data;

	wl_list_for_each(device, &seat->devices_list, link)
		if (device->output_name &&
		    strcmp(output->name, device->output_name) == 0)
			device->output = output;
}

static struct udev_seat *
udev_seat_create(struct udev_input *input, const char *seat_name)
{
	struct weston_compositor *c = input->compositor;
	struct udev_seat *seat;

	seat = zalloc(sizeof *seat);

	if (!seat)
		return NULL;
	weston_seat_init(&seat->base, c, seat_name);
	seat->base.led_update = udev_seat_led_update;

	seat->output_create_listener.notify = notify_output_create;
	wl_signal_add(&c->output_created_signal,
		      &seat->output_create_listener);

	wl_list_init(&seat->devices_list);

	return seat;
}

static void
udev_seat_destroy(struct udev_seat *seat)
{
	udev_seat_remove_devices(seat);
	if (seat->base.keyboard)
		notify_keyboard_focus_out(&seat->base);
	weston_seat_release(&seat->base);
	wl_list_remove(&seat->output_create_listener.link);
	free(seat);
}

struct udev_seat *
udev_seat_get_named(struct udev_input *input, const char *seat_name)
{
	struct udev_seat *seat;

	wl_list_for_each(seat, &input->compositor->seat_list, base.link) {
		if (strcmp(seat->base.seat_name, seat_name) == 0)
			return seat;
	}

	return udev_seat_create(input, seat_name);
}
