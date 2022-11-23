#define	_POSIX_C_SOURCE	200112L
#define CGLM_DEFINE_PRINTS
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <pixman-1/pixman.h>
#include <vulkan/vulkan.h>
#include <drm_fourcc.h>

#define CGLM_CLIPSPACE_INCLUDE_ALL
#include <cglm/cglm.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xwayland.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#define PHYSAC_IMPLEMENTATION
#define PHYSAC_STANDALONE
#include "physac.h"

#include "vulkan.h"
#include "render.h"
#include "util.h"
#include "render/vulkan.h"

#define PHYSAC_BOUNDARY_THICKNESS	10

/* For brevity's sake, struct members are annotated where they are used. */
enum CursorMode	{
	VKWC_CURSOR_PASSTHROUGH,
	VKWC_CURSOR_MOVE,
	VKWC_CURSOR_RESIZE,
	VKWC_CURSOR_XY_ROTATE,
	VKWC_CURSOR_Z_ROTATE,
	VKWC_CURSOR_X_ROTATE_SPEED,
	VKWC_CURSOR_Y_ROTATE_SPEED,
	VKWC_CURSOR_Z_ROTATE_SPEED,
	VKWC_CURSOR_X_MOVE,
	VKWC_CURSOR_Y_MOVE,
	VKWC_CURSOR_Z_MOVE,
};

enum CursorMode TRANSFORM_MODES[] = {
	VKWC_CURSOR_XY_ROTATE,
	VKWC_CURSOR_Z_ROTATE,
	VKWC_CURSOR_X_ROTATE_SPEED,
	VKWC_CURSOR_Y_ROTATE_SPEED,
	VKWC_CURSOR_Z_ROTATE_SPEED,
	VKWC_CURSOR_X_MOVE,
	VKWC_CURSOR_Y_MOVE,
	VKWC_CURSOR_Z_MOVE,
};

xkb_keysym_t TRANSFORM_KEYS[] = {
	XKB_KEY_q,
	XKB_KEY_e,
	XKB_KEY_a,
	XKB_KEY_s,
	XKB_KEY_d,
	XKB_KEY_z,
	XKB_KEY_x,
	XKB_KEY_c,
};

struct Server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;
	struct wl_listener handle_xdg_map;
	struct wl_listener handle_new_subsurface;
	struct wl_listener handle_subsurface_map;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat	*seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum CursorMode	cursor_mode;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	struct Surface *grabbed_surface;

	struct wlr_output *output;
	struct wlr_output_layout *output_layout;	// Even though we only support one output, the screencopy API
							// requires this
	struct wl_listener output_frame;
	struct wl_listener new_output;

	struct wl_listener new_xwayland_surface;

	struct wl_list windows;

	struct wl_listener new_surface;
	struct wl_list surfaces;

	// We have to update the position of this if the screen size changes
	PhysicsBody floor;
};

struct Keyboard	{
	struct wl_list link;
	struct Server *server;
	struct wlr_input_device	*device;

	struct wl_listener modifiers;
	struct wl_listener key;
};

static void surface_handle_destroy(struct wl_listener *listener, void *data) {
	struct Surface *surface = wl_container_of(listener, surface, destroy);

	// TODO: Why does this make it crash? :((
	/*
	if (surface->body != NULL) {
		DestroyPhysicsBody(surface->body);
	}
	*/
	if (surface->body != NULL) {
		surface->body->position.x = 10000;
		surface->body->position.y = 10000;
		surface->body->enabled = false;
	}

	wl_list_remove(&surface->link);
	wl_list_remove(&surface->destroy.link);

	printf("Surface destroyed!\n");

	free(surface);
}

