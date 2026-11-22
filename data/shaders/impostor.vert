#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"
#include "impostor.glsl"
#define SKY_VERTEX_BINDING 0
#include "sky_vertex.glsl"

// An impostor (see src/impostors.h): a square facing the eye, as big as
// the object's bounding sphere, painted from the three pictures of the
// grid taken from nearest where the eye is (blended by how near). Each
// picture's coordinates of a point of the square come from the eye's ray
// through it, met with the picture's own plane: the pictures line up
// where they overlap instead of sliding over each other.
//
// Drawn instanced from the objects' culling (the same instances as the
// models: placement, rotation, fade), four corners each.

layout(location = 0) in vec4 instance_placement;         // x, y, z of the origin, size
layout(location = 1) in vec4 instance_rotation;          // quaternion (x, y, z, w)
layout(location = 2) in float instance_fade;

layout(std140, set = UNIFORM_SET, binding = 1) uniform impostor_block
{
	vec4 sphere;                   // the model's bounding sphere: center, radius (model units)
	vec4 look;                     // x: how leafy (0..1: lit like leaves, the light wrapping round); y: a plant's (impostor.glsl); z: pictures across its atlas
};

layout(location = 0) out vec3 world_position;            // on the square
layout(location = 1) out vec2 picture_point_0;           // the point on each picture (-1..1)
layout(location = 2) out vec2 picture_point_1;
layout(location = 3) out vec2 picture_point_2;
layout(location = 4) flat out ivec4 frames_01;           // the pictures: the first two
layout(location = 5) flat out ivec2 frame_2;             // and the third
layout(location = 6) flat out vec3 weights;
layout(location = 7) flat out vec4 rotation;
layout(location = 8) flat out float fade;
layout(location = 9) flat out vec4 toward_eye;           // xyz: from the object towards the eye (world); w: the sphere's radius (meters)
layout(location = 10) flat out float leafy;
layout(location = 12) out float haze;                    // how much the haze hides it (scene.glsl's haze_amount, per vertex: it changes slowly over a blade, a card, a picture)
layout(location = 11) flat out vec4 sky_light;          // at its middle: the light sent back (rgb, onto a face up), the sky seen (a)
layout(location = 13) flat out vec4 picture_turn;        // xy: cos, sin of the turn about up from the pictures' side to the eye's (a plant's; 1, 0 else); z: pictures across the atlas

vec4 conjugate(vec4 quaternion)
{
	return vec4(-quaternion.xyz, quaternion.w);
}

// A point of the square (model axes, in sphere radii from its center) on
// a picture: where the ray from the eye through it meets the picture's plane
vec2 on_picture_from(vec3 direction, vec3 eye, vec3 point)
{
	vec3 right;
	vec3 up;
	vec3 along = eye - point;
	float across = dot(along, direction);
	vec3 met = point;

	impostor_basis(direction, right, up);
	if (abs(across) > 1e-5)
	{
		met = point - along * (dot(point, direction) / across);
	}
	return vec2(dot(met, right), dot(met, up));
}

vec2 on_picture(ivec2 frame, vec3 eye, vec3 point)
{
	return on_picture_from(impostor_frame_direction(frame), eye, point);
}

