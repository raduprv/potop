#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// Ambient occlusion, step 1 (ambient_occlusion.h): at half the scene's size,
// GTAO (Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate
// Indirect Occlusion"). Each pixel looks along OCCLUSION_SLICES directions
// on the screen, both ways from it, at OCCLUSION_STEPS points each, for the
// highest horizon within the radius; the two horizons of a slice, against
// the pixel's facing in that slice, give the sky it sees there, weighted by
// the cosine (as the diffuse light takes it). Out: the occlusion (1 open,
// 0 shut in), the pixel's distance from the eye (below 0: the sky, nothing
// here), for step 2 to gather by.
//
// And the light bounced off what it finds (screen space indirect light,
// the horizons' way): each point that raises a horizon covers the slice's
// arc between the horizon before and its own, cosine weighted as the sky's;
// its color in the scene (lit this frame, the sun and the sky on it) times
// that share of the hemisphere is the light it sends here. Out in the
// second target: that light (as the scene's colors: step 2 turns it into
// what this pixel gives back, by its own light). The finders' facing is
// not known (no normals but from the depth): a wall's back lights too.
//
// Everything in the world from the eye (world minus eye_position:
// clip_to_eye), so that a pixel kilometers out is as precise as one near.

layout(set = TEXTURE_SET, binding = 0) uniform sampler2D depth_texture;
layout(set = TEXTURE_SET, binding = 1) uniform sampler2D scene_texture;   // this frame's scene so far (the light the found points send)

layout(std140, set = UNIFORM_SET, binding = 1) uniform occlusion_block
{
	vec4 occlusion_parameters;     // x: radius (m), y: the distance it is gone at (m), z: strength, w: 1 if the shadow maps are bound
	vec4 occlusion_size;           // the scene's width and height, the half size target's
	vec4 occlusion_frame;          // x: the frame (0..15: the pattern's turn), y: 1 if there is a history, z: the bounced light's strength (0: none), w: its radius (m)
};

layout(location = 0) in vec2 screen_point;

layout(location = 0) out vec4 fragment_color;
layout(location = 1) out vec4 fragment_indirect;  // the bounced light (rgb)

const int OCCLUSION_SLICES = 3;
const int OCCLUSION_STEPS = 5;
const int BOUNCE_STEPS = 3;                     // the bounced light's own, beyond the occlusion's radius out to its own
const float BOUNCE_MAXIMUM_PIXELS = 320.0;      // its radius on the screen at most (the scene's pixels)
const float OCCLUSION_PI = 3.14159265;
const float OCCLUSION_MAXIMUM_PIXELS = 160.0;   // the radius on the screen at most (the scene's pixels): near the eye it shrinks to this
const float OCCLUSION_FALLOFF_SHARE = 0.6;      // of the radius: the share over which what is found counts less and less

// The cosine weighted arc of a slice from the view to a horizon at this
// angle (positive toward the slice's direction), the facing's angle n
float slice_arc(float angle, float normal_angle, float normal_cosine)
{
	angle = normal_angle + clamp(angle - normal_angle, -0.5 * OCCLUSION_PI, 0.5 * OCCLUSION_PI);
	return (normal_cosine + 2.0 * angle * sin(normal_angle) - cos(2.0 * angle - normal_angle)) * 0.25;
}

// A pixel of the scene (full size) from the eye; its depth's 1: the sky
vec3 eye_point(ivec2 pixel, ivec2 size, out float depth)
{
	pixel = clamp(pixel, ivec2(0), size - 1);
	depth = texelFetch(depth_texture, pixel, 0).r;
	vec2 ndc = vec2((float(pixel.x) + 0.5) / float(size.x) * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / float(size.y) * 2.0);
	return clip_to_eye(ndc, depth * 2.0 - 1.0);
}

vec3 eye_point_only(ivec2 pixel, ivec2 size)
{
	float depth;

	return eye_point(pixel, size, depth);
}

