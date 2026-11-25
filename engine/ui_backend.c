// Nuklear on our side. See ui_backend.h.

#include "ui_backend.h"

#include <stddef.h>

#include "file_io.h"
#include "object_model.h"
#include "stb_image.h"

typedef struct
{
	float position[2];
	float uv[2];
	nk_byte color[4];
} ui_vertex_t;

// Characters baked into the font: Latin with its extensions (Romanian
// among them), general punctuation (dashes, the ellipsis), arrows and
// geometric shapes (the play triangle, squares) for text buttons
static const nk_rune font_glyph_ranges[] = {
	0x0020, 0x024F,
	0x2000, 0x206F,
	0x2190, 0x21FF,
	0x25A0, 0x25FF,
	0
};

static void clipboard_paste(nk_handle user_data, struct nk_text_edit *edit)
{
	char *text = SDL_GetClipboardText();

	(void)user_data;
	if (text != NULL)
	{
		nk_textedit_paste(edit, text, nk_strlen(text));
		SDL_free(text);
	}
}

static void clipboard_copy(nk_handle user_data, const char *text, int length)
{
	char *copy = SDL_malloc((size_t)length + 1);

	(void)user_data;
	if (copy == NULL)
	{
		return;
	}
	SDL_memcpy(copy, text, (size_t)length);
	copy[length] = '\0';
	SDL_SetClipboardText(copy);
	SDL_free(copy);
}

static bool bake_font(ui_backend_t *backend, const char *font_path, float font_size, float large_scale, float pixel_scale)
{
	size_t font_file_size = 0;
	void *font_file = file_io_load_data_file(font_path, &font_file_size);
	struct nk_font_config config;
	const void *pixels;
	int width;
	int height;

	nk_font_atlas_init_default(&backend->atlas);
	nk_font_atlas_begin(&backend->atlas);
	config = nk_font_config(font_size * pixel_scale);
	config.range = font_glyph_ranges;
	config.oversample_h = 2;
	config.oversample_v = 1;
	if (font_file != NULL)
	{
		struct nk_font_config large = nk_font_config(font_size * large_scale * pixel_scale);

		backend->font = nk_font_atlas_add_from_memory(&backend->atlas, font_file, font_file_size, font_size * pixel_scale, &config);
		large.range = font_glyph_ranges;
		large.oversample_h = 2;
		large.oversample_v = 1;
		backend->large_font = nk_font_atlas_add_from_memory(&backend->atlas, font_file, font_file_size,
			font_size * large_scale * pixel_scale, &large);
	}
	if (backend->font == NULL)
	{
		// Nuklear's built-in font (ProggyClean) keeps the game usable
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the interface font %s; using the built-in one", font_path);
		config.range = nk_font_default_glyph_ranges();
		backend->font = nk_font_atlas_add_default(&backend->atlas, font_size * pixel_scale, &config);
	}
	pixels = nk_font_atlas_bake(&backend->atlas, &width, &height, NK_FONT_ATLAS_RGBA32);
	if (pixels == NULL)
	{
		SDL_free(font_file);
		return false;
	}
	backend->font_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)width,
		(uint32_t)height, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
	if (backend->font_texture == NULL)
	{
		SDL_free(font_file);
		return false;
	}
	gpu_upload_texture(backend->font_texture, 0, 0, 0, 0, (uint32_t)width, (uint32_t)height, pixels, (size_t)width * (size_t)height * 4);
	nk_font_atlas_end(&backend->atlas, nk_handle_ptr(backend->font_texture), &backend->null_texture);
	nk_font_atlas_cleanup(&backend->atlas);
	SDL_free(font_file);
	// Baked at pixel size, measured in interface units: the glyph metrics
	// scale with the height Nuklear is told
	backend->font->handle.height = font_size;
	if (backend->large_font != NULL)
	{
		backend->large_font->handle.height = font_size * large_scale;
	}
	return true;
}