// When windows are resized, their projection matrices in their Surfaces must be updated.
// This will recalculate the matrices of the specified node and all children
// x and y is the position of the parent node, since a surface only knows its position relative to its parent
void calc_matrices(struct wl_list *surfaces, int output_width, int output_height) {
	struct Surface *surface;
	wl_list_for_each(surface, surfaces, link) {
		surface->x_rot += surface->x_rot_speed;
		surface->y_rot += surface->y_rot_speed;
		surface->z_rot += surface->z_rot_speed;

		// If physics is enabled, clobber position and rotation with physics values
		if (surface->apply_physics && surface->body != NULL) {
			surface->x_offset = 0;
			surface->y_offset = 0;
			surface->z_offset = 0;
			surface->x = surface->body->position.x - output_width * 0.5;
			surface->y = surface->body->position.y - output_height * 0.5;
			surface->z_rot = surface->body->orient;
			surface->x_rot = 0;
			surface->y_rot = 0;
		}

		assert(surface->toplevel != NULL);

		// If physics is applied, transforms shouldn't be relative to the toplevel
		bool is_toplevel = surface->toplevel == surface;
		if (is_toplevel || surface->apply_physics) {
			glm_mat4_identity(surface->matrix);

			mat4 view;
			mat4 projection;
			glm_perspective_rh_zo(1, (float) output_width / (float) output_height, 1, 10000, projection);

			vec3 eye = {0, 0, 1000};
			vec3 center = {0, 0, 0};
			vec3 up = {0, 1, 0};
			glm_lookat_rh_zo(eye, center, up, view);

			glm_mat4_mul(surface->matrix, projection, surface->matrix);
			glm_mat4_mul(surface->matrix, view, surface->matrix);

			// These are in backwards order
			// Move it
			glm_translate(surface->matrix, (vec3) {
				surface->x + surface->x_offset,
				surface->y + surface->y_offset,
				surface->z_offset
			});
			// Rotate it
			glm_rotate_x(surface->matrix, surface->x_rot, surface->matrix);
			glm_rotate_y(surface->matrix, surface->y_rot, surface->matrix);
			glm_rotate_z(surface->matrix, surface->z_rot, surface->matrix);
			// Move it so its 0, 0 is at the center
			glm_translate(surface->matrix, (vec3) {-0.5 * surface->width, -0.5 * surface->height, 0.0});
			// Scale from 0..1, 0..1 to surface->width, surface->height
			glm_scale(surface->matrix, (vec3) {surface->width, surface->height, 1.0});

			vec4 top_left = {-1, 1, 0, 1};
			vec4 dst;
			glm_mat4_mulv(surface->matrix, top_left, dst);
		} else {
			// First we translate ourselves relative to toplevel, then apply toplevel transform
			// This allows for child transforms to be relative to parent transform
			struct Surface *toplevel = surface->toplevel;

			glm_mat4_identity(surface->matrix);

			glm_mat4_mul(surface->toplevel->matrix, surface->matrix, surface->matrix);

			glm_translate(surface->matrix, (vec3) {
				((float) surface->x - toplevel->x) / toplevel->width,
				((float) surface->y - toplevel->y) / toplevel->height,
				0,
			});

			glm_scale(surface->matrix, (vec3) {(float) surface->width / toplevel->width,
				(float) surface->height / toplevel->height, 1});
		}
	}
}

// We require the cursor position so we can spawn them wherever the cursor is
void create_bodies(struct wl_list *surfaces, struct wlr_scene_node *node, int cursor_x, int cursor_y) {
	if (node->type == WLR_SCENE_NODE_SURFACE) {
		// Find the Surface this node corresponds to
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_from_node(node);
		struct wlr_surface *wlr_surface = scene_surface->surface;
		struct Surface *surface = find_surface(wlr_surface, surfaces);
		assert(surface != NULL);

		if (surface->body == NULL && surface->width >= 1 && surface->height >= 1) {
			float x = cursor_x + surface->x + surface->x_offset;
			float y = cursor_y + surface->y + surface->y_offset;
			float width = surface->width, height = surface->height;
			surface->body = CreatePhysicsBodyRectangle((Vector2) {x + width / 2, y + height / 2},
				width, height, 1);
			surface->body->restitution = 0;
		};
	}

	struct wlr_scene_node *cur;
	wl_list_for_each(cur, &node->state.children, state.link) {
		create_bodies(surfaces, cur, cursor_x, cursor_y);
	};
}

