// Contact shadows (contact_shadows.h)

#include "contact_shadows.h"

#include "gpu.h"

// contact_shadows.frag's own block
typedef struct
{
	float parameters[4];           // x: how far the march goes (m), y: how far out from the eye it fades (m), z: the frame (its offsets), w: strength
} contact_uniforms_t;

static void create(contact_shadows_t *contact)
{
	gpu_pipeline_info_t info;

	contact->tried = true;
	SDL_zero(info);
	// (the clouds' full screen triangle)
	info.vertex_shader = gpu_load_shader("clouds.vert", 0, 0, 1);
	info.fragment_shader = gpu_load_shader("contact_shadows.frag", 3, 0, 2);
	info.cull = SDL_GPU_CULLMODE_NONE;
	info.no_depth = true;
	info.blend = GPU_BLEND_MULTIPLY;
	if (info.vertex_shader != NULL && info.fragment_shader != NULL)
	{
		contact->pipeline = gpu_make_pipeline(&info, "contact shadows");
	}
	if (info.vertex_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.vertex_shader);
	}
	if (info.fragment_shader != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, info.fragment_shader);
	}
}

void contact_shadows_destroy(contact_shadows_t *contact)
{
	if (contact->pipeline != NULL)
	{
		SDL_ReleaseGPUGraphicsPipeline(gpu.device, contact->pipeline);
		contact->pipeline = NULL;
	}
	contact->tried = false;
}

void contact_shadows_draw(contact_shadows_t *contact, SDL_GPUTexture *color_texture, SDL_GPUTexture *depth_texture, uint32_t width, uint32_t height,
	const scene_uniforms_t *scene, const render_settings_t *settings, SDL_GPUTexture *far_shadow, SDL_GPUTexture *near_shadow)
{
	contact_uniforms_t uniforms;
	SDL_GPUColorTargetInfo color;
	SDL_GPURenderPass *pass;

	if (!contact->tried)
	{
		create(contact);
	}
	if (contact->pipeline == NULL || far_shadow == NULL || near_shadow == NULL)
	{
		return;
	}
	contact->frame++;
	uniforms.parameters[0] = SDL_clamp(settings->contact_shadow_meters, 0.05f, 2.0f);
	uniforms.parameters[1] = 40.0f;
	uniforms.parameters[2] = (float)(contact->frame % 64u);
	uniforms.parameters[3] = SDL_clamp(settings->contact_shadow_strength, 0.0f, 1.5f);
	{
		SDL_GPUTextureSamplerBinding textures[3] =
		{
			{ far_shadow, gpu.shadow_compare },
			{ near_shadow, gpu.shadow_compare },
			{ depth_texture, gpu.nearest_clamp },
		};
		SDL_GPUViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };

		SDL_zero(color);
		color.texture = color_texture;
		color.load_op = SDL_GPU_LOADOP_LOAD;
		color.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass(gpu.command_buffer, &color, 1, NULL);
		if (pass == NULL)
		{
			return;
		}
		SDL_SetGPUViewport(pass, &viewport);
		SDL_BindGPUGraphicsPipeline(pass, contact->pipeline);
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, scene, sizeof(*scene));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 1, &uniforms, sizeof(uniforms));
		SDL_BindGPUFragmentSamplers(pass, 0, textures, 3);
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}
}