void main()
{
	vec2 corner = vec2((gl_VertexIndex & 1) != 0 ? 1.0 : -1.0, (gl_VertexIndex & 2) != 0 ? 1.0 : -1.0);
	float size = instance_placement.w;
	vec3 center = instance_placement.xyz + impostor_rotate(instance_rotation, sphere.xyz) * size;
	float radius = sphere.w * size;
	vec3 eye_direction = normalize(eye_position - center);
	vec3 right;
	vec3 up;

	impostor_basis(eye_direction, right, up);
	sky_light = sky_light_at(center, center.y - instance_placement.y, vec3(0.0, 1.0, 0.0));
	// A little larger than the sphere: seen from near, its outline is
	// wider than its radius
	world_position = center + (corner.x * right + corner.y * up) * radius * 1.1;

	// The eye and the corner in the model's axes, in sphere radii
	{
		vec4 back = conjugate(instance_rotation);
		vec3 eye = impostor_rotate(back, eye_position - center) / radius;
		vec3 point = impostor_rotate(back, world_position - center) / radius;

		picture_turn = vec4(1.0, 0.0, look.z, 0.0);
		// A plant's: the two pictures nearest the eye's height, turned to
		// its side
		if (look.y > 0.5)
		{
			vec3 toward = normalize(eye);
			float azimuth = atan(toward.z, toward.x);
			float height = clamp(asin(clamp(toward.y, -1.0, 1.0)), 0.0, radians(IMPOSTOR_PLANT_HIGHEST_DEGREES));
			float along = height / radians(IMPOSTOR_PLANT_HIGHEST_DEGREES) * float(IMPOSTOR_PLANT_PICTURES - 1);
			int lower = min(int(floor(along)), IMPOSTOR_PLANT_PICTURES - 2);
			float upper_share = clamp(along - float(lower), 0.0, 1.0);

			frames_01 = ivec4(impostor_plant_frame(lower), impostor_plant_frame(lower + 1));
			frame_2 = impostor_plant_frame(lower);
			weights = vec3(1.0 - upper_share, upper_share, 0.0);
			picture_point_0 = on_picture_from(impostor_plant_direction(lower, azimuth), eye, point);
			picture_point_1 = on_picture_from(impostor_plant_direction(lower + 1, azimuth), eye, point);
			picture_point_2 = picture_point_0;
			picture_turn.xy = vec2(cos(azimuth), sin(azimuth));
		}
		else
		{
		vec2 grid = (impostor_hemi_encode(normalize(eye)) * 0.5 + 0.5) * float(IMPOSTOR_GRID - 1);
		ivec2 base = clamp(ivec2(floor(grid)), ivec2(0), ivec2(IMPOSTOR_GRID - 2));
		vec2 within = clamp(grid - vec2(base), 0.0, 1.0);
		ivec2 frame_a;
		ivec2 frame_b;
		ivec2 frame_c;

		// The grid's square cut in two triangles: the three corners of the
		// one the eye is in, weighted by where in it
		if (within.x + within.y < 1.0)
		{
			frame_a = base;
			frame_b = base + ivec2(1, 0);
			frame_c = base + ivec2(0, 1);
			weights = vec3(1.0 - within.x - within.y, within.x, within.y);
		}
		else
		{
			frame_a = base + ivec2(1, 1);
			frame_b = base + ivec2(0, 1);
			frame_c = base + ivec2(1, 0);
			weights = vec3(within.x + within.y - 1.0, 1.0 - within.x, 1.0 - within.y);
		}
		frames_01 = ivec4(frame_a, frame_b);
		frame_2 = frame_c;
		picture_point_0 = on_picture(frame_a, eye, point);
		picture_point_1 = on_picture(frame_b, eye, point);
		picture_point_2 = on_picture(frame_c, eye, point);
		}
	}
	rotation = instance_rotation;
	fade = instance_fade;
	toward_eye = vec4(eye_direction, radius);
	leafy = look.x;
	haze = clamp(haze_amount(world_position), 0.0, 1.0);
	// Drawn at the sphere's near side (the same square on the screen,
	// moved towards the eye along the rays: the attributes stay those of
	// the square through the middle): impostor.frag writes its depth only
	// farther than that (depth_greater), so the GPU can still turn away
	// what is hidden before shading it (a far wood: impostor behind
	// impostor). A little nearer still: the depth the pictures give is
	// taken along the way to the middle, a corner's off it by up to about
	// radius squared over the distance
	{
		float eye_distance = max(length(eye_position - center), 1e-3);
		float near_distance = max(eye_distance - radius - 0.8 * radius * radius / eye_distance - 0.02, 0.05);

		gl_Position = world_to_clip(eye_position + (world_position - eye_position) * (near_distance / eye_distance));
	}
}