void check_uv(struct Server *server, int cursor_x, int cursor_y,
        	struct Surface **surface_out, int *surface_x, int *surface_y) {
	// Checks the UV texture to see what's under the cursor. Returns the surface under the cursor and the x
	// and y relative to this surface.
	// Returns NULL to surface if there is no surface under the cursor.
	
	// There are multiple render buffers, so we have to find the right one. I do this just by checking whether
	// the render buffer's dimensions match those of the first output, which isn't a great way but works for now.
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) server->renderer;
	struct wlr_vk_render_buffer *render_buffer = NULL;
	struct wlr_output *output = server->output;

	struct wlr_vk_render_buffer *cur;
	wl_list_for_each(cur, &renderer->render_buffers, link) {
		if (cur->wlr_buffer->width == output->width
				&& cur->wlr_buffer->height == output->height) {
			if (render_buffer == NULL || render_buffer->frame < cur->frame) {
				// Always choose the most recent one
				render_buffer = cur;
			}
		}
	};
	assert(render_buffer != NULL);

	// Map the depth buffer
	int width = render_buffer->wlr_buffer->width, height = render_buffer->wlr_buffer->height;

	VkDeviceSize uv_byte_count = width * height * 8;
	void *uv_mem;
	vkMapMemory(renderer->dev->dev, render_buffer->host_uv_mem, 0, uv_byte_count, 0, &uv_mem);
	struct { uint16_t r; uint16_t g; uint16_t b; uint16_t a; } *pixel = uv_mem;

	float pixel_surface_id = (double) pixel[0].b / UINT16_MAX;
	double pixel_x_norm = (double) pixel[0].r / UINT16_MAX;
	double pixel_y_norm = (double) pixel[0].g / UINT16_MAX;
	double error_margin = 1.0 / 65536;

	vkUnmapMemory(renderer->dev->dev, render_buffer->host_uv_mem);

	//printf("id, x, y: %f %f %f\n", pixel_surface_id, pixel_x_norm, pixel_y_norm);

	// Close to 0 means the cursor is above the background, so no surface
	if (pixel_surface_id < error_margin) {
		//printf("ID is close to zero. Exit.\n");
		*surface_out = NULL;
		return;
	}

	// Otherwise, go through all surfaces until we find the one with a matching id
	bool found_surface = false;
	struct Surface *surface = NULL;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->id - error_margin < pixel_surface_id && surface->id + error_margin > pixel_surface_id) {
			//printf("Surface with id %f matches\n", surface->id);
			found_surface = true;
			break;
		}
	}

	if (!found_surface) {
		fprintf(stderr, "Could not find surface with id matching: %f\n", pixel_surface_id);
		exit(1);
	}

	// Set return values
	*surface_out = surface;
	if (surface_x != NULL && surface_y != NULL) {
		*surface_x = pixel_x_norm * surface->width;
		*surface_y = pixel_y_norm * surface->height;
	}
}

