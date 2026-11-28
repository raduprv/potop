#version 450
// The picture bent by explosions' shock waves: round each, a ring growing
// outwards pushes what is seen through it out and pulls it in just
// behind, as hot, squeezed air would

layout(location = 0) in vec2 in_coordinates;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D scene;

layout(set = 3, binding = 0) uniform shock_block
{
	vec4 rings[4];                     // in pixels: the middle (x, y), the radius, how far it bends
	vec4 info;                         // x: how many rings, y: the ring's width in pixels, z, w: 1 / the picture's size
};

void main()
{
	vec2 pixel = gl_FragCoord.xy;
	vec2 bend = vec2(0.0);

	for (int i = 0; i < int(info.x); i++)
	{
		vec2 from_middle = pixel - rings[i].xy;
		float distance = length(from_middle);
		float width = max(info.y, rings[i].z * 0.2);
		float across = (distance - rings[i].z) / width;
		// (out ahead of the ring's middle, in behind it, none far off)
		float wave = across * exp(-across * across * 2.0);

		bend += from_middle / max(distance, 1.0) * wave * rings[i].w;
	}
	out_color = texture(scene, (pixel - bend) * info.zw);
}
