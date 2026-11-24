// The GPU through SDL_GPU. See gpu.h.

#include "gpu.h"
#include "profile.h"

#include "file_io.h"

gpu_t gpu;

// The staging area and transfer buffer start this big and grow when a
// frame (or the first upload of a big map) asks for more
#define STAGING_START_BYTES (4u * 1024u * 1024u)
// Uploads are aligned like this in the staging area (texel blocks, floats)
#define STAGING_ALIGNMENT 16u

static SDL_GPUSampler *make_sampler_biased(SDL_GPUFilter filter, SDL_GPUSamplerMipmapMode mipmap_mode, SDL_GPUSamplerAddressMode address_mode,
	float anisotropy, bool compare, float bias)
{
	SDL_GPUSamplerCreateInfo info;

	SDL_zero(info);
	info.mip_lod_bias = bias;
	info.min_filter = filter;
	info.mag_filter = filter;
	info.mipmap_mode = mipmap_mode;
	info.address_mode_u = address_mode;
	info.address_mode_v = address_mode;
	info.address_mode_w = address_mode;
	info.max_lod = 1000.0f;
	if (anisotropy > 1.0f)
	{
		info.enable_anisotropy = true;
		info.max_anisotropy = anisotropy;
	}
	if (compare)
	{
		info.enable_compare = true;
		info.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	}
	return SDL_CreateGPUSampler(gpu.device, &info);
}

static SDL_GPUSampler *make_sampler(SDL_GPUFilter filter, SDL_GPUSamplerMipmapMode mipmap_mode, SDL_GPUSamplerAddressMode address_mode,
	float anisotropy, bool compare)
{
	return make_sampler_biased(filter, mipmap_mode, address_mode, anisotropy, compare, 0.0f);
}

SDL_GPUSampler *gpu_biased_sampler(float bias)
{
	if (gpu.device == NULL || SDL_fabsf(bias) < 0.01f)
	{
		return gpu.linear_repeat;
	}
	if (gpu.linear_repeat_biased == NULL || gpu.linear_repeat_bias != bias)
	{
		SDL_GPUSampler *fresh = make_sampler_biased(SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERMIPMAPMODE_LINEAR, SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
			gpu.maximum_anisotropy, false, bias);

		if (fresh == NULL)
		{
			return gpu.linear_repeat;
		}
		// (the old one let go once the GPU is done with it)
		if (gpu.linear_repeat_biased != NULL)
		{
			SDL_ReleaseGPUSampler(gpu.device, gpu.linear_repeat_biased);
		}
		gpu.linear_repeat_biased = fresh;
		gpu.linear_repeat_bias = bias;
	}
	return gpu.linear_repeat_biased;
}

static void apply_present_mode(void)
{
	SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;

	if (!gpu.vsync)
	{
		if (SDL_WindowSupportsGPUPresentMode(gpu.device, gpu.window, SDL_GPU_PRESENTMODE_IMMEDIATE))
		{
			mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
		}
		else if (SDL_WindowSupportsGPUPresentMode(gpu.device, gpu.window, SDL_GPU_PRESENTMODE_MAILBOX))
		{
			mode = SDL_GPU_PRESENTMODE_MAILBOX;
		}
	}
	if (!SDL_SetGPUSwapchainParameters(gpu.device, gpu.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
	{
		SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "GPU: cannot set the present mode: %s", SDL_GetError());
	}
}

bool gpu_create(SDL_Window *window, bool vsync, float anisotropy)
{
	// Validation layers on request: slow, but they explain every mistake
	bool debug = SDL_getenv("POTOP_GPU_DEBUG") != NULL;

	SDL_zero(gpu);
#if SDL_VERSION_ATLEAST(3, 4, 0)
	// Vulkan 1.1 (the subgroup operations, core there: no feature to
	// enable); the environment's POTOP_NO_VULKAN_11 leaves it at 1.0. If
	// the device will not, as before
	if (SDL_getenv("POTOP_NO_VULKAN_11") == NULL)
	{
		SDL_GPUVulkanOptions options;
		SDL_PropertiesID properties = SDL_CreateProperties();

		SDL_zero(options);
		options.vulkan_api_version = (1u << 22) | (1u << 12);   // VK_MAKE_API_VERSION(0, 1, 1, 0)
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, debug);
		SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
		SDL_SetPointerProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &options);
		gpu.device = SDL_CreateGPUDeviceWithProperties(properties);
		SDL_DestroyProperties(properties);
		gpu.vulkan_11 = gpu.device != NULL;
		if (gpu.device == NULL)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "GPU: no Vulkan 1.1 device (%s); asking for the plain one", SDL_GetError());
		}
	}
	if (gpu.device == NULL)
