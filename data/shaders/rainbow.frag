#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "scene.glsl"

// The rainbow (see src/rain.h). Sunlight comes back from raindrops most
// strongly about 42 degrees from the point opposite the sun, a little
// farther for red than for violet: the primary bow, red outside. Light
// reflected twice in the drops makes the secondary, fainter, at about 51
// degrees, its colors the other way round. Inside the primary the drops
// send back light of all colors (the sky is brighter there); between the
// two, none (Alexander's dark band). It hangs in the rain some way off:
// its depth is that of a point that far along the pixel's ray, so the
// depth test keeps it off whatever is nearer. Premultiplied: the alpha
// darkens the band.

layout(std140, set = UNIFORM_SET, binding = 1) uniform rainbow_block
{
	vec4 rainbow;                   // x: how much it shows
};

layout(location = 0) in vec3 view_ray;      // the near plane's point from the eye (its direction)

layout(location = 0) out vec4 fragment_color;

// How far off it hangs (meters)
const float RAINBOW_DISTANCE = 450.0;

// Its colors from violet (0) to red (1): the light's wavelength (400 ..
// 700 nm) as a color, each as bright as the eye finds it (yellow and
// orange the brightest, violet the dimmest, red between)
vec3 spectrum(float t)
{
	float wavelength = 400.0 + 300.0 * t;
	vec3 color;

	if (wavelength < 440.0)
	{
		color = vec3(0.35 * (440.0 - wavelength) / 40.0, 0.0, 1.0);
	}
	else if (wavelength < 490.0)
	{
		color = vec3(0.0, (wavelength - 440.0) / 50.0, 1.0);
	}
	else if (wavelength < 510.0)
	{
		color = vec3(0.0, 1.0, (510.0 - wavelength) / 20.0);
	}
	else if (wavelength < 580.0)
	{
		color = vec3((wavelength - 510.0) / 70.0, 1.0, 0.0);
	}
	else if (wavelength < 645.0)
	{
		color = vec3(1.0, (645.0 - wavelength) / 65.0, 0.0);
	}
	else
	{
		color = vec3(1.0, 0.0, 0.0);
	}
	// How bright the eye finds it (a broad hump round yellow orange)
	return color * (0.3 + 0.9 * exp(-pow((wavelength - 590.0) / 65.0, 2.0)));
}

// A band's colors between two angles (degrees), violet at the first
vec3 bow(float angle, float violet, float red)
{
	float t = (angle - violet) / (red - violet);

	if (t < -0.1 || t > 1.1)
	{
		return vec3(0.0);
	}
	return spectrum(clamp(t, 0.0, 1.0)) * smoothstep(-0.1, 0.12, t) * (1.0 - smoothstep(0.88, 1.1, t));
}

void main()
{
	vec3 direction = normalize(view_ray);
	vec4 clip = view_projection_relative * vec4(direction * RAINBOW_DISTANCE, 1.0);

	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
	// Only the sun makes one (the scene's light is the moon at night)
	if (moon.w > 0.5 || rainbow.x <= 0.0)
	{
		discard;
	}
	float angle = degrees(acos(clamp(dot(direction, -sun_direction), -1.0, 1.0)));

	if (angle > 56.0)
	{
		discard;
	}
	vec3 colors = bow(angle, 40.6, 42.4) + bow(angle, 53.4, 50.4) * 0.38;
	float inside = smoothstep(25.0, 40.5, angle) * (1.0 - smoothstep(40.5, 41.5, angle)) * 0.1;
	float dark = smoothstep(42.2, 43.5, angle) * (1.0 - smoothstep(49.2, 50.5, angle)) * 0.12;
	vec3 light = sun_color * 0.22;
	float amount = rainbow.x;

	fragment_color = vec4((colors + inside) * light * amount, clamp(dark * amount, 0.0, 0.5));
}
