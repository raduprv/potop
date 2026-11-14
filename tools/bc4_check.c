#include <SDL3/SDL.h>
#include <stdio.h>

int main(void)
{
	SDL_GPUDevice *device;

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		printf("SDL: %s\n", SDL_GetError());
		return 1;
	}
	device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
	if (device == NULL)
	{
		printf("No GPU: %s\n", SDL_GetError());
		return 1;
	}
	printf("driver %s\n", SDL_GetGPUDeviceDriver(device));
	printf("BC4 3D sampler: %s\n", SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM, SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? "yes" : "no");
	printf("BC4 2D sampler: %s\n", SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? "yes" : "no");
	printf("R8 3D sampler:  %s\n", SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? "yes" : "no");
	SDL_DestroyGPUDevice(device);
	SDL_Quit();
	return 0;
}