#endif
	{
		gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, NULL);
	}
	if (gpu.device == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: no device (Vulkan needed): %s", SDL_GetError());
		return false;
	}
	if (window != NULL)
	{
		if (!SDL_ClaimWindowForGPUDevice(gpu.device, window))
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: cannot draw into the window: %s", SDL_GetError());
			SDL_DestroyGPUDevice(gpu.device);
			gpu.device = NULL;
			return false;
		}
		gpu.window = window;
		gpu.vsync = vsync;
		// Three frames in flight (SDL's default two): the CPU may record one
		// more ahead while the GPU works (POTOP_FRAMES_IN_FLIGHT: 1..3)
		{
			const char *asked = SDL_getenv("POTOP_FRAMES_IN_FLIGHT");
			int frames = asked != NULL ? SDL_atoi(asked) : 3;

			if (!SDL_SetGPUAllowedFramesInFlight(gpu.device, (uint32_t)SDL_clamp(frames, 1, 3)))
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "GPU: cannot allow %d frames in flight: %s", frames, SDL_GetError());
			}
		}
		apply_present_mode();
		gpu.swapchain_format = SDL_GetGPUSwapchainTextureFormat(gpu.device, window);
	}

	gpu.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	gpu.shadow_depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
	gpu.scene_sample_count = SDL_GPU_SAMPLECOUNT_1;
	gpu.scene_samples = 1;
	if (!SDL_GPUTextureSupportsFormat(gpu.device, gpu.depth_format, SDL_GPU_TEXTURETYPE_2D,
		SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
	{
		gpu.depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
		gpu.shadow_depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
	}

	gpu.maximum_anisotropy = SDL_clamp(anisotropy, 1.0f, 16.0f);
	gpu.linear_repeat = make_sampler(SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERMIPMAPMODE_LINEAR, SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
		gpu.maximum_anisotropy, false);
	gpu.linear_clamp = make_sampler(SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERMIPMAPMODE_LINEAR, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, 1.0f,
		false);
	gpu.nearest_clamp = make_sampler(SDL_GPU_FILTER_NEAREST, SDL_GPU_SAMPLERMIPMAPMODE_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		1.0f, false);
	gpu.shadow_compare = make_sampler(SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERMIPMAPMODE_NEAREST, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		1.0f, true);

	SDL_Log("GPU: %s, window format %d, depth format %d%s", SDL_GetGPUDeviceDriver(gpu.device), (int)gpu.swapchain_format,
		(int)gpu.depth_format, debug ? ", validation on" : "");
	return gpu.linear_repeat != NULL && gpu.linear_clamp != NULL && gpu.nearest_clamp != NULL && gpu.shadow_compare != NULL;
}

void gpu_set_anisotropy(float anisotropy)
{
	float wanted = SDL_clamp(anisotropy, 1.0f, 16.0f);
	SDL_GPUSampler *fresh;

	if (gpu.device == NULL || wanted == gpu.maximum_anisotropy)
	{
		return;
	}
	fresh = make_sampler(SDL_GPU_FILTER_LINEAR, SDL_GPU_SAMPLERMIPMAPMODE_LINEAR, SDL_GPU_SAMPLERADDRESSMODE_REPEAT, wanted, false);
	if (fresh == NULL)
	{
		return;
	}
	// (the old one is let go once the GPU is done with it; every binding
	// takes gpu.linear_repeat anew)
	SDL_ReleaseGPUSampler(gpu.device, gpu.linear_repeat);
	gpu.linear_repeat = fresh;
	gpu.maximum_anisotropy = wanted;
	// (the biased one made again with it, when next asked)
	if (gpu.linear_repeat_biased != NULL)
	{
		SDL_ReleaseGPUSampler(gpu.device, gpu.linear_repeat_biased);
		gpu.linear_repeat_biased = NULL;
	}
}

void gpu_set_vsync(bool vsync)
{
	if (gpu.device != NULL && gpu.window != NULL && gpu.vsync != vsync)
	{
		gpu.vsync = vsync;
		apply_present_mode();
	}
}

void gpu_destroy(void)
{
	if (gpu.device == NULL)
	{
		return;
	}
	gpu_submit_work(NULL);
	SDL_WaitForGPUIdle(gpu.device);
	SDL_ReleaseGPUSampler(gpu.device, gpu.linear_repeat);
	if (gpu.linear_repeat_biased != NULL)
	{
		SDL_ReleaseGPUSampler(gpu.device, gpu.linear_repeat_biased);
	}
	SDL_ReleaseGPUSampler(gpu.device, gpu.linear_clamp);
	SDL_ReleaseGPUSampler(gpu.device, gpu.nearest_clamp);
	SDL_ReleaseGPUSampler(gpu.device, gpu.shadow_compare);
	if (gpu.transfer_small != NULL)
	{
		SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.transfer_small);
		gpu.transfer_small = NULL;
	}
	if (gpu.transfer != NULL)
	{
		SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.transfer);
	}
	if (gpu.final_texture != NULL)
	{
		SDL_ReleaseGPUTexture(gpu.device, gpu.final_texture);
	}
	SDL_free(gpu.staging);
	SDL_free(gpu.pending);
	SDL_free(gpu.mipmap_requests);
	if (gpu.window != NULL)
	{
		SDL_ReleaseWindowFromGPUDevice(gpu.device, gpu.window);
	}
	SDL_DestroyGPUDevice(gpu.device);
	SDL_zero(gpu);
}

