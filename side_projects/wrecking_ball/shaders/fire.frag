#version 450
// An explosion's fire: a glowing ball added onto what is behind it, soft
// at its rim (the less of it the eye looks through). The instance's color
// is its light (linear, may be over 1), its alpha how strongly it shows.

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_world_normal;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec4 out_color;

layout(set = 3, binding = 0) uniform light_block
{
	vec4 sun_direction;
	vec4 sun_color;
	vec4 sky_color;
	vec4 ground_color;
	vec4 fog_color;
	vec4 eye;
};

void main()
{
	vec3 normal = normalize(in_world_normal);
	vec3 to_eye = normalize(eye.xyz - in_world_position);
	float through = abs(dot(normal, to_eye));
	vec3 light = in_color.rgb * in_color.a * pow(through, 1.8);

	// (added in display terms: a soft knee, then to sRGB)
	light = 1.0 - exp(-light);
	out_color = vec4(pow(light, vec3(1.0 / 2.2)), 1.0);
}
