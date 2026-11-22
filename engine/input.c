// Input as actions. See input.h for the mapping.

#include "input.h"

void input_initialize(input_state_t *input)
{
	SDL_zerop(input);
	input->select_tool = -1;
	input->select_soil = -1;
}

void input_begin_frame(input_state_t *input)
{
	input->rotate_drag_x_pixels = 0.0f;
	input->rotate_drag_y_pixels = 0.0f;
	input->right_clicked = false;
	input->pan_drag_x_pixels = 0.0f;
	input->pan_drag_y_pixels = 0.0f;
	input->zoom_steps = 0.0f;
	input->look_x_pixels = 0.0f;
	input->look_y_pixels = 0.0f;
	input->jump_pressed = false;
	input->select_tool = -1;
	input->select_soil = -1;
	input->brush_size_steps = 0;
	input->stamp_height_steps = 0;
	input->cycle_stamp = false;
	input->place_object = false;
	input->remove_object = false;
	input->place_actor = false;
	input->cycle_actor_type = false;
	input->remove_actor = false;
	input->cycle_actor_action = 0;
	input->cycle_plant = false;
	input->select_clear = false;
	input->request_repopulate = false;
	input->request_remove_objects = false;
	input->cycle_layout = false;
	input->request_remove_layout = false;
	input->time_scale_steps = 0;
	input->request_regenerate = false;
	input->toggle_spring = false;
	input->toggle_rain = false;
	input->toggle_borders = false;
	input->bake_sky_occlusion = false;
	input->toggle_water_model = false;
	input->toggle_pause = false;
	input->request_screenshot = false;
	input->toggle_wireframe = false;
	input->toggle_debug_view = false;
	input->toggle_tile_overlay = false;
	input->toggle_thin_highlight = false;
	input->toggle_water_freeze = false;
	input->toggle_navigation_overlay = false;
	input->toggle_first_person = false;
	input->request_diagnostics = false;
	input->request_quick_save = false;
	input->request_quick_load = false;
	input->toggle_interface = false;
	input->request_escape = false;
	input->toggle_fullscreen = false;
	input->toggle_hands = false;
	input->interact = false;
	input->toggle_inventory = false;
	input->focus_gained = false;
}

static void handle_key_down(input_state_t *input, SDL_Keycode key, SDL_Keymod modifiers)
{
	switch (key)
	{
		case SDLK_O:
			if (modifiers & SDL_KMOD_SHIFT)
			{
				input->remove_object = true;
			}
			else
			{
				input->place_object = true;
			}
			break;
		case SDLK_K:
			if (modifiers & SDL_KMOD_SHIFT)
			{
				input->remove_actor = true;
			}
			else if (modifiers & SDL_KMOD_CTRL)
			{
				input->cycle_actor_type = true;
			}
			else
			{
				input->place_actor = true;
			}
			break;
		case SDLK_J:
			input->cycle_actor_action = (modifiers & SDL_KMOD_SHIFT) ? -1 : 1;
			break;
		case SDLK_G:
			input->cycle_plant = true;
			break;
		case SDLK_H:
			input->select_clear = true;
			break;
		case SDLK_L:
			input->cycle_layout = true;
			break;
		case SDLK_DELETE:
			input->request_remove_layout = true;
			break;
		case SDLK_V:
			if (modifiers & SDL_KMOD_SHIFT)
			{
				input->request_remove_objects = true;
			}
			else
			{
				input->request_repopulate = true;
			}
			break;
		case SDLK_ESCAPE:
			input->request_escape = true;
			break;
		case SDLK_Q:
			if (modifiers & SDL_KMOD_CTRL)
			{
				input->request_quit = true;
			}
			break;
		case SDLK_F11:
			input->toggle_fullscreen = true;
			break;
		case SDLK_E:
			input->interact = true;
			break;
		case SDLK_I:
			input->toggle_inventory = true;
			break;
		case SDLK_0:
			input->toggle_hands = true;
			break;
		case SDLK_1:
		case SDLK_2:
		case SDLK_3:
		case SDLK_4:
			input->select_tool = (int)(key - SDLK_1);
			break;
		case SDLK_5:
		case SDLK_6:
		case SDLK_7:
		case SDLK_8:
			input->select_soil = (int)(key - SDLK_5);
			break;
		case SDLK_LEFTBRACKET:
			input->brush_size_steps--;
			break;
		case SDLK_RIGHTBRACKET:
			input->brush_size_steps++;
			break;
		case SDLK_T:
			input->cycle_stamp = true;
			break;
		case SDLK_MINUS:
			input->stamp_height_steps--;
			break;
		case SDLK_EQUALS:
			input->stamp_height_steps++;
			break;
		case SDLK_N:
			input->request_regenerate = true;
			break;
		case SDLK_F:
			input->toggle_spring = true;
			break;
		case SDLK_R:
			input->toggle_rain = true;
			break;
		case SDLK_B:
			input->toggle_borders = true;
			break;
		case SDLK_U:
			input->bake_sky_occlusion = true;
			break;
		case SDLK_M:
			input->toggle_water_model = true;
			break;
		case SDLK_SPACE:
			if (input->first_person)
			{
				input->jump_pressed = true;
			}
			else
			{
				input->toggle_pause = true;
			}
			break;
		case SDLK_P:
			input->toggle_pause = true;
			break;
		case SDLK_C:
			input->toggle_first_person = true;
			break;
		case SDLK_F8:
			input->toggle_navigation_overlay = true;
			break;
		case SDLK_F7:
			input->toggle_water_freeze = true;
			break;
		case SDLK_COMMA:
			input->time_scale_steps--;
			break;
		case SDLK_PERIOD:
			input->time_scale_steps++;
			break;
		case SDLK_F1:
			input->toggle_wireframe = true;
			break;
		case SDLK_F2:
			input->toggle_debug_view = true;
			break;
		case SDLK_F3:
			input->toggle_tile_overlay = true;
			break;
		case SDLK_F4:
			input->request_diagnostics = true;
			break;
		case SDLK_F6:
			input->toggle_thin_highlight = true;
			break;
		case SDLK_F5:
			input->request_quick_save = true;
			break;
		case SDLK_F9:
			input->request_quick_load = true;
			break;
		case SDLK_F10:
			input->toggle_interface = true;
			break;
		case SDLK_F12:
			input->request_screenshot = true;
			break;
		default:
			break;
	}
}