// ---------------------------------------------------------------------------
// Shaders

SDL_GPUShader *gpu_load_shader(const char *name, uint32_t samplers, uint32_t storage_buffers, uint32_t uniform_buffers)
{
	char path[256];
	size_t size = 0;
	void *code;
	SDL_GPUShaderCreateInfo info;
	SDL_GPUShader *shader;

	SDL_snprintf(path, sizeof(path), "shaders/spirv/%s.spv", name);
	code = file_io_load_data_file(path, &size);
	if (code == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: no shader %s (make shaders)", path);
		return NULL;
	}
	SDL_zero(info);
	info.code = code;
	info.code_size = size;
	info.entrypoint = "main";
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.stage = SDL_strstr(name, ".frag") != NULL ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
	info.num_samplers = samplers;
	info.num_storage_buffers = storage_buffers;
	info.num_uniform_buffers = uniform_buffers;
	shader = SDL_CreateGPUShader(gpu.device, &info);
	SDL_free(code);
	if (shader == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: shader %s: %s", name, SDL_GetError());
	}
	return shader;
}

SDL_GPUComputePipeline *gpu_load_compute(const char *name, const gpu_compute_layout_t *layout)
{
	char path[256];
	size_t size = 0;
	void *code;
	SDL_GPUComputePipelineCreateInfo info;
	SDL_GPUComputePipeline *pipeline;

	SDL_snprintf(path, sizeof(path), "shaders/spirv/%s.spv", name);
	code = file_io_load_data_file(path, &size);
	if (code == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: no shader %s (make shaders)", path);
		return NULL;
	}
	SDL_zero(info);
	info.code = code;
	info.code_size = size;
	info.entrypoint = "main";
	info.format = SDL_GPU_SHADERFORMAT_SPIRV;
	info.num_samplers = layout->samplers;
	info.num_readonly_storage_buffers = layout->readonly_storage_buffers;
	info.num_readwrite_storage_buffers = layout->readwrite_storage_buffers;
	info.num_uniform_buffers = layout->uniform_buffers;
	info.num_readonly_storage_textures = layout->readonly_storage_textures;
	info.num_readwrite_storage_textures = layout->readwrite_storage_textures;
	info.threadcount_x = SDL_max(layout->threads_x, 1u);
	info.threadcount_y = 1;
	info.threadcount_z = 1;
	pipeline = SDL_CreateGPUComputePipeline(gpu.device, &info);
	SDL_free(code);
	if (pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: compute shader %s: %s", name, SDL_GetError());
	}
	return pipeline;
}

// ---------------------------------------------------------------------------
// Resources

uint32_t gpu_mip_count(uint32_t width, uint32_t height)
{
	uint32_t largest = SDL_max(width, height);
	uint32_t count = 1;

	while (largest > 1)
	{
		largest >>= 1;
		count++;
	}
	return count;
}

