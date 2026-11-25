// The loading screen (ui_loading.h)

#include "ui_loading.h"

#include <SDL3/SDL.h>

#include "gpu.h"

void ui_loading_frame(ui_backend_t *backend, const char *title, const char *what, float fraction)
{
	struct nk_context *context = &backend->context;
	float width = 420.0f;
	float height = 110.0f;
	nk_size done = (nk_size)(SDL_clamp(fraction, 0.0f, 1.0f) * 1000.0f);

	// (the window answers meanwhile: no "not responding")
	SDL_PumpEvents();
	if (gpu_begin_frame() && gpu_final_target() != NULL)
	{
		SDL_GPUColorTargetInfo target;
		SDL_GPURenderPass *pass;

		SDL_zero(target);
		target.texture = gpu.final_texture;
		target.clear_color.r = 0.05f;
		target.clear_color.g = 0.07f;
		target.clear_color.b = 0.10f;
		target.clear_color.a = 1.0f;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(gpu.command_buffer, &target, 1, NULL);
		if (pass != NULL)
		{
			SDL_EndGPURenderPass(pass);
		}
		// (the input frame sizes it to the window)
		ui_backend_input_begin(backend);
		ui_backend_input_end(backend);
		if (nk_begin(context, "Loading", nk_rect(0.5f * (backend->width - width), 0.5f * (backend->height - height), width, height),
			NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT))
		{
			nk_layout_row_dynamic(context, 26.0f, 1);
			nk_label(context, title, NK_TEXT_CENTERED);
			nk_label(context, what, NK_TEXT_CENTERED);
			nk_layout_row_dynamic(context, 18.0f, 1);
			nk_progress(context, &done, 1000, nk_false);
		}
		nk_end(context);
		ui_backend_render(backend, (int)gpu.final_width, (int)gpu.final_height);
	}
	gpu_end_frame(NULL, NULL, NULL);
}