// A four by four pattern (0 .. 15 / 16, each once in every four by four
// block: step 2 gathers four by four and averages it away), moved each
// frame over the sixteen places (the history gathers them all)
float pattern(ivec2 pixel, int turn)
{
	const int order[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
	int frame = int(occlusion_frame.x);
	ivec2 at = (pixel + ivec2(turn, turn * 2) + ivec2(frame & 3, (frame >> 2) & 3)) & 3;

	return (float(order[at.y * 4 + at.x]) + 0.5) / 16.0;
}

void main()
{
	ivec2 size = ivec2(occlusion_size.xy);
	ivec2 half_pixel = ivec2(gl_FragCoord.xy);
	// (the half size pixel's top left one of the scene's)
	ivec2 pixel = min(half_pixel * 2, size - 1);
	float depth;
	vec3 point = eye_point(pixel, size, depth);

	fragment_color = vec4(1.0, -1.0, 0.0, 0.0);
	fragment_indirect = vec4(0.0);
	// (the sky, under water: nothing)
	if (depth >= 1.0 || underwater_color.a > 0.5)
	{
		return;
	}
	float distance_eye = length(point);

	fragment_color = vec4(1.0, distance_eye, 0.0, 0.0);
	if (distance_eye >= occlusion_parameters.y || occlusion_parameters.z <= 0.0)
	{
		return;
	}
	// Its facing, from its neighbors' points (the nearer of each pair: no
	// normal across an edge to what is behind)
	vec3 right = eye_point_only(pixel + ivec2(1, 0), size) - point;
	vec3 left = point - eye_point_only(pixel - ivec2(1, 0), size);
	vec3 down = eye_point_only(pixel + ivec2(0, 1), size) - point;
	vec3 up = point - eye_point_only(pixel - ivec2(0, 1), size);
	vec3 across = dot(right, right) < dot(left, left) ? right : left;
	vec3 along = dot(down, down) < dot(up, up) ? down : up;
	float pixel_meters = distance_eye * 2.0 / (abs(view_projection[1][1]) * float(size.y));

	// (on an edge both ways, its facing is not to be had: left open)
	if (min(dot(across, across), dot(along, along)) > pixel_meters * pixel_meters * 36.0)
	{
		return;
	}
	vec3 view = -point / distance_eye;
	vec3 normal = normalize(cross(along, across));

	if (dot(normal, view) < 0.0)
	{
		normal = -normal;
	}
	float radius = occlusion_parameters.x;
	float radius_pixels = min(radius / pixel_meters, OCCLUSION_MAXIMUM_PIXELS);

	// (smaller on the screen than a few pixels: nothing to find)
	if (radius_pixels < 3.0)
	{
		return;
	}
	radius = radius_pixels * pixel_meters;
	float falloff_range = OCCLUSION_FALLOFF_SHARE * radius;
	float falloff_multiplier = -1.0 / falloff_range;
	float falloff_add = (radius - falloff_range) / falloff_range + 1.0;
	// The bounced light's radius (render.indirect_light_meters: farther than
	// the occlusion's, so a wall gets the meadow before it), at least the
	// occlusion's; its steps beyond only if it reaches farther on the screen
	float bounce_pixels = max(min(occlusion_frame.w / pixel_meters, BOUNCE_MAXIMUM_PIXELS), radius_pixels);
	float bounce_radius = bounce_pixels * pixel_meters;
	float bounce_falloff_range = OCCLUSION_FALLOFF_SHARE * bounce_radius;
	float bounce_falloff_multiplier = -1.0 / bounce_falloff_range;
	float bounce_falloff_add = (bounce_radius - bounce_falloff_range) / bounce_falloff_range + 1.0;
	float slice_turn = pattern(half_pixel, 0);
	float step_offset = pattern(half_pixel, 1);
	float ndc_depth = depth * 2.0 - 1.0;
	vec2 ndc = vec2((float(pixel.x) + 0.5) / float(size.x) * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / float(size.y) * 2.0);
	float visibility = 0.0;
	vec3 indirect = vec3(0.0);
	bool bounce = occlusion_frame.z > 0.0;
	bool bounce_far = bounce && bounce_pixels > radius_pixels + 2.0;

	for (int slice = 0; slice < OCCLUSION_SLICES; slice++)
	{
		float angle = (float(slice) + slice_turn) * OCCLUSION_PI / float(OCCLUSION_SLICES);
		// On the screen (pixels: x right, y down)
		vec2 direction = vec2(cos(angle), sin(angle));
		// ...and in the world: toward the point a little that way on the
		// screen at the same depth, square to the view (the slice's plane
		// holds the view and this)
		vec2 aside_ndc = ndc + vec2(direction.x, -direction.y) * 2.0 * 4.0 / vec2(size);
		vec3 aside = clip_to_eye(aside_ndc, ndc_depth) - point;
		vec3 slice_direction = aside - view * dot(aside, view);

		if (dot(slice_direction, slice_direction) < 1e-12)
		{
			visibility += 1.0;
			continue;
		}
		slice_direction = normalize(slice_direction);
		// The facing in the slice's plane: its angle from the view, toward
		// slice_direction positive
		vec3 axis = normalize(cross(slice_direction, view));
		vec3 projected_normal = normal - axis * dot(normal, axis);
		float projected_length = length(projected_normal);

		if (projected_length < 1e-4)
		{
			visibility += 1.0;
			continue;
		}
		float normal_cosine = clamp(dot(projected_normal, view) / projected_length, -1.0, 1.0);
		float normal_angle = sign(dot(projected_normal, slice_direction)) * acos(normal_cosine);
		// The highest horizons each way (their cosines from the view),
		// from the lowest the facing allows (the surface itself)
		float horizon_toward = cos(normal_angle + 0.5 * OCCLUSION_PI);
		float horizon_away = cos(normal_angle - 0.5 * OCCLUSION_PI);
		float lowest_toward = horizon_toward;
		float lowest_away = horizon_away;
		// The bounced light's own horizons (farther: its own radius)
		float bounce_toward = horizon_toward;
		float bounce_away = horizon_away;
		int steps = OCCLUSION_STEPS + (bounce_far ? BOUNCE_STEPS : 0);

		for (int k = 0; k < steps; k++)
		{
			// The occlusion's steps (closer together near the pixel; never
			// nearer than one half size pixel: no finding itself), then the
			// bounced light's beyond them, out to its radius
			float offset;

			if (k < OCCLUSION_STEPS)
			{
				float s = (float(k) + step_offset) / float(OCCLUSION_STEPS);

				offset = 2.0 + s * s * (radius_pixels - 2.0);
			}
			else
			{
				float s = (float(k - OCCLUSION_STEPS) + step_offset) / float(BOUNCE_STEPS);

				offset = radius_pixels + s * (bounce_pixels - radius_pixels);
			}
			ivec2 shift = ivec2(round(direction * offset));

			for (int way = 0; way < 2; way++)
			{
				ivec2 at = way == 0 ? pixel + shift : pixel - shift;

				if (at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y)
				{
					continue;
				}
				float there_depth;
				vec3 there = eye_point(at, size, there_depth) - point;

				if (there_depth >= 1.0)
				{
					continue;
				}
				float there_length = length(there);
				float there_cosine = dot(there, view) / max(there_length, 1e-5);
				float lowest = way == 0 ? lowest_toward : lowest_away;

				// The occlusion, within its radius
				if (k < OCCLUSION_STEPS)
				{
					float weight = clamp(there_length * falloff_multiplier + falloff_add, 0.0, 1.0);
					float raised = mix(lowest, there_cosine, weight);

					if (way == 0)
					{
						horizon_toward = max(horizon_toward, raised);
					}
					else
					{
						horizon_away = max(horizon_away, raised);
					}
				}
				// The bounced light, within its own: the arc a point newly
				// hides is where its light comes from
				if (bounce)
				{
					float weight = clamp(there_length * bounce_falloff_multiplier + bounce_falloff_add, 0.0, 1.0);
					float raised = mix(lowest, there_cosine, weight);
					float horizon = way == 0 ? bounce_toward : bounce_away;

					if (raised <= horizon)
					{
						continue;
					}
					float sign_way = way == 0 ? 1.0 : -1.0;
					float before = slice_arc(sign_way * acos(clamp(horizon, -1.0, 1.0)), normal_angle, normal_cosine);
					float after = slice_arc(sign_way * acos(clamp(raised, -1.0, 1.0)), normal_angle, normal_cosine);
					vec3 light = max(texelFetch(scene_texture, at, 0).rgb, vec3(0.0));

					// (beyond the occlusion's radius the scene's shaders still
					// give that arc the light from round about they take for
					// it, the sky's above, the ground's below (prop_body.glsl):
					// only what the found point sends more, or less, counts
					// there, its tint over a plain ground's)
					if (k >= OCCLUSION_STEPS)
					{
						vec3 toward = there / max(there_length, 1e-5);

						light -= mix(ground_bounce_color(1.0), sky_color, toward.y * 0.5 + 0.5) * ambient_intensity;
					}
					indirect += light * max(before - after, 0.0) * projected_length;
					if (way == 0)
					{
						bounce_toward = raised;
					}
					else
					{
						bounce_away = raised;
					}
				}
			}
		}
		// The horizons as angles (toward positive), no farther than square
		// to the facing; the cosine weighted arc between them
		float angle_toward = acos(clamp(horizon_toward, -1.0, 1.0));
		float angle_away = -acos(clamp(horizon_away, -1.0, 1.0));

		angle_toward = normal_angle + clamp(angle_toward - normal_angle, -0.5 * OCCLUSION_PI, 0.5 * OCCLUSION_PI);
		angle_away = normal_angle + clamp(angle_away - normal_angle, -0.5 * OCCLUSION_PI, 0.5 * OCCLUSION_PI);
		float arc_toward = (normal_cosine + 2.0 * angle_toward * sin(normal_angle) - cos(2.0 * angle_toward - normal_angle)) * 0.25;
		float arc_away = (normal_cosine + 2.0 * angle_away * sin(normal_angle) - cos(2.0 * angle_away - normal_angle)) * 0.25;

		visibility += projected_length * (arc_toward + arc_away);
	}
	visibility = clamp(visibility / float(OCCLUSION_SLICES), 0.0, 1.0);
	fragment_color = vec4(visibility, distance_eye, 0.0, 0.0);
	// (held under a bright limit: a glint or the sun in a window found by
	// one sample is no lamp)
	fragment_indirect = vec4(clamp(indirect / float(OCCLUSION_SLICES), vec3(-4.0), vec3(16.0)), 0.0);
}