SDL_GPUTexture *gpu_create_texture(SDL_GPUTextureType type, SDL_GPUTextureFormat format, uint32_t width, uint32_t height,
	uint32_t layers, uint32_t levels, SDL_GPUTextureUsageFlags usage)
{
	SDL_GPUTextureCreateInfo info;
	SDL_GPUTexture *texture;

	SDL_zero(info);
	info.type = type;
	info.format = format;
	info.usage = usage;
	info.width = SDL_max(width, 1u);
	info.height = SDL_max(height, 1u);
	info.layer_count_or_depth = SDL_max(layers, 1u);
	info.num_levels = levels == 0 ? gpu_mip_count(info.width, info.height) : levels;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	texture = SDL_CreateGPUTexture(gpu.device, &info);
	if (texture == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: texture %ux%u (format %d): %s", info.width, info.height, (int)format, SDL_GetError());
	}
	return texture;
}

SDL_GPUBuffer *gpu_create_buffer(SDL_GPUBufferUsageFlags usage, uint32_t size)
{
	SDL_GPUBufferCreateInfo info;
	SDL_GPUBuffer *buffer;

	SDL_zero(info);
	info.usage = usage;
	info.size = SDL_max(size, 16u);
	buffer = SDL_CreateGPUBuffer(gpu.device, &info);
	if (buffer == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: buffer of %u bytes: %s", size, SDL_GetError());
	}
	return buffer;
}

// ---------------------------------------------------------------------------
// Uploads

// Loading many textures at once would pile up hundreds of megabytes: past
// this much waiting, the uploads go out first (never inside a pass: nobody
// uploads there)
#define STAGING_FLUSH_BYTES (64u * 1024u * 1024u)

static gpu_pending_upload_t *stage(const void *data, size_t size)
{
	size_t offset;
	gpu_pending_upload_t *upload;

	if (gpu.pending_count > 0 && gpu.staging_used + size > STAGING_FLUSH_BYTES)
	{
		gpu_flush_uploads();
	}
	offset = (gpu.staging_used + STAGING_ALIGNMENT - 1) & ~(size_t)(STAGING_ALIGNMENT - 1);

	if (offset + size > gpu.staging_capacity)
	{
		size_t capacity = SDL_max(gpu.staging_capacity, (size_t)STAGING_START_BYTES);
		uint8_t *grown;

		while (capacity < offset + size)
		{
			capacity *= 2;
		}
		grown = SDL_realloc(gpu.staging, capacity);
		if (grown == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: out of memory staging %zu bytes", size);
			return NULL;
		}
		gpu.staging = grown;
		gpu.staging_capacity = capacity;
	}
	if (gpu.pending_count == gpu.pending_capacity)
	{
		int capacity = SDL_max(64, gpu.pending_capacity * 2);
		gpu_pending_upload_t *grown = SDL_realloc(gpu.pending, sizeof(gpu_pending_upload_t) * (size_t)capacity);

		if (grown == NULL)
		{
			return NULL;
		}
		gpu.pending = grown;
		gpu.pending_capacity = capacity;
	}
	SDL_memcpy(gpu.staging + offset, data, size);
	gpu.staging_used = offset + size;
	upload = &gpu.pending[gpu.pending_count++];
	SDL_zerop(upload);
	upload->staging_offset = offset;
	upload->size = size;
	return upload;
}

void gpu_upload_texture(SDL_GPUTexture *texture, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
	const void *data, size_t size)
{
	gpu_pending_upload_t *upload;

	if (texture == NULL || data == NULL || size == 0)
	{
		return;
	}
	upload = stage(data, size);
	if (upload == NULL)
	{
		return;
	}
	upload->texture = texture;
	upload->level = level;
	upload->layer = layer;
	upload->x = x;
	upload->y = y;
	upload->width = width;
	upload->height = height;
	upload->depth = 1;
}

void gpu_upload_volume(SDL_GPUTexture *texture, uint32_t width, uint32_t height, uint32_t depth, const void *data, size_t size)
{
	gpu_upload_texture(texture, 0, 0, 0, 0, width, height, data, size);
	if (gpu.pending_count > 0 && gpu.pending[gpu.pending_count - 1].texture == texture)
	{
		gpu.pending[gpu.pending_count - 1].depth = depth;
	}
}

void gpu_upload_buffer(SDL_GPUBuffer *buffer, uint32_t offset, const void *data, size_t size)
{
	gpu_pending_upload_t *upload;

	if (buffer == NULL || data == NULL || size == 0)
	{
		return;
	}
	upload = stage(data, size);
	if (upload == NULL)
	{
		return;
	}
	upload->buffer = buffer;
	upload->offset = offset;
}

