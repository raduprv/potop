#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The occlusion culling seen (objects_draw_occlusion_debug, console:
// objects occlusion show|depth|all), over the scene: each box the camera's
// culling hid this frame (objects_cull.comp's occlusion_buffer) outlined
// in red and faintly filled, the pyramid's texels read for it outlined in
// yellow; asked for (all), each box it let be drawn outlined in green, its
// texels too; asked for (depth), the pyramid's level 0 under them in gray,
// near dark, far light (made linear and spread logarithmically), the sky
// blue. More boxes than kept: a red bar along the top.

layout(location = 0) in vec2 screen_uv;
layout(location = 0) out vec4 color_out;

layout(std430, set = TEXTURE_SET, binding = 0) readonly buffer occlusion_buffer
{
	uint occlusion_count;
	uint occlusion_unused[3];
	vec4 occlusion_records[];
};
layout(std430, set = TEXTURE_SET, binding = 1) readonly buffer hiz_buffer { float hiz_depths[]; };

layout(std140, set = UNIFORM_SET, binding = 0) uniform debug_block
{
	vec4 sizes;                    // the pyramid's level 0 width and height; z: 1 its depth drawn; w: the most records
	vec4 depth_range;              // the camera's near and far (m)
};

// 1 on a rectangle's outline (0..1 from the top left: least xy, most zw),
// width pixels wide inside it
float outline(vec4 rect, vec2 uv, vec2 pixel, float width)
{
	vec2 inner_minimum = rect.xy + pixel * width;
	vec2 inner_maximum = rect.zw - pixel * width;
	bool inside = all(greaterThanEqual(uv, rect.xy)) && all(lessThanEqual(uv, rect.zw));
	bool inner = all(greaterThanEqual(uv, inner_minimum)) && all(lessThanEqual(uv, inner_maximum));

	return inside && !inner ? 1.0 : 0.0;
}

void main()
{
	vec2 pixel = max(fwidth(screen_uv), vec2(1e-5));
	vec4 color = vec4(0.0);

	if (sizes.z > 0.5)
	{
		ivec2 size = ivec2(sizes.xy);
		ivec2 texel = clamp(ivec2(screen_uv * sizes.xy), ivec2(0), size - 1);
		float depth = hiz_depths[texel.y * size.x + texel.x];

		if (depth >= 0.999999)
		{
			color = vec4(0.2, 0.4, 0.9, 0.6);
		}
		else
		{
			float near_distance = depth_range.x;
			float far_distance = depth_range.y;
			float meters = near_distance * far_distance / max(far_distance - depth * (far_distance - near_distance), 1e-6);
			float shade = clamp(log(max(meters, near_distance) / near_distance) / log(far_distance / near_distance), 0.0, 1.0);

			color = vec4(vec3(shade), 0.85);
		}
	}
	uint count = min(occlusion_count, uint(sizes.w));

	for (uint i = 0u; i < count; i++)
	{
		vec4 box = occlusion_records[3u * i];
		vec4 texels = occlusion_records[3u * i + 1u];
		bool hidden = occlusion_records[3u * i + 2u].w > 0.5;

		if (hidden && all(greaterThanEqual(screen_uv, box.xy)) && all(lessThanEqual(screen_uv, box.zw)))
		{
			color = vec4(mix(color.rgb, vec3(1.0, 0.0, 0.0), 0.3), max(color.a, 0.2));
		}
		if (outline(texels, screen_uv, pixel, hidden ? 2.0 : 1.0) > 0.5)
		{
			color = hidden ? vec4(1.0, 0.9, 0.0, 0.9) : vec4(1.0, 0.9, 0.0, max(color.a, 0.45));
		}
		if (outline(box, screen_uv, pixel, 2.0) > 0.5)
		{
			color = hidden ? vec4(1.0, 0.1, 0.1, 0.95) : vec4(0.1, 1.0, 0.2, 0.9);
		}
	}
	if (occlusion_count > uint(sizes.w) && screen_uv.y < pixel.y * 6.0)
	{
		color = vec4(1.0, 0.0, 0.0, 1.0);
	}
	color_out = color;
}