static void handle_keyboard_modifiers(struct wl_listener *listener, void *data) {
	/* This	event is raised	when a modifier	key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct Keyboard	*keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have	one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to	the
	 * same	seat. You can swap out the underlying wlr_keyboard like	this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	/* Send	modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->device->keyboard->modifiers);
}

static void focus_surface(struct wlr_seat *seat, struct Surface *surface) {
	// Should only call this on toplevel Surfaces
	assert(surface->toplevel == surface);

	struct wlr_xdg_surface *xdg_surface = surface->xdg_surface;
	assert(xdg_surface != NULL);

	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);
	wlr_xdg_toplevel_set_activated(xdg_surface, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	assert(keyboard != NULL);
	wlr_seat_keyboard_notify_enter(seat, surface->wlr_surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

static bool handle_keybinding(struct Server *server, xkb_keysym_t sym) {
	/*
	 * Here	we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than	passing	them on	to the client for its own
	 * processing.
	 *
	 * This	function assumes Alt is	held down.
	 */
	if (sym == XKB_KEY_Escape) {
		wl_display_terminate(server->wl_display);
	} else if (sym == XKB_KEY_F1) {
		// Focus the next view
		// TODO: Make it actually cycle instead of always taking the last

		struct Surface *surface;
		wl_list_for_each(surface, &server->surfaces, link) {
			if (surface->toplevel == surface) focus_surface(server->seat, surface);
		}

		return true;
	} else if (sym == XKB_KEY_F2) {
		if (fork() == 0) {
			const char *arg[] = {"foot", NULL };
			setsid();
			execvp(((char **)arg)[0], (char	**)arg);
			fprintf(stderr,	"vkwc: execvp %s", ((char **)arg)[0]);
			perror(" failed");
			exit(EXIT_SUCCESS);
		}
		return true;
	} else if (sym == XKB_KEY_F4) {
		if (fork() == 0) {
			const char *arg[] = {"my-chvt", "1", NULL };
			setsid();
			execvp(((char **)arg)[0], (char	**)arg);
			fprintf(stderr,	"vkwc: execvp %s", ((char **)arg)[0]);
			perror(" failed");
			exit(EXIT_SUCCESS);
		}
		return true;
	} else if (sym == XKB_KEY_F9) {
		struct Surface *surface;
		check_uv(server, server->cursor->x, server->cursor->y, &surface, NULL, NULL);
		if (surface != NULL) {
			surface->x_rot_speed = 0;
			surface->y_rot_speed = 0;
			surface->z_rot_speed = 0;
		}
		return true;
	} else if (sym == XKB_KEY_F10) {
		struct Surface *surface;
		check_uv(server, server->cursor->x, server->cursor->y, &surface, NULL, NULL);
		if (surface != NULL) {
			surface->x_rot = 0;
			surface->y_rot = 0;
			surface->z_rot = 0;
		}
		return true;
	} else if (sym == XKB_KEY_r) {
		struct wlr_vk_renderer *vk_renderer = (struct wlr_vk_renderer *) server->renderer;
		vk_renderer->render_mode = (vk_renderer->render_mode + 1) % WLR_VK_RENDER_MODE_COUNT;
	}

	assert(sizeof(TRANSFORM_MODES) / sizeof(TRANSFORM_MODES[0]) == sizeof(TRANSFORM_KEYS) / sizeof(TRANSFORM_KEYS[0]));
	for (int i = 0; i < sizeof(TRANSFORM_MODES) / sizeof(TRANSFORM_MODES[0]); i++) {
		enum CursorMode mode = TRANSFORM_MODES[i];
		xkb_keysym_t key = TRANSFORM_KEYS[i];

		if (sym == key) {
			if (server->cursor_mode == mode) {
				server->grabbed_surface = NULL;
				server->cursor_mode = VKWC_CURSOR_PASSTHROUGH;
			} else {
				check_uv(server, server->cursor->x, server->cursor->y,
					&server->grabbed_surface, NULL, NULL);
				if (server->grabbed_surface != NULL) {
					server->grabbed_surface = server->grabbed_surface->toplevel;
					server->cursor_mode = mode;
				} else {
					printf("No surface under cursor\n");
					return false;
				}
			}
			return true;
		}
	}

	return false;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	/* This	event is raised	when a key is pressed or released. */
	struct Keyboard	*keyboard =
		wl_container_of(listener, keyboard, key);
	struct Server *server =	keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat	*seat =	server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->device->keyboard->xkb_state, keycode,	&syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state ==	WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this	button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0;	i < nsyms; i++)	{
			handled	= handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode,	event->state);
	}
}

static void server_new_keyboard(struct Server *server, struct wlr_input_device *device)	{
	struct Keyboard	*keyboard =
		calloc(1, sizeof(struct	Keyboard));
	keyboard->server = server;
	keyboard->device = device;

	/* We need to prepare an XKB keymap and	assign it to the keyboard. This
	 * assumes the defaults	(e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(device->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	/* Here	we set up listeners for	keyboard events. */
	keyboard->modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = handle_keyboard_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard	to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct Server *server,
		struct wlr_input_device	*device) {
	/* We don't do anything	special	with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take	this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void handle_new_input(struct wl_listener	*listener, void	*data) {
	/* This	event is raised	by the backend when a new input	device becomes
	 * available. */
	struct Server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device	*device	= data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the	client.	In TinyWL we always have a cursor, even	if
	 * there are no	pointer	devices, so we always include that capability. */
	uint32_t caps =	WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))	{
		caps |=	WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat,	caps);
}