bool ui_backend_create(ui_backend_t *backend, SDL_Window *window, const char *font_path, float font_size, float large_scale, float scale)
{
	float pixel_density = SDL_GetWindowPixelDensity(window);

	SDL_zerop(backend);
	backend->window = window;
	backend->scale = SDL_clamp(scale, 0.5f, 4.0f);
	{
		static const SDL_GPUVertexBufferDescription vertex_buffer = { 0, sizeof(ui_vertex_t), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
		static const SDL_GPUVertexAttribute attributes[3] =
		{
			{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ui_vertex_t, position) },
			{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ui_vertex_t, uv) },
			{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(ui_vertex_t, color) },
		};
		SDL_GPUShader *vertex = gpu_load_shader("ui.vert", 0, 0, 1);
		SDL_GPUShader *fragment = gpu_load_shader("ui.frag", 1, 0, 0);
		gpu_pipeline_info_t info;

		if (vertex != NULL && fragment != NULL)
		{
			SDL_zero(info);
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.vertex_buffers = &vertex_buffer;
			info.vertex_buffer_count = 1;
			info.vertex_attributes = attributes;
			info.vertex_attribute_count = 3;
			info.cull = SDL_GPU_CULLMODE_NONE;
			info.blend = GPU_BLEND_ALPHA;
			info.no_depth = true;
			info.color_format = GPU_FINAL_FORMAT;
			backend->pipeline = gpu_make_pipeline(&info, "interface");
			if (gpu.direct_possible && gpu.swapchain_format != GPU_FINAL_FORMAT)
			{
				info.color_format = gpu.swapchain_format;
				backend->window_pipeline = gpu_make_pipeline(&info, "interface into the window");
				info.color_format = GPU_FINAL_FORMAT;
			}
		}
		SDL_ReleaseGPUShader(gpu.device, fragment);
		// (the same, hidden behind the scene: the world's names)
		fragment = gpu_load_shader("ui_depth.frag", 2, 0, 1);
		if (vertex != NULL && fragment != NULL)
		{
			info.fragment_shader = fragment;
			backend->depth_pipeline = gpu_make_pipeline(&info, "interface in the world");
			if (gpu.direct_possible && gpu.swapchain_format != GPU_FINAL_FORMAT)
			{
				info.color_format = gpu.swapchain_format;
				backend->window_depth_pipeline = gpu_make_pipeline(&info, "interface in the world, into the window");
				info.color_format = GPU_FINAL_FORMAT;
			}
		}
		SDL_ReleaseGPUShader(gpu.device, vertex);
		SDL_ReleaseGPUShader(gpu.device, fragment);
		if (backend->pipeline == NULL)
		{
			return false;
		}
	}

	backend->large_scale = SDL_clamp(large_scale, 1.0f, 3.0f);
	if (!bake_font(backend, font_path, SDL_clamp(font_size, 6.0f, 64.0f), backend->large_scale, backend->scale * SDL_max(pixel_density, 1.0f)))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot bake the interface font");
		return false;
	}
	if (!nk_init_default(&backend->context, &backend->font->handle))
	{
		return false;
	}
	backend->initialized = true;
	backend->context.clip.copy = clipboard_copy;
	backend->context.clip.paste = clipboard_paste;
	nk_buffer_init_default(&backend->commands);
	nk_buffer_init_default(&backend->vertices);
	nk_buffer_init_default(&backend->elements);
	return true;
}

void ui_backend_destroy(ui_backend_t *backend)
{
	if (backend->initialized)
	{
		nk_buffer_free(&backend->commands);
		nk_buffer_free(&backend->vertices);
		nk_buffer_free(&backend->elements);
		nk_free(&backend->context);
		nk_font_atlas_clear(&backend->atlas);
	}
	if (backend->font_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, backend->font_texture);
	}
	if (backend->vertex_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, backend->vertex_buffer);
	}
	if (backend->element_buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, backend->element_buffer);
	}
	if (backend->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, backend->pipeline);
	}
	if (backend->depth_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, backend->depth_pipeline);
	}
	if (backend->window_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, backend->window_pipeline);
	}
	if (backend->window_depth_pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, backend->window_depth_pipeline);
	}
	if (backend->text_input_started)
	{
		SDL_StopTextInput(backend->window);
	}
	SDL_zerop(backend);
}

void ui_backend_input_begin(ui_backend_t *backend)
{
	int window_width = 1;
	int window_height = 1;

	SDL_GetWindowSize(backend->window, &window_width, &window_height);
	backend->width = (float)SDL_max(window_width, 1) / backend->scale;
	backend->height = (float)SDL_max(window_height, 1) / backend->scale;
	nk_input_begin(&backend->context);
}