void gpu_generate_mipmaps(SDL_GPUTexture *texture)
{
	if (texture == NULL)
	{
		return;
	}
	if (gpu.mipmap_count == gpu.mipmap_capacity)
	{
		int capacity = SDL_max(16, gpu.mipmap_capacity * 2);
		SDL_GPUTexture **grown = SDL_realloc(gpu.mipmap_requests, sizeof(SDL_GPUTexture *) * (size_t)capacity);

		if (grown == NULL)
		{
			return;
		}
		gpu.mipmap_requests = grown;
		gpu.mipmap_capacity = capacity;
	}
	gpu.mipmap_requests[gpu.mipmap_count++] = texture;
}

static void record_uploads(SDL_GPUCommandBuffer *command_buffer)
{
	SDL_GPUCopyPass *copy_pass;
	uint8_t *mapped;
	SDL_GPUTransferBuffer *transfer = NULL;

	if (gpu.pending_count > 0)
	{
		// (a frame's uploads through the small buffer, a load's through
		// the big one)
		if (gpu.staging_used <= STAGING_START_BYTES)
		{
			if (gpu.transfer_small == NULL)
			{
				SDL_GPUTransferBufferCreateInfo info;

				SDL_zero(info);
				info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				info.size = STAGING_START_BYTES;
				gpu.transfer_small = SDL_CreateGPUTransferBuffer(gpu.device, &info);
			}
			transfer = gpu.transfer_small;
		}
		if (transfer == NULL)
		{
			if (gpu.transfer == NULL || gpu.transfer_capacity < gpu.staging_used)
			{
				SDL_GPUTransferBufferCreateInfo info;

				if (gpu.transfer != NULL)
				{
					SDL_ReleaseGPUTransferBuffer(gpu.device, gpu.transfer);
				}
				SDL_zero(info);
				info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
				info.size = (uint32_t)SDL_max(gpu.staging_capacity, (size_t)STAGING_START_BYTES);
				gpu.transfer = SDL_CreateGPUTransferBuffer(gpu.device, &info);
				gpu.transfer_capacity = gpu.transfer != NULL ? info.size : 0;
				// (it is mapped cycled at every flush that needs it: its size
				// is what each of those may ask the driver to back anew)
				SDL_Log("GPU: the uploads' transfer buffer made, %.1f MB", (double)info.size / (1024.0 * 1024.0));
				if (gpu.transfer == NULL)
				{
					SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: transfer buffer of %u bytes: %s", info.size, SDL_GetError());
					gpu.pending_count = 0;
					gpu.staging_used = 0;
					return;
				}
			}
			transfer = gpu.transfer;
		}
		// Cycled: the last frame's uploads may still be reading the old one
		mapped = SDL_MapGPUTransferBuffer(gpu.device, transfer, true);
		if (mapped == NULL)
		{
			gpu.pending_count = 0;
			gpu.staging_used = 0;
			return;
		}
		SDL_memcpy(mapped, gpu.staging, gpu.staging_used);
		SDL_UnmapGPUTransferBuffer(gpu.device, transfer);

		copy_pass = SDL_BeginGPUCopyPass(command_buffer);
		for (int i = 0; i < gpu.pending_count; i++)
		{
			const gpu_pending_upload_t *upload = &gpu.pending[i];

			if (upload->texture != NULL)
			{
				SDL_GPUTextureTransferInfo source;
				SDL_GPUTextureRegion destination;

				SDL_zero(source);
				source.transfer_buffer = transfer;
				source.offset = (uint32_t)upload->staging_offset;
				SDL_zero(destination);
				destination.texture = upload->texture;
				destination.mip_level = upload->level;
				destination.layer = upload->layer;
				destination.x = upload->x;
				destination.y = upload->y;
				destination.w = upload->width;
				destination.h = upload->height;
				destination.d = SDL_max(upload->depth, 1u);
				SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
			}
			else
			{
				SDL_GPUTransferBufferLocation source;
				SDL_GPUBufferRegion destination;

				source.transfer_buffer = transfer;
				source.offset = (uint32_t)upload->staging_offset;
				destination.buffer = upload->buffer;
				destination.offset = upload->offset;
				destination.size = (uint32_t)upload->size;
				SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
			}
		}
		SDL_EndGPUCopyPass(copy_pass);
		gpu.uploaded_bytes += gpu.staging_used;
		frame_profile_count(FRAME_UPLOAD_FLUSHES, 1);
		frame_profile_count(FRAME_UPLOAD_KILOBYTES, (int32_t)((gpu.staging_used + 1023) / 1024));
	}
	for (int i = 0; i < gpu.mipmap_count; i++)
	{
		SDL_GenerateMipmapsForGPUTexture(command_buffer, gpu.mipmap_requests[i]);
	}
	gpu.pending_count = 0;
	gpu.staging_used = 0;
	gpu.mipmap_count = 0;
}