static void handle_new_cursor_image(struct wl_listener *listener, void *data) {
	struct Server *server =	wl_container_of(
			listener, server, request_cursor);
	/* This	event is raised	by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event	= data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This	can be sent by any client, so we check to make sure this one is
	 * actually has	pointer	focus first. */
	if (focused_client == event->seat_client) {
		/* Once	we've vetted the client, we can	tell the cursor	to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on	and continue to	do so as the
		 * cursor moves	between	outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void handle_selection_request(struct wl_listener	*listener, void	*data) {
	/* This	event is raised	by the seat when a client wants	to set the selection,
	 * usually when	the user copies	something. wlroots allows compositors to
	 * ignore such requests	if they	so choose, but in vkwc we always honor
	 */
	struct Server *server =	wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void process_cursor_motion(struct Server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions.	*/
	if (server->cursor_mode	== VKWC_CURSOR_MOVE) {
		fprintf(stderr, "AHHHHH I don't know how to move things!!\n");
		return;
	} else if (server->cursor_mode == VKWC_CURSOR_RESIZE) {
		fprintf(stderr, "AHHHHH I don't know how to resize things!!\n");
		return;
	}

	/* Otherwise, find the Surface under the pointer and send the event along.	*/
	struct wlr_seat	*seat =	server->seat;

	struct Surface *surface;
	int surface_x, surface_y;	// Cursor position relative to surface
	check_uv(server, server->cursor->x, server->cursor->y, &surface, &surface_x, &surface_y);

	if (surface == NULL) {
		// If there's no view under the	cursor,	set the	cursor image to	a
		// default. This is what makes the cursor image	appear when you	move it
		// around the screen, not over any views.
		wlr_xcursor_manager_set_cursor_image(
				server->cursor_mgr, "left_ptr",	server->cursor);
	} else {
		//
		// Send	pointer	enter and motion events.
		//
		// The enter event gives the surface "pointer focus", which is distinct
		// from	keyboard focus.	You get	pointer	focus by moving	the pointer over
		// a window.
		//
		// Note	that wlroots will avoid	sending	duplicate enter/motion events if
		// the surface has already has pointer focus or	if the client is already
		// aware of the	coordinates passed.
		wlr_seat_pointer_notify_enter(seat, surface->wlr_surface, surface_x, surface_y);
		wlr_seat_pointer_notify_motion(seat, time, surface_x, surface_y);
		wlr_seat_pointer_notify_frame(server->seat);
	}
}

static void handle_cursor_motion_relative(struct wl_listener *listener,	void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits a	_relative_
	 * pointer motion event	(i.e. a	delta) */
	struct Server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion	*event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining	the motion to the output layout, as well as any
	 * special configuration applied for the specific input	device which
	 * generated the event.	You can	pass NULL for the device if you	want to	move
	 * the cursor around without any input.	*/
	wlr_cursor_move(server->cursor,	event->device, event->delta_x, event->delta_y);

	// If we're in a transform mode, don't bother processing the motion
	if (server->grabbed_surface != NULL) {
		if (server->cursor_mode == VKWC_CURSOR_XY_ROTATE) {			// Rotation
			server->grabbed_surface->x_rot += event->delta_y * -0.02;
			server->grabbed_surface->y_rot += event->delta_x * 0.02;
		} else if (server->cursor_mode == VKWC_CURSOR_Z_ROTATE) {
			server->grabbed_surface->z_rot += event->delta_x * 0.02;
		} else if (server->cursor_mode == VKWC_CURSOR_X_ROTATE_SPEED) {		// Rotation speed
			server->grabbed_surface->x_rot_speed += event->delta_x * 0.02 * 0.05;
		} else if (server->cursor_mode == VKWC_CURSOR_Y_ROTATE_SPEED) {
			server->grabbed_surface->y_rot_speed += event->delta_x * 0.02 * 0.05;
		} else if (server->cursor_mode == VKWC_CURSOR_Z_ROTATE_SPEED) {
			server->grabbed_surface->z_rot_speed += event->delta_x * 0.02 * 0.05;
		} else if (server->cursor_mode == VKWC_CURSOR_X_MOVE) {			// Translation
			server->grabbed_surface->x_offset += event->delta_x;
		} else if (server->cursor_mode == VKWC_CURSOR_Y_MOVE) {			// Translation
			server->grabbed_surface->y_offset += event->delta_y;
		} else if (server->cursor_mode == VKWC_CURSOR_Z_MOVE) {			// Translation
			server->grabbed_surface->z_offset += event->delta_y;
		} else {
			process_cursor_motion(server, event->time_msec);
		}
	} else {
		process_cursor_motion(server, event->time_msec);
	}
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move	the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse	there. There is	also some hardware which
	 * emits these events. */
	struct Server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event	= data;
	wlr_cursor_warp_absolute(server->cursor, event->device,	event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits an axis event,
	 * for example when you	move the scroll	wheel. */
	struct Server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = data;
	/* Notify the client with pointer focus	of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits a	frame
	 * event. Frame	events are sent	after regular pointer events to	group
	 * multiple events together. For instance, two axis events may happen at the
	 * same	time, in which case a frame event won't	be sent	in between. */
	struct Server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus	of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
	/* This	function is called every time an output	is ready to display a frame,
	 * generally at	the output's refresh rate (e.g.	60Hz). */
	struct Server *server = wl_container_of(listener, server, output_frame);
	struct wlr_output *output = server->output;

	// Pre-frame processing
	struct wl_list *surfaces = &server->surfaces;
	calc_matrices(surfaces, output->width, output->height);

	/* Render the scene if needed and commit the output */
	draw_frame(output, &server->surfaces, server->cursor->x, server->cursor->y);

	struct timespec	now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	//wlr_scene_output_send_frame_done(scene_output, &now);

	uint32_t time = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;

	// Tell all the surfaces we finished a frame
	struct Surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		wlr_surface_send_frame_done(surface->wlr_surface, &now);
	}

	// Send cursor position to focused Surface, with so much spinning stuff it might have changed
	process_cursor_motion(server, time);
}

static void handle_new_output(struct wl_listener *listener, void *data)	{
	/* This	event is raised	by the backend when a new output (aka a	display	or
	 * monitor) becomes available. */
	struct Server *server =
		wl_container_of(listener, server, new_output);

	if (server->output != NULL) {
		fprintf(stderr, "Already have an output! Not adding new one.\n");
		return;
	}

	printf("Adding output\n");

	server->output = data;

	/* Configures the output created by the	backend	to use our allocator
	 * and our renderer. Must be done once,	before commiting the output */
	wlr_output_init_render(server->output, server->allocator, server->renderer);

	/* Some	backends don't have modes. DRM+KMS does, and we	need to	set a mode
	 * before we can use the output. The mode is a tuple of	(width,	height,
	 * refresh rate), and each monitor supports only a specific set	of modes. We
	 * just	pick the monitor's preferred mode, a more sophisticated	compositor
	 * would let the user configure	it. */
	if (!wl_list_empty(&server->output->modes))	{
		struct wlr_output_mode *mode = wlr_output_preferred_mode(server->output);
		wlr_output_set_mode(server->output,	mode);
		wlr_output_enable(server->output, true);
		if (!wlr_output_commit(server->output)) {
			return;
		}
	}

	wlr_output_layout_add_auto(server->output_layout, server->output);

	/* Sets	up a listener for the frame notify event. */
	wl_signal_add(&server->output->events.frame, &server->output_frame);
}

// Allocates a new Surface, zeroing the struct and setting server, wlr_surface, id, and destroy.
// The user must still set the geometry, physics body, and toplevel.
// Also adds surface to surfaces.
static struct Surface *create_surface(struct wl_list *surfaces, struct wlr_surface *wlr_surface) {
	struct Surface *surface = calloc(1, sizeof(struct Surface));
	surface->wlr_surface = wlr_surface;
	surface->toplevel = NULL;
	surface->id = (double) rand() / RAND_MAX;

	wl_list_insert(surfaces->prev, &surface->link);

	return surface;
}

// The Surface was already created, we just have to set the width and height
static void handle_xdg_map(struct wl_listener *listener, void *data) {
	struct Server *server = wl_container_of(listener, server, handle_xdg_map);
	struct wlr_xdg_surface *xdg_surface = data;
	struct wlr_surface *wlr_surface = xdg_surface->surface;

	struct Surface *surface = find_surface(wlr_surface, &server->surfaces);
	assert(surface != NULL);

	surface->width = wlr_surface->current.width;
	surface->height = wlr_surface->current.height;

	focus_surface(server->seat, surface);

	printf("Surface mapped, set dims to %d %d\n", surface->width, surface->height);
}

// Adds a subsurface to the given list of surfaces. surfaces should be a list of type struct Surface.
static void add_subsurface(struct wl_list *surfaces, struct wlr_subsurface *subsurface) {
	struct wlr_surface *wlr_surface = subsurface->surface;

	// Make sure the surface doesn't already exist - this seems to never happen but it's worth checking
	struct Surface *found_surface = find_surface(wlr_surface, surfaces);
	assert(found_surface == NULL);

	struct Surface *surface = create_surface(surfaces, wlr_surface);
	printf("Adding sneaky subsurface with geo %d %d %d %d\n", subsurface->current.x, subsurface->current.y,
		wlr_surface->current.width, wlr_surface->current.height);
	surface->width = wlr_surface->current.width;
	surface->height = wlr_surface->current.height;
	surface->x = subsurface->current.x;
	surface->y = subsurface->current.y;
	surface->z_offset = 2;

	surface->toplevel = find_surface(subsurface->parent, surfaces);

	// The x and y we just filled in are relative to our parent. However, it's possible that surface->toplevel is
	// itself a subsurface, in which case we need to offset x and y by its position.
	// 
	// It's not necessary to adjust our position relative to the real toplevel, because calc_matrices already
	// takes this into account.
	assert(surface->toplevel != NULL);
	while (surface->toplevel != surface->toplevel->toplevel) {
		surface->x += surface->toplevel->x;
		surface->y += surface->toplevel->y;
		surface->toplevel = surface->toplevel->toplevel;
		assert(surface->toplevel != NULL);
	}
}

static void handle_new_subsurface(struct wl_listener *listener, void *data) {
	struct Server *server = wl_container_of(listener, server, handle_new_subsurface);
	struct wlr_subsurface *subsurface = data;
	struct wlr_surface *wlr_surface = subsurface->surface;

	add_subsurface(&server->surfaces, subsurface);

	// Listen for subsurfaces of the subsurface
	wl_signal_add(&wlr_surface->events.new_subsurface, &server->handle_new_subsurface);

	// Pretty sure this never gets called, but better safe than sorry
	wl_signal_add(&subsurface->events.map, &server->handle_subsurface_map);

	// Add existing subsurfaces above and below
	struct wlr_subsurface *cur;
	wl_list_for_each(cur, &wlr_surface->current.subsurfaces_below, current.link) {
		add_subsurface(&server->surfaces, cur);
	}

	wl_list_for_each(cur, &wlr_surface->current.subsurfaces_above, current.link) {
		add_subsurface(&server->surfaces, cur);
	}
}

static void handle_subsurface_map(struct wl_listener *listener, void *data) {
	// It seems that surfaces are always mapped by the time handle_new_subsurface gets called, so this is
	// redundant I think.
	fprintf(stderr, "This can't happen\n");
	exit(1);
}

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This	event is raised	when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct Server *server = wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;
	struct wlr_surface *wlr_surface = xdg_surface->surface;

	printf("New XDG surface!\n");

	// The width and height will be filled in by handle_xdg_map once it is known
	struct Surface *surface = create_surface(&server->surfaces, wlr_surface);

	surface->xdg_surface = xdg_surface;
	surface->width = 0;
	surface->height = 0;

	surface->destroy.notify = surface_handle_destroy;
	wl_signal_add(&wlr_surface->events.destroy, &surface->destroy);

	surface->toplevel = surface;
	surface->body = NULL;
	surface->apply_physics = false;

	wl_signal_add(&xdg_surface->events.map, &server->handle_xdg_map);
	wl_signal_add(&wlr_surface->events.new_subsurface, &server->handle_new_subsurface);

	// Check for subsurfaces above and below
	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &wlr_surface->current.subsurfaces_below, current.link) {
		add_subsurface(&server->surfaces, subsurface);
	}

	wl_list_for_each(subsurface, &wlr_surface->current.subsurfaces_above, current.link) {
		add_subsurface(&server->surfaces, subsurface);
	}

	/*
	// We must add xdg popups to the scene graph so	they get rendered. The
	// wlroots scene graph provides	a helper for this, but to use it we must
	// provide the proper parent scene node	of the xdg popup. To enable this,
	// we always set the user data field of	xdg_surfaces to	the corresponding
	// scene node.
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_from_wlr_surface(
			xdg_surface->popup->parent);
		struct wlr_scene_node *parent_node = parent->data;
		xdg_surface->data = wlr_scene_xdg_surface_create(
			parent_node, xdg_surface);
		return;
	}
	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

	// Allocate a View for this surface
	view->xdg_surface = xdg_surface;
	view->scene_node = wlr_scene_xdg_surface_create(&view->server->scene->node, view->xdg_surface);
	view->scene_node->data = view;
	xdg_surface->data = view->scene_node;

	// Listen to the various events	it can emit
	view->map.notify = handle_xdg_toplevel_map;
	wl_signal_add(&xdg_surface->events.map,	&view->map);
	view->unmap.notify = handle_xdg_toplevel_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = handle_xdg_toplevel_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

	// cotd
	struct wlr_xdg_toplevel	*toplevel = xdg_surface->toplevel;
	view->request_move.notify = handle_xdg_toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = handle_xdg_toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize,	&view->request_resize);
	*/
}