void input_handle_event(input_state_t *input, const SDL_Event *event)
{
	switch (event->type)
	{
		case SDL_EVENT_QUIT:
			input->request_quit = true;
			break;
		case SDL_EVENT_KEY_DOWN:
			if (!event->key.repeat)
			{
				handle_key_down(input, event->key.key, event->key.mod);
			}
			break;
		case SDL_EVENT_MOUSE_MOTION:
			input->pointer_x = event->motion.x;
			input->pointer_y = event->motion.y;
			input->pointer_in_window = true;
			input->look_x_pixels += event->motion.xrel;
			input->look_y_pixels += event->motion.yrel;
			if (event->motion.state & SDL_BUTTON_RMASK)
			{
				input->rotate_drag_x_pixels += event->motion.xrel;
				input->rotate_drag_y_pixels += event->motion.yrel;
				input->right_drag_pixels += SDL_fabsf(event->motion.xrel) + SDL_fabsf(event->motion.yrel);
			}
			if (event->motion.state & SDL_BUTTON_MMASK)
			{
				input->pan_drag_x_pixels += event->motion.xrel;
				input->pan_drag_y_pixels += event->motion.yrel;
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (event->button.button == SDL_BUTTON_LEFT)
			{
				input->left_button_down = true;
			}
			else if (event->button.button == SDL_BUTTON_RIGHT)
			{
				input->right_button_down = true;
				input->right_drag_pixels = 0.0f;
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (event->button.button == SDL_BUTTON_LEFT)
			{
				input->left_button_down = false;
			}
			else if (event->button.button == SDL_BUTTON_RIGHT)
			{
				// (a click, not the camera turned: it hardly moved)
				input->right_clicked = input->right_button_down && input->right_drag_pixels < 6.0f;
				input->right_button_down = false;
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			input->zoom_steps += event->wheel.y;
			break;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			input->pointer_in_window = false;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		case SDL_EVENT_WINDOW_RESTORED:
			input->focus_gained = true;
			input->in_background = false;
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
		case SDL_EVENT_WINDOW_MINIMIZED:
			input->in_background = true;
			break;
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
			input->pointer_in_window = true;
			break;
		default:
			break;
	}
}

void input_end_frame(input_state_t *input)
{
	const bool *keys = SDL_GetKeyboardState(NULL);

	input->pan_keyboard_x = 0.0f;
	input->pan_keyboard_y = 0.0f;
	input->rotate_keyboard = 0.0f;

	if (input->keyboard_captured)
	{
		// Typing into the user interface (or the console): the letters are
		// not camera keys. (Static: a compound literal here would die with
		// this block, and the keys below would read whatever took its place)
		static const bool no_keys[SDL_SCANCODE_COUNT] = { false };

		keys = no_keys;
	}
	if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])
	{
		input->pan_keyboard_x -= 1.0f;
	}
	if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT])
	{
		input->pan_keyboard_x += 1.0f;
	}
	if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])
	{
		input->pan_keyboard_y += 1.0f;
	}
	if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])
	{
		input->pan_keyboard_y -= 1.0f;
	}
	if (keys[SDL_SCANCODE_Q])
	{
		input->rotate_keyboard -= 1.0f;
	}
	if (keys[SDL_SCANCODE_E])
	{
		input->rotate_keyboard += 1.0f;
	}
	input->jump_held = keys[SDL_SCANCODE_SPACE];
	input->run_held = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
	{
		bool held = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];

		input->dive_pressed = held && !input->dive_held;
		input->dive_held = held;
	}
	input->pick_held = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];

	{
		bool was_held = input->apply_tool_held;

		// From the button events, not the button state: a click the user
		// interface took never reaches the tools
		input->apply_tool_held = input->left_button_down && input->pointer_in_window;
		input->apply_tool_pressed = input->apply_tool_held && !was_held;
		input->apply_tool_released = !input->apply_tool_held && was_held;
	}
	input->water_action = WATER_ACTION_NONE;
	if (input->apply_tool_held)
	{
		SDL_Keymod modifiers = SDL_GetModState();

		if (modifiers & SDL_KMOD_SHIFT)
		{
			input->water_action = WATER_ACTION_ADD;
		}
		else if (modifiers & SDL_KMOD_CTRL)
		{
			input->water_action = WATER_ACTION_REMOVE;
		}
	}
}
