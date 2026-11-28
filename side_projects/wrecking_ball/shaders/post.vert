#version 450
// A triangle over the whole screen (no vertex buffer)

layout(location = 0) out vec2 out_coordinates;

void main()
{
	vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));

	out_coordinates = corner;
	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