int main(int argc, char	*argv[]) {
	wlr_log_init(WLR_DEBUG,	NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct Server server = {0};
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from	the Unix socket, mangling Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the	underlying input and
	 * output hardware. The	autocreate option will choose the most suitable
	 * backend based on the	current	environment, such as opening an	X11 window
	 * if an X11 server is running.	*/
	server.backend = wlr_backend_autocreate(server.wl_display);

	// Create a renderer, we want Vulkan
	int drm_fd = -1;
	drm_fd = wlr_backend_get_drm_fd(server.backend);

	if (drm_fd < 0)	{
		fprintf(stderr,	"Couldn't get DRM file descriptor\n");
		exit(1);
	}

	server.renderer	= wlr_vk_renderer_create_with_drm_fd(drm_fd);
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for	us.
	 * The allocator is the	bridge between the renderer and	the backend. It
	 * handles the buffer creation,	allowing wlroots to render onto	the
	 * screen */
	 server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);

	/* This	creates	some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these	wlroots	interfaces has room for	you
	 * to dig your fingers in and play with	their behavior if you want. Note that
	 * the clients cannot set the selection	directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	struct wlr_compositor *compositor = wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	// I used to listen to the new surface event. Now, we instead map listeners to xdg_surface->map and
	// xdg_surface->subsurface->map to get positioning information.
	wl_list_init(&server.surfaces);

	// We only support one output, which will be whichever one is added first.
	server.output = NULL;
	server.output_layout = wlr_output_layout_create();

	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.output_frame.notify = handle_output_frame;

	/* Set up the xdg-shell. The xdg-shell is a Wayland protocol which is used
	 * for application windows. For	more detail on shells, refer to	my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display);

	server.new_xdg_surface.notify =	handle_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
			&server.new_xdg_surface);

	server.handle_xdg_map.notify = handle_xdg_map;
	server.handle_new_subsurface.notify = handle_new_subsurface;
	server.handle_subsurface_map.notify = handle_subsurface_map;

	/*
	 * Creates a cursor, which is a	wlroots	utility	for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor =	wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	// I added this	so grim	can figure out the screen dimensions
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available	at all scale factors on	the screen (necessary for
	 * HiDPI support). We add a cursor theme at scale factor 1 to begin with. */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when	the pointer moves. However, we can attach input	devices	to it, and
	 * it will generate aggregate events for all of	them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_motion.notify = handle_cursor_motion_relative;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	//server.cursor_button.notify = handle_cursor_button; // TODO
	//wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a	seat, which is a single	"seat" at which	a user sits and
	 * operates the	computer. This conceptually includes up	to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener	to
	 * let us know when new	input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify	= handle_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = handle_new_cursor_image;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.request_set_selection.notify = handle_selection_request;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	// Screencopy support
	wlr_screencopy_manager_v1_create(server.wl_display);

	// Set up xwayland
	struct wlr_xwayland *xwayland =	wlr_xwayland_create(server.wl_display, compositor, true);
	if (!xwayland) {
		fprintf(stderr,	"Cannot	create XWayland	server!\n");
		exit(1);
	};

	struct wlr_xcursor_manager *xcursor_manager = wlr_xcursor_manager_create("left_ptr", 24);
	if (!xcursor_manager) {
		fprintf(stderr,	"Can't create XCursor manager!\n");
		exit(1);
	};

	if (setenv("DISPLAY", xwayland->display_name, true) < 0) {
		fprintf(stderr,	"Couldn't set DISPLAY for XWayland!\n");
		exit(1);
	} else {
		printf("XWayland on DISPLAY=%s\n", xwayland->display_name);
		fflush(stdout);
	}

	if (!wlr_xcursor_manager_load(xcursor_manager, 1)) {
		fprintf(stderr,	"Can't load XCursor theme!\n");
		exit(1);
	}
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(xcursor_manager, "left_ptr", 1);
	if (xcursor) {
		struct wlr_xcursor_image *image	= xcursor->images[0];
		wlr_xwayland_set_cursor(xwayland, image->buffer, image->width *	4, image->width, image->height,
					image->hotspot_x, image->hotspot_y);
	};

	/* Add a Unix socket to	the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will	enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend))	{
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable	to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}

	// Start the physics engine
	InitPhysics();
	// Make the floor (it's huge so we don't have to resize it later)
	server.floor = CreatePhysicsBodyRectangle((Vector2) {0, 0}, 10000, PHYSAC_BOUNDARY_THICKNESS, 10);
	server.floor->enabled = false;

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting	the backend rigged up all of the necessary event
	 * loop	configuration to listen	to libinput events, DRM	events,	generate
	 * frame events	at the refresh rate, and so on.	*/
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);

	/* Once	wl_display_run returns,	we shut	down the server. */
	ClosePhysics();
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}
