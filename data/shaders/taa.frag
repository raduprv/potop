#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Temporal anti-aliasing (see post.h, post_temporal): the buildings are drawn
// a fraction of a pixel off each frame (prop_vertex.glsl, world_render), and
// each of their pixels (marked in the alpha) is averaged with what it was
// over the last frames, found
// where it was on the screen then. The camera's motion alone moves it there
// (the depth back into the world, then into last frame's view): what moves
// by itself (the grass in the wind, a bear) is held to the colors around it
// this frame, so it leaves no trail.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D current_texture;  // this frame's scene (linear light)
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D history_texture;  // last frame's result
layout(set = TEXTURE_SET, binding = 2) uniform sampler2D depth_texture;    // this frame's depth

layout(std140, set = UNIFORM_SET, binding = 0) uniform temporal_block
{
	mat4 reprojection;          // this frame's OpenGL style clip space (unjittered) into last frame's
	vec4 texel;                 // xy: 1 / the size; z: the history's weight (0.9); w: 1 if there is a history
	vec4 temporal_parameters;   // x: depth under which nothing moves (the weapon in hand, near the eye); y: sharpening of the history taps (0..1)
};

layout(location = 0) in vec2 screen_uv;

layout(location = 0) out vec4 fragment_color;

vec3 to_ycocg(vec3 color)
{
	return vec3(dot(color, vec3(0.25, 0.5, 0.25)), dot(color, vec3(0.5, 0.0, -0.5)), dot(color, vec3(-0.25, 0.5, -0.25)));
}

vec3 from_ycocg(vec3 color)
{
	return vec3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

// Bright pixels count less (else a glint drags its neighbors along)
vec3 compress(vec3 color)
{
	return color / (1.0 + max(max(color.r, color.g), color.b));
}

// (its brightest channel held under 1: the box and the blend in YCoCg can
// push a channel up to 1 or past it, and divided by next to nothing it
// became a pixel thousands of times too bright, flickering)
vec3 uncompress(vec3 color)
{
	color = clamp(color, vec3(0.0), vec3(0.96));
	return color / (1.0 - max(max(color.r, color.g), color.b));
}

vec3 fetch_current(ivec2 pixel)
{
	ivec2 size = textureSize(current_texture, 0);
	vec3 color = texelFetch(current_texture, clamp(pixel, ivec2(0), size - 1), 0).rgb;

	return to_ycocg(compress(max(color, vec3(0.0))));
}

// The history between its texels: Catmull-Rom from five bilinear taps
// (sharper than one bilinear tap, which blurs a little more every frame)
vec3 sample_history(vec2 uv)
{
	vec2 size = 1.0 / texel.xy;
	vec2 position = uv * size;
	vec2 center = floor(position - 0.5) + 0.5;
	vec2 f = position - center;
	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);
	vec2 w12 = w1 + w2;
	vec2 offset12 = w2 / w12;
	vec2 uv0 = (center - 1.0) * texel.xy;
	vec2 uv3 = (center + 2.0) * texel.xy;
	vec2 uv12 = (center + offset12) * texel.xy;
	vec3 sum = vec3(0.0);
	float weight = 0.0;

	sum += textureLod(history_texture, vec2(uv12.x, uv0.y), 0.0).rgb * (w12.x * w0.y);
	sum += textureLod(history_texture, vec2(uv0.x, uv12.y), 0.0).rgb * (w0.x * w12.y);
	sum += textureLod(history_texture, vec2(uv12.x, uv12.y), 0.0).rgb * (w12.x * w12.y);
	sum += textureLod(history_texture, vec2(uv3.x, uv12.y), 0.0).rgb * (w3.x * w12.y);
	sum += textureLod(history_texture, vec2(uv12.x, uv3.y), 0.0).rgb * (w12.x * w3.y);
	weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	vec3 sharp = max(sum / max(weight, 1e-4), vec3(0.0));
	vec3 soft = textureLod(history_texture, uv, 0.0).rgb;

	return mix(soft, sharp, temporal_parameters.y);
}

// The history pulled into the box of this frame's colors round the pixel,
// along the line to the box's middle
vec3 clip_to_box(vec3 history, vec3 box_minimum, vec3 box_maximum)
{
	vec3 middle = 0.5 * (box_maximum + box_minimum);
	vec3 extent = 0.5 * (box_maximum - box_minimum) + 1e-5;
	vec3 offset = history - middle;
	vec3 units = abs(offset / extent);
	float most = max(units.x, max(units.y, units.z));

	return most > 1.0 ? middle + offset / most : history;
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec4 raw = texelFetch(current_texture, pixel, 0);

	// Only the buildings' pixels (alpha 0.25, prop_body.glsl): the rest,
	// drawn where it is, as it is (the trees' needles stay sharp)
	if (raw.a > 0.5)
	{
		fragment_color = vec4(max(raw.rgb, vec3(0.0)), 1.0);
		return;
	}
	vec3 center = fetch_current(pixel);
	vec3 box_minimum = center;
	vec3 box_maximum = center;
	vec3 mean = center;
	vec3 mean_square = center * center;
	// The nearest depth round the pixel: an edge moves with what is in front
	float depth = texelFetch(depth_texture, pixel, 0).r;
	ivec2 nearest_pixel = pixel;

	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			if (x == 0 && y == 0)
			{
				continue;
			}
			vec3 color = fetch_current(pixel + ivec2(x, y));

			box_minimum = min(box_minimum, color);
			box_maximum = max(box_maximum, color);
			mean += color;
			mean_square += color * color;
			float here = texelFetch(depth_texture, clamp(pixel + ivec2(x, y), ivec2(0), textureSize(depth_texture, 0) - 1), 0).r;

			if (here < depth)
			{
				depth = here;
				nearest_pixel = pixel + ivec2(x, y);
			}
		}
	}
	mean /= 9.0;
	// (the box narrowed to where most of the colors are: less of the
	// history's stale color gets through on a moving edge)
	vec3 deviation = sqrt(max(mean_square / 9.0 - mean * mean, vec3(0.0)));

	box_minimum = max(box_minimum, mean - 1.25 * deviation);
	box_maximum = min(box_maximum, mean + 1.25 * deviation);

	// Where this pixel was last frame
	vec2 previous_uv = screen_uv;
	bool still = depth < temporal_parameters.x;

	if (!still)
	{
		vec2 point = (vec2(nearest_pixel) + 0.5) * texel.xy;
		vec4 previous = reprojection * vec4(point.x * 2.0 - 1.0, 1.0 - point.y * 2.0, depth * 2.0 - 1.0, 1.0);

		previous.xy /= max(previous.w, 1e-6);
		previous_uv = screen_uv + (vec2(previous.x * 0.5 + 0.5, 0.5 - previous.y * 0.5) - point);
	}
	float history_weight = texel.z;

	if (texel.w < 0.5 || any(lessThan(previous_uv, vec2(0.0))) || any(greaterThan(previous_uv, vec2(1.0))))
	{
		history_weight = 0.0;
	}
	vec3 result = center;

	if (history_weight > 0.0)
	{
		vec3 history = to_ycocg(compress(sample_history(previous_uv)));

		history = clip_to_box(history, box_minimum, box_maximum);
		result = mix(center, history, history_weight);
	}
	result = uncompress(from_ycocg(result));
	if (any(isnan(result)) || any(isinf(result)))
	{
		result = vec3(0.0);
	}
	fragment_color = vec4(result, 1.0);
}