static void handle_key(struct nk_context *context, SDL_Keycode key, SDL_Keymod modifiers, bool down)
{
	bool control = (modifiers & SDL_KMOD_CTRL) != 0;

	switch (key)
	{
		case SDLK_LSHIFT:
		case SDLK_RSHIFT:
			nk_input_key(context, NK_KEY_SHIFT, down);
			break;
		case SDLK_LCTRL:
		case SDLK_RCTRL:
			nk_input_key(context, NK_KEY_CTRL, down);
			break;
		case SDLK_DELETE:
			nk_input_key(context, NK_KEY_DEL, down);
			break;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			nk_input_key(context, NK_KEY_ENTER, down);
			break;
		case SDLK_TAB:
			nk_input_key(context, NK_KEY_TAB, down);
			break;
		case SDLK_BACKSPACE:
			nk_input_key(context, NK_KEY_BACKSPACE, down);
			break;
		case SDLK_HOME:
			nk_input_key(context, NK_KEY_TEXT_START, down);
			nk_input_key(context, NK_KEY_SCROLL_START, down);
			break;
		case SDLK_END:
			nk_input_key(context, NK_KEY_TEXT_END, down);
			nk_input_key(context, NK_KEY_SCROLL_END, down);
			break;
		case SDLK_PAGEDOWN:
			nk_input_key(context, NK_KEY_SCROLL_DOWN, down);
			break;
		case SDLK_PAGEUP:
			nk_input_key(context, NK_KEY_SCROLL_UP, down);
			break;
		case SDLK_UP:
			nk_input_key(context, NK_KEY_UP, down);
			break;
		case SDLK_DOWN:
			nk_input_key(context, NK_KEY_DOWN, down);
			break;
		case SDLK_LEFT:
			nk_input_key(context, control ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, down);
			break;
		case SDLK_RIGHT:
			nk_input_key(context, control ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, down);
			break;
		case SDLK_A:
			if (control)
			{
				nk_input_key(context, NK_KEY_TEXT_SELECT_ALL, down);
			}
			break;
		case SDLK_C:
			if (control)
			{
				nk_input_key(context, NK_KEY_COPY, down);
			}
			break;
		case SDLK_V:
			if (control)
			{
				nk_input_key(context, NK_KEY_PASTE, down);
			}
			break;
		case SDLK_X:
			if (control)
			{
				nk_input_key(context, NK_KEY_CUT, down);
			}
			break;
		case SDLK_Z:
			if (control)
			{
				nk_input_key(context, NK_KEY_TEXT_UNDO, down);
			}
			break;
		default:
			break;
	}
}

// Nuklear lets the first click on a window that is not the active one
// only make it active: the widget under the pointer does not see it (a
// window opened by a key, or left for another, wanted two clicks). The
// window on top under the pointer (where the windows were last frame) is
// made the active one before the click goes in
static void activate_window_under(struct nk_context *context, int x, int y)
{
	for (struct nk_window *window = context->end; window != NULL; window = window->prev)
	{
		struct nk_rect bounds = window->bounds;

		// (windows that take no input, the names over the actors, the
		// prompt, the notices, are passed: the click is for what is under
		// them)
		if ((window->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND)) != 0 || (float)x < bounds.x
			|| (float)y < bounds.y || (float)x >= bounds.x + bounds.w || (float)y >= bounds.y + bounds.h)
		{
			continue;
		}
		if (context->active != window)
		{
			nk_window_set_focus(context, window->name_string);
		}
		return;
	}
}