void gpu_flush_uploads(void)
{
	if (gpu.device == NULL || (gpu.pending_count == 0 && gpu.mipmap_count == 0))
	{
		return;
	}
	{
		SDL_GPUCommandBuffer *command_buffer = gpu_commands();

		if (command_buffer != NULL)
		{
			record_uploads(command_buffer);
		}
	}
}

SDL_GPUCommandBuffer *gpu_commands(void)
{
	if (gpu.command_buffer != NULL)
	{
		return gpu.command_buffer;
	}
	if (gpu.work_command_buffer == NULL && gpu.device != NULL)
	{
		gpu.work_command_buffer = SDL_AcquireGPUCommandBuffer(gpu.device);
		if (gpu.work_command_buffer == NULL)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: no command buffer: %s", SDL_GetError());
		}
	}
	return gpu.work_command_buffer;
}

void gpu_submit_work(SDL_GPUFence **fence_out)
{
	if (fence_out != NULL)
	{
		*fence_out = NULL;
	}
	if (gpu.device == NULL || gpu.command_buffer != NULL)
	{
		return;
	}
	if (gpu.pending_count > 0 || gpu.mipmap_count > 0)
	{
		gpu_flush_uploads();
	}
	if (gpu.work_command_buffer == NULL)
	{
		return;
	}
	if (fence_out != NULL)
	{
		*fence_out = SDL_SubmitGPUCommandBufferAndAcquireFence(gpu.work_command_buffer);
	}
	else
	{
		SDL_SubmitGPUCommandBuffer(gpu.work_command_buffer);
	}
	gpu.work_command_buffer = NULL;
}

// ---------------------------------------------------------------------------
// Frames

bool gpu_begin_frame(void)
{
	uint64_t submit_start = SDL_GetPerformanceCounter();
	// What was recorded since the last frame goes first
	gpu_submit_work(NULL);
	frame_profile_end(FRAME_PRESENT, submit_start);
	gpu.command_buffer = SDL_AcquireGPUCommandBuffer(gpu.device);
	gpu.swapchain_texture = NULL;
	if (gpu.command_buffer == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: no command buffer: %s", SDL_GetError());
		return false;
	}
	// The window's picture is not asked for here but at the end
	// (gpu_end_frame): waiting for it (for a frame in flight to finish)
	// before the frame is recorded kept the GPU idle while the CPU
	// recorded; now the recording goes on while the GPU still works on the
	// frames before. The frame's size the window's now
	{
		int width = 0;
		int height = 0;

		if (gpu.window == NULL || !SDL_GetWindowSizeInPixels(gpu.window, &width, &height) || width <= 0 || height <= 0)
		{
			return false;
		}
		gpu.window_width = (uint32_t)width;
		gpu.window_height = (uint32_t)height;
	}
	if (gpu.final_texture == NULL || gpu.final_width != gpu.window_width || gpu.final_height != gpu.window_height)
	{
		if (gpu.final_texture != NULL)
		{
			SDL_ReleaseGPUTexture(gpu.device, gpu.final_texture);
		}
		gpu.final_texture = gpu_create_texture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, gpu.window_width,
			gpu.window_height, 1, 1, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		gpu.final_width = gpu.window_width;
		gpu.final_height = gpu.window_height;
	}
	return gpu.final_texture != NULL;
}

