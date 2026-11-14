#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// A splash (rain_splash.vert). On water: a thin ring that spreads and
// fades, a bright dot at first where the drop hit. On the ground: a few
// drops thrown up and out, falling back (each on its own arc, gravity
// pulling it down). Premultiplied.

layout(location = 0) in vec2 local;
layout(location = 1) in float age;
layout(location = 2) in float splash_alpha;
layout(location = 3) in float on_water;
layout(location = 4) in float seed;

layout(location = 0) out vec4 fragment_color;

// rain_splash.vert's crown quad (meters) and a splash's life (seconds)
const float CROWN_HALF_WIDTH = 0.08;
const float CROWN_HEIGHT = 0.08;
const float LIFE = 0.3;
const int CROWN_DROPS = 5;

void main()
{
	float alpha;

	if (on_water > 0.5)
	{
		float radius = length(local);
		float ring = exp(-pow((radius - 0.8) / 0.12, 2.0));
		float hit = exp(-pow(radius / 0.25, 2.0)) * (1.0 - smoothstep(0.0, 0.3, age));

		alpha = (ring + hit) * pow(1.0 - age, 2.0) * splash_alpha * 0.28;
	}
	else
	{
		float seconds = age * LIFE;
		// Where this fragment is, in meters from the foot of the splash
		vec2 meters = vec2(local.x * CROWN_HALF_WIDTH, local.y * CROWN_HEIGHT);
		float drops = 0.0;

		for (int k = 0; k < CROWN_DROPS; k++)
		{
			float spread = (float(k) + 0.5) / float(CROWN_DROPS) * 2.0 - 1.0;
			float jitter = fract(seed * 13.7 + float(k) * 0.618);
			vec2 velocity = vec2(spread * 0.22 + (jitter - 0.5) * 0.08, 0.55 + 0.35 * jitter);
			vec2 drop = velocity * seconds - vec2(0.0, 4.9 * seconds * seconds);

			if (drop.y > 0.0)
			{
				vec2 offset = (meters - drop) / 0.007;

				drops += exp(-dot(offset, offset));
			}
		}
		alpha = min(drops, 1.0) * (1.0 - age) * splash_alpha * 0.9;
	}
	if (alpha <= 0.002)
	{
		discard;
	}
	vec3 light = sky_color * ambient_intensity * 1.6 + sun_color * 0.2;

	fragment_color = vec4(light * alpha, alpha * 0.4);
}