bool ui_backend_handle_event(ui_backend_t *backend, const SDL_Event *event)
{
	struct nk_context *context = &backend->context;
	float scale = backend->scale;

	switch (event->type)
	{
		case SDL_EVENT_MOUSE_MOTION:
			nk_input_motion(context, (int)(event->motion.x / scale), (int)(event->motion.y / scale));
			return false;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			int x = (int)(event->button.x / scale);
			int y = (int)(event->button.y / scale);

			if (down)
			{
				activate_window_under(context, x, y);
			}

			if (event->button.button == SDL_BUTTON_LEFT)
			{
				if (down && event->button.clicks == 2)
				{
					nk_input_button(context, NK_BUTTON_DOUBLE, x, y, true);
				}
				else if (!down)
				{
					nk_input_button(context, NK_BUTTON_DOUBLE, x, y, false);
				}
				nk_input_button(context, NK_BUTTON_LEFT, x, y, down);
			}
			else if (event->button.button == SDL_BUTTON_RIGHT)
			{
				nk_input_button(context, NK_BUTTON_RIGHT, x, y, down);
			}
			else if (event->button.button == SDL_BUTTON_MIDDLE)
			{
				nk_input_button(context, NK_BUTTON_MIDDLE, x, y, down);
			}
			// Releases always reach the game too, or a button could stay
			// held there
			return down && backend->mouse_captured;
		}
		case SDL_EVENT_MOUSE_WHEEL:
			if (backend->mouse_captured)
			{
				nk_input_scroll(context, nk_vec2(event->wheel.x, event->wheel.y));
				return true;
			}
			return false;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			handle_key(context, event->key.key, event->key.mod, event->type == SDL_EVENT_KEY_DOWN);
			// Escape always reaches the game (quit), so a stuck text field
			// cannot lock the player in
			return backend->keyboard_captured && event->key.key != SDLK_ESCAPE;
		case SDL_EVENT_TEXT_INPUT:
		{
			const char *text = event->text.text;

			while (text != NULL && *text != '\0')
			{
				Uint32 codepoint = SDL_StepUTF8(&text, NULL);

				if (codepoint == 0)
				{
					break;
				}
				nk_input_unicode(context, (nk_rune)codepoint);
			}
			return true;
		}
		default:
			return false;
	}
}

void ui_backend_input_end(ui_backend_t *backend)
{
	nk_input_end(&backend->context);
}

void ui_backend_update_capture(ui_backend_t *backend)
{
	struct nk_context *context = &backend->context;

	struct nk_window *window;

	// As nk_window_is_any_hovered, but windows that take no input (the
	// status line) let the pointer through to the map
	backend->mouse_captured = false;
	for (window = context->begin; window != NULL; window = window->next)
	{
		struct nk_rect bounds = window->bounds;

		if (window->flags & NK_WINDOW_HIDDEN)
		{
			continue;
		}
		if (window->popup.active && window->popup.win != NULL && nk_input_is_mouse_hovering_rect(&context->input, window->popup.win->bounds))
		{
			backend->mouse_captured = true;
			break;
		}
		if (window->flags & NK_WINDOW_NO_INPUT)
		{
			continue;
		}
		if (window->flags & NK_WINDOW_MINIMIZED)
		{
			bounds.h = context->style.font->height + 2.0f * context->style.window.header.padding.y;
		}
		if (nk_input_is_mouse_hovering_rect(&context->input, bounds))
		{
			backend->mouse_captured = true;
			break;
		}
	}
	// A number being typed into (a property field, double clicked or
	// clicked on its value) wants the keyboard's text too, as a text field
	// does: without SDL's text input it takes Backspace but no digits
	// (nuklear's NK_PROPERTY_EDIT, 1: its enum is in its implementation)
	for (window = context->begin; window != NULL; window = window->next)
	{
		if (!(window->flags & NK_WINDOW_HIDDEN) && window->property.active && window->property.state == 1)
		{
			backend->wants_text_input = true;
			break;
		}
	}
	backend->keyboard_captured = backend->wants_text_input;
	if (backend->wants_text_input && !backend->text_input_started)
	{
		SDL_StartTextInput(backend->window);
		backend->text_input_started = true;
	}
	else if (!backend->wants_text_input && backend->text_input_started)
	{
		SDL_StopTextInput(backend->window);
		backend->text_input_started = false;
	}
	backend->wants_text_input = false;
}

// Grows a buffer the interface streams into (to the next power of two)
static bool ensure_buffer(SDL_GPUBuffer **buffer, uint32_t *bytes, SDL_GPUBufferUsageFlags usage, size_t needed)
{
	uint32_t size = 64 * 1024;

	if (*buffer != NULL && *bytes >= needed)
	{
		return true;
	}
	while (size < needed)
	{
		size *= 2;
	}
	if (*buffer != NULL)
	{
		SDL_ReleaseGPUBuffer(gpu.device, *buffer);
	}
	*buffer = gpu_create_buffer(usage, size);
	*bytes = *buffer != NULL ? size : 0;
	return *buffer != NULL;
}