void gpu_end_frame(uint8_t **pixels_out, uint32_t *width_out, uint32_t *height_out)
{
	SDL_GPUTransferBuffer *download = NULL;
	SDL_GPUFence *fence;

	if (pixels_out != NULL)
	{
		*pixels_out = NULL;
	}
	if (gpu.command_buffer == NULL)
	{
		return;
	}
	gpu_flush_uploads();
	// The window's picture, as late as can be (gpu_begin_frame); its size
	// may differ from the frame's for a frame while the window is resized:
	// the blit scales
	if (gpu.window != NULL && gpu.final_texture != NULL)
	{
		uint64_t acquire_start = SDL_GetPerformanceCounter();
		uint32_t swapchain_width = 0;
		uint32_t swapchain_height = 0;

		if (!SDL_WaitAndAcquireGPUSwapchainTexture(gpu.command_buffer, gpu.window, &gpu.swapchain_texture, &swapchain_width,
			&swapchain_height))
		{
			gpu.swapchain_texture = NULL;
		}
		frame_profile_end(FRAME_ACQUIRE, acquire_start);
		if (gpu.swapchain_texture != NULL)
		{
			gpu.window_width = swapchain_width;
			gpu.window_height = swapchain_height;
		}
	}
	if (gpu.swapchain_texture != NULL && gpu.final_texture != NULL)
	{
		SDL_GPUBlitInfo blit;

		SDL_zero(blit);
		blit.source.texture = gpu.final_texture;
		blit.source.w = gpu.final_width;
		blit.source.h = gpu.final_height;
		blit.destination.texture = gpu.swapchain_texture;
		blit.destination.w = gpu.window_width;
		blit.destination.h = gpu.window_height;
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_NEAREST;
		SDL_BlitGPUTexture(gpu.command_buffer, &blit);

		if (pixels_out != NULL)
		{
			SDL_GPUTransferBufferCreateInfo info;

			SDL_zero(info);
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = gpu.final_width * gpu.final_height * 4;
			download = SDL_CreateGPUTransferBuffer(gpu.device, &info);
			if (download != NULL)
			{
				SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(gpu.command_buffer);
				SDL_GPUTextureRegion source;
				SDL_GPUTextureTransferInfo destination;

				SDL_zero(source);
				source.texture = gpu.final_texture;
				source.w = gpu.final_width;
				source.h = gpu.final_height;
				source.d = 1;
				SDL_zero(destination);
				destination.transfer_buffer = download;
				SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
				SDL_EndGPUCopyPass(copy_pass);
			}
		}
	}
	if (download == NULL)
	{
		SDL_SubmitGPUCommandBuffer(gpu.command_buffer);
		gpu.command_buffer = NULL;
		return;
	}
	fence = SDL_SubmitGPUCommandBufferAndAcquireFence(gpu.command_buffer);
	gpu.command_buffer = NULL;
	if (fence != NULL)
	{
		size_t size = (size_t)gpu.final_width * gpu.final_height * 4;
		uint8_t *pixels = SDL_malloc(size);
		void *mapped;

		SDL_WaitForGPUFences(gpu.device, true, &fence, 1);
		SDL_ReleaseGPUFence(gpu.device, fence);
		mapped = SDL_MapGPUTransferBuffer(gpu.device, download, false);
		if (pixels != NULL && mapped != NULL)
		{
			SDL_memcpy(pixels, mapped, size);
			*pixels_out = pixels;
			*width_out = gpu.final_width;
			*height_out = gpu.final_height;
		}
		else
		{
			SDL_free(pixels);
		}
		if (mapped != NULL)
		{
			SDL_UnmapGPUTransferBuffer(gpu.device, download);
		}
	}
	SDL_ReleaseGPUTransferBuffer(gpu.device, download);
}


void gpu_clip_matrix(const float gl[16], float out[16])
{
	SDL_memcpy(out, gl, sizeof(float) * 16);
	// Column major: row 2 (z) becomes the average of rows 2 and 3, which
	// takes depth from -w..w to 0..w
	for (int column = 0; column < 4; column++)
	{
		out[column * 4 + 2] = 0.5f * gl[column * 4 + 2] + 0.5f * gl[column * 4 + 3];
	}
}

void gpu_set_scene_rect(int32_t x, int32_t y, int32_t width, int32_t height)
{
	gpu.scene_x = x;
	gpu.scene_y = y;
	gpu.scene_width = SDL_max(width, 0);
	gpu.scene_height = SDL_max(height, 0);
}

void gpu_set_scene_samples(int samples)
{
	static const SDL_GPUSampleCount counts[4] = { SDL_GPU_SAMPLECOUNT_1, SDL_GPU_SAMPLECOUNT_2, SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_8 };
	int level = samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;

	// (all three of the scene's targets must take it)
	while (level > 0 && !(SDL_GPUTextureSupportsSampleCount(gpu.device, GPU_SCENE_COLOR_FORMAT, counts[level])
		&& SDL_GPUTextureSupportsSampleCount(gpu.device, GPU_SCENE_DEPTH_VALUE_FORMAT, counts[level])
		&& SDL_GPUTextureSupportsSampleCount(gpu.device, gpu.depth_format, counts[level])))
	{
		level--;
	}
	gpu.scene_sample_count = counts[level];
	gpu.scene_samples = 1u << level;
	if (samples > 1)
	{
		SDL_Log("Multisampling the scene: %u samples a pixel", gpu.scene_samples);
	}
}

