#version 450
// An explosion's smoke: a soft, see through ball, lit by the sky and a
// little by the sun, thinner at its rim

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
	vec3 ambient = mix(ground_color.rgb, sky_color.rgb, 0.5 + 0.5 * normal.y);
	vec3 color = in_color.rgb * (ambient + sun_color.rgb * 0.35 * max(dot(normal, sun_direction.xyz), 0.0));

	color = color / (1.0 + color * 0.25);
	out_color = vec4(pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2)), in_color.a * pow(through, 1.3));
}