// The depth in a user data: an id from 1 (0 is the rest), 24 bits of it
static const float WORLD_DEPTH_STEPS = 16777215.0f;
// Nearer than this (of the depth buffer's 0..1) before the scene hides it
static const float WORLD_DEPTH_BIAS = 0.00002f;

nk_handle ui_backend_world_depth(float depth)
{
	return nk_handle_id(1 + (int)(SDL_clamp(depth, 0.0f, 1.0f) * WORLD_DEPTH_STEPS));
}

void ui_backend_set_scene_depth(ui_backend_t *backend, SDL_GPUTexture *depth_texture)
{
	backend->scene_depth = depth_texture;
}

void ui_backend_render(ui_backend_t *backend, int pixel_width, int pixel_height)
{
	static const struct nk_draw_vertex_layout_element vertex_layout[] = {
		{ NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(ui_vertex_t, position) },
		{ NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(ui_vertex_t, uv) },
		{ NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(ui_vertex_t, color) },
		{ NK_VERTEX_LAYOUT_END }
	};
	struct nk_context *context = &backend->context;
	struct nk_convert_config config;
	const struct nk_draw_command *command;
	float pixels_per_unit_x = (float)pixel_width / backend->width;
	float pixels_per_unit_y = (float)pixel_height / backend->height;
	size_t index_offset = 0;

	SDL_zero(config);
	config.vertex_layout = vertex_layout;
	config.vertex_size = sizeof(ui_vertex_t);
	config.vertex_alignment = NK_ALIGNOF(ui_vertex_t);
	config.tex_null = backend->null_texture;
	config.circle_segment_count = 22;
	config.curve_segment_count = 22;
	config.arc_segment_count = 22;
	config.global_alpha = 1.0f;
	config.shape_AA = NK_ANTI_ALIASING_ON;
	config.line_AA = NK_ANTI_ALIASING_ON;

	nk_buffer_clear(&backend->commands);
	nk_buffer_clear(&backend->vertices);
	nk_buffer_clear(&backend->elements);
	nk_convert(context, &backend->commands, &backend->vertices, &backend->elements, &config);

	if (gpu.command_buffer == NULL || backend->vertices.needed == 0 || backend->elements.needed == 0 || gpu_final_target() == NULL)
	{
		nk_clear(context);
		return;
	}
	if (!ensure_buffer(&backend->vertex_buffer, &backend->vertex_buffer_bytes, SDL_GPU_BUFFERUSAGE_VERTEX, backend->vertices.needed)
		|| !ensure_buffer(&backend->element_buffer, &backend->element_buffer_bytes, SDL_GPU_BUFFERUSAGE_INDEX, backend->elements.needed))
	{
		nk_clear(context);
		return;
	}
	gpu_upload_buffer(backend->vertex_buffer, 0, nk_buffer_memory_const(&backend->vertices), backend->vertices.needed);
	gpu_upload_buffer(backend->element_buffer, 0, nk_buffer_memory_const(&backend->elements), backend->elements.needed);
	gpu_flush_uploads();
	{
		SDL_GPUColorTargetInfo target;
		SDL_GPURenderPass *pass;
		SDL_GPUBufferBinding vertex_binding = { backend->vertex_buffer, 0 };
		SDL_GPUBufferBinding index_binding = { backend->element_buffer, 0 };
		SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)pixel_width, (float)pixel_height, 0.0f, 1.0f };
		float uniforms[4] = { 2.0f / backend->width, 2.0f / backend->height, 0.0f, 0.0f };

		SDL_zero(target);
		target.texture = gpu.final_texture;
		target.load_op = SDL_GPU_LOADOP_LOAD;
		target.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(gpu.command_buffer, &target, 1, NULL);
		if (pass == NULL)
		{
			nk_clear(context);
			return;
		}
		// (into the window's picture straight: those made for its format)
		bool into_window = gpu.final_format != GPU_FINAL_FORMAT && backend->window_pipeline != NULL;
		SDL_GPUGraphicsPipeline *plain_pipeline = into_window ? backend->window_pipeline : backend->pipeline;
		SDL_GPUGraphicsPipeline *world_pipeline = into_window ? backend->window_depth_pipeline : backend->depth_pipeline;
		SDL_GPUGraphicsPipeline *bound = plain_pipeline;

		SDL_BindGPUGraphicsPipeline(pass, plain_pipeline);
		SDL_SetGPUViewport(pass, &viewport);
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, uniforms, sizeof(uniforms));
		SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
		SDL_BindGPUIndexBuffer(pass, &index_binding, sizeof(nk_draw_index) == 2 ? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT);
		nk_draw_foreach(command, context, &backend->commands)
		{
			SDL_Rect clip;
			int right;
			int bottom;
			SDL_GPUTextureSamplerBinding texture_binding;

			if (command->elem_count == 0)
			{
				continue;
			}
			// The scissor: in pixels from the top left, inside the target
			clip.x = SDL_max((int)SDL_floorf(command->clip_rect.x * pixels_per_unit_x), 0);
			clip.y = SDL_max((int)SDL_floorf(command->clip_rect.y * pixels_per_unit_y), 0);
			right = SDL_min((int)SDL_ceilf((command->clip_rect.x + command->clip_rect.w) * pixels_per_unit_x), pixel_width);
			bottom = SDL_min((int)SDL_ceilf((command->clip_rect.y + command->clip_rect.h) * pixels_per_unit_y), pixel_height);
			clip.w = right - clip.x;
			clip.h = bottom - clip.y;
			if (clip.w > 0 && clip.h > 0 && command->texture.ptr != NULL)
			{
				bool in_world = command->userdata.id != 0 && world_pipeline != NULL && backend->scene_depth != NULL;
				SDL_GPUGraphicsPipeline *wanted = in_world ? world_pipeline : plain_pipeline;

				if (wanted != bound)
				{
					SDL_BindGPUGraphicsPipeline(pass, wanted);
					SDL_SetGPUViewport(pass, &viewport);
					SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, uniforms, sizeof(uniforms));
					bound = wanted;
				}
				SDL_SetGPUScissor(pass, &clip);
				texture_binding.texture = command->texture.ptr;
				texture_binding.sampler = gpu.linear_clamp;
				if (in_world)
				{
					SDL_GPUTextureSamplerBinding bindings[2] = { texture_binding, { backend->scene_depth, gpu.nearest_clamp } };
					float depth_uniforms[4] = { (float)(command->userdata.id - 1) / WORLD_DEPTH_STEPS, WORLD_DEPTH_BIAS, 1.0f / (float)pixel_width,
						1.0f / (float)pixel_height };

					SDL_BindGPUFragmentSamplers(pass, 0, bindings, 2);
					SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, depth_uniforms, sizeof(depth_uniforms));
				}
				else
				{
					SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
				}
				SDL_DrawGPUIndexedPrimitives(pass, command->elem_count, 1, (uint32_t)index_offset, 0, 0);
			}
			index_offset += command->elem_count;
		}
		SDL_EndGPURenderPass(pass);
	}
	nk_clear(context);
}