SDL_GPUGraphicsPipeline *gpu_make_pipeline(const gpu_pipeline_info_t *info, const char *name)
{
	SDL_GPUGraphicsPipelineCreateInfo create;
	SDL_GPUColorTargetDescription colors[4];
	SDL_GPUColorTargetDescription color;
	SDL_GPUGraphicsPipeline *pipeline;
	uint32_t extra_count = SDL_min(info->extra_color_target_count, 3u);

	if (info->vertex_shader == NULL || info->fragment_shader == NULL)
	{
		return NULL;
	}
	SDL_zero(create);
	SDL_zero(color);
	create.vertex_shader = info->vertex_shader;
	create.fragment_shader = info->fragment_shader;
	create.vertex_input_state.vertex_buffer_descriptions = info->vertex_buffers;
	create.vertex_input_state.num_vertex_buffers = info->vertex_buffer_count;
	create.vertex_input_state.vertex_attributes = info->vertex_attributes;
	create.vertex_input_state.num_vertex_attributes = info->vertex_attribute_count;
	create.primitive_type = info->primitive;
	create.rasterizer_state.fill_mode = info->fill;
	create.rasterizer_state.cull_mode = info->cull;
	create.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	if (info->depth_bias_constant != 0.0f || info->depth_bias_slope != 0.0f)
	{
		create.rasterizer_state.enable_depth_bias = true;
		create.rasterizer_state.depth_bias_constant_factor = info->depth_bias_constant;
		create.rasterizer_state.depth_bias_slope_factor = info->depth_bias_slope;
	}
	create.rasterizer_state.enable_depth_clip = true;
	create.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	if (info->multisampled && !info->depth_only && gpu.scene_samples > 1)
	{
		create.multisample_state.sample_count = gpu.scene_sample_count;
		create.multisample_state.enable_alpha_to_coverage = info->alpha_to_coverage;
	}
	create.depth_stencil_state.enable_depth_test = info->depth_test;
	create.depth_stencil_state.enable_depth_write = info->depth_write;
	create.depth_stencil_state.compare_op = info->depth_compare != 0 ? info->depth_compare : SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	if (!info->depth_only)
	{
		color.format = info->color_format != 0 ? info->color_format : GPU_SCENE_COLOR_FORMAT;
		if (info->blend != GPU_BLEND_NONE)
		{
			color.blend_state.enable_blend = true;
			color.blend_state.src_color_blendfactor = info->blend == GPU_BLEND_ALPHA ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
			color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			if (info->blend == GPU_BLEND_MULTIPLY)
			{
				color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
				color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR;
				color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
				color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			}
			if (info->blend == GPU_BLEND_ADD)
			{
				color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
				color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
				color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			}
			// Over the scene (its format: the water, the fog, the clouds, the
			// particles) the alpha stays: it marks the buildings' pixels for
			// the temporal anti-aliasing (taa.frag)
			if (info->color_format == 0)
			{
				color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
				color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			}
		}
		SDL_zeroa(colors);
		colors[0] = color;
		for (uint32_t i = 0; i < extra_count; i++)
		{
			colors[1 + i].format = info->extra_color_format;
		}
		// (the multisampled scene's depth values: location 1, no extra
		// targets with it)
		if (info->multisampled && gpu.scene_samples > 1)
		{
			colors[1].format = GPU_SCENE_DEPTH_VALUE_FORMAT;
			extra_count = 1;
		}
		create.target_info.color_target_descriptions = colors;
		create.target_info.num_color_targets = 1 + extra_count;
	}
	if (!info->no_depth)
	{
		create.target_info.has_depth_stencil_target = true;
		// (a shadow map's own)
		create.target_info.depth_stencil_format = info->depth_only ? (info->depth_format != 0 ? info->depth_format : gpu.shadow_depth_format)
			: gpu.depth_format;
	}
	pipeline = SDL_CreateGPUGraphicsPipeline(gpu.device, &create);
	if (pipeline == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "GPU: pipeline %s: %s", name, SDL_GetError());
	}
	return pipeline;
}
