// The volumetric clouds' layer and dome (included by clouds_dome.comp and
// clouds_composite.frag, after scene.glsl). The layer is a shell around a
// small planet whose top touches the ground under the eye
// (CLOUD_PLANET_RADIUS, common.glsl): towards the horizon the clouds come
// down to it, as they do in the real sky, instead of running off flat into
// the distance.
//
// The dome holds the clouds seen from the eye in every direction, in an
// octahedral map: of the whole sphere, or, with the eye under the clouds,
// of the sky above the horizon only (twice the texels for it).

layout(std140, set = UNIFORM_SET, binding = 1) uniform volume_block
{
	float base_height;      // meters above the ground level 0
	float thickness;        // meters
	float density;          // extinction per meter at full density
	float steps;            // fine steps (inside clouds) over 700 m of cloud
	float shape_scale;      // 1 / meters of the shape noise's tile
	float detail_scale;     // 1 / meters of the detail noise's tile
	float maximum_distance; // meters
	float frame;            // changes every frame: jitter
	vec4 vertical_factors;  // x: 1 / meters of the shape noise's tile upwards (whatever the clouds' size), y: the detail's
	vec4 dome;              // x: texels a side; y: the refreshed texels' spacing (1 or 4); z: this frame's Bayer rank; w: 1: coarse (spacing 2, a march for four texels)
	vec4 refresh;           // x: the weight of a new march against what the texel held; y: how many times the texels have been marched (jitter sequence)
	vec4 look;              // y: how dark the bases get (0..1), z: 1: the dome holds the upper half only (see dome_coordinates), w: the clouds' size
	vec4 flock;             // the flocks (clouds.h): x: their layer's bottom, y: its thickness (meters), z: the size of each (meters), w: how many (0: none at all)
	vec4 flock_wind;        // xy: the wind's direction (their rows), z: how dense (of FLOCK_EXTINCTION), w: how much they shade themselves (0: white)
	vec4 planet;            // x: how far under the eye's ground the planet's surface is (meters; see planet_center), y: the light from the sky, of the day's (darker at night)
};

// The planet is let down under the eye by planet.x: the heights are still
// counted from the ground under the eye (height 0), over a sphere of
// CLOUD_PLANET_RADIUS + planet.x, but what stops the rays is the planet's
// own surface, that much lower. So the rays a little under the horizon
// still reach the clouds, and the sky has clouds down to there (a few
// degrees) instead of stopping at the horizon's line
vec3 planet_center()
{
	return vec3(eye_position.x, -CLOUD_PLANET_RADIUS - planet.x, eye_position.z);
}

// Height of a point over the curved ground (0 under the eye)
float curved_height(vec3 point)
{
	return length(point - planet_center()) - CLOUD_PLANET_RADIUS - planet.x;
}

// Where a ray meets a sphere around the planet's center: false if not
bool sphere_hits(vec3 origin, vec3 direction, float radius, out float near_distance, out float far_distance)
{
	vec3 offset = origin - planet_center();
	float b = dot(offset, direction);
	float c = dot(offset, offset) - radius * radius;
	float discriminant = b * b - c;

	if (discriminant < 0.0)
	{
		near_distance = 0.0;
		far_distance = 0.0;
		return false;
	}
	discriminant = sqrt(discriminant);
	near_distance = -b - discriminant;
	far_distance = -b + discriminant;
	return true;
}

// The part of the ray between two heights over the curved ground (false:
// none), up to limit and to the curved ground (beyond the map's edge nothing
// else stops the ray)
bool shell_interval(vec3 origin, vec3 direction, float limit, float bottom, float top, out float enter, out float leave)
{
	float inner_radius = CLOUD_PLANET_RADIUS + planet.x + bottom;
	float outer_radius = CLOUD_PLANET_RADIUS + planet.x + top;
	float eye_radius = length(origin - planet_center());
	float inner_near;
	float inner_far;
	float outer_near;
	float outer_far;
	bool hits_inner = sphere_hits(origin, direction, inner_radius, inner_near, inner_far);
	bool hits_outer = sphere_hits(origin, direction, outer_radius, outer_near, outer_far);

	enter = 0.0;
	leave = 0.0;
	if (eye_radius < inner_radius)
	{
		// Under the layer (always hits both: we are inside them)
		enter = inner_far;
		leave = outer_far;
	}
	else if (eye_radius <= outer_radius)
	{
		enter = 0.0;
		leave = hits_inner && inner_near > 0.0 ? inner_near : outer_far;
	}
	else
	{
		if (!hits_outer || outer_far <= 0.0)
		{
			return false;
		}
		enter = max(outer_near, 0.0);
		leave = hits_inner && inner_near > 0.0 ? inner_near : outer_far;
	}
	{
		float ground_near;
		float ground_far;

		if (sphere_hits(origin, direction, CLOUD_PLANET_RADIUS, ground_near, ground_far) && ground_near > 0.0)
		{
			leave = min(leave, ground_near);
		}
	}
	leave = min(leave, limit);
	return leave > enter;
}

// The big clouds' layer
bool layer_interval(vec3 origin, vec3 direction, float limit, out float enter, out float leave)
{
	return shell_interval(origin, direction, limit, base_height, base_height + thickness, enter, leave);
}

// The flocks' layer (false too when there are none)
bool flock_interval(vec3 origin, vec3 direction, float limit, out float enter, out float leave)
{
	enter = 0.0;
	leave = 0.0;
	return flock.w > 0.0 && shell_interval(origin, direction, limit, flock.x, flock.x + flock.y, enter, leave);
}

#include "dome_mapping.glsl"

// A direction to its place in the dome (0..1), and back
vec2 dome_coordinates(vec3 direction)
{
	return look.z > 0.5 ? half_coordinates(direction) : sphere_coordinates(direction);
}

vec3 dome_direction(vec2 coordinates)
{
	return look.z > 0.5 ? half_direction(coordinates) : sphere_direction(coordinates);
}