bool ui_backend_load_image(const char *path, struct nk_image *image_out)
{
	size_t file_size = 0;
	void *file_data;
	uint8_t *pixels;
	int width;
	int height;
	int channels;
	SDL_GPUTexture *texture;

	SDL_zerop(image_out);
	if (!file_io_data_file_exists(path))
	{
		return false;
	}
	file_data = file_io_load_data_file(path, &file_size);
	if (file_data == NULL)
	{
		return false;
	}
	pixels = stbi_load_from_memory(file_data, (int)file_size, &width, &height, &channels, 4);
	SDL_free(file_data);
	if (pixels == NULL)
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot decode %s: %s", path, stbi_failure_reason());
		return false;
	}
	texture = object_texture_upload(pixels, width, height);
	stbi_image_free(pixels);
	if (texture == NULL)
	{
		return false;
	}
	*image_out = nk_image_ptr(texture);
	image_out->w = (nk_ushort)width;
	image_out->h = (nk_ushort)height;
	image_out->region[2] = (nk_ushort)width;
	image_out->region[3] = (nk_ushort)height;
	return true;
}

void ui_backend_free_image(struct nk_image *image)
{
	SDL_GPUTexture *texture = image->handle.ptr;

	if (texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, texture);
	}
	SDL_zerop(image);
}
