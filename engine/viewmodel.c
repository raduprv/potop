// The weapon in hand. See viewmodel.h.

#include "viewmodel.h"

#include "gpu.h"
#include "math3d.h"
#include "terrain_renderer.h"

// Its own field of view (degrees, vertical) and near and far
static const float VIEWMODEL_FIELD_OF_VIEW = 52.0f;
static const float VIEWMODEL_NEAR = 0.01f;
static const float VIEWMODEL_FAR = 4.0f;
// The share of the depth range it is drawn into (the nearest)
static const float VIEWMODEL_DEPTH_SQUEEZE = 0.002f;
// Where it is held (camera space: right, up, back) and the muzzle, in its own
static const float HELD_POSITION[3] = { 0.16f, -0.17f, -0.36f };
static const float MUZZLE[3] = { 0.0f, 0.02f, -0.66f };
// Aimed: where it is held so its sights are at the eye (the front sight's
// top, or the scope's middle, that high over the grip)
static const float AIMED_DISTANCE = -0.27f;
static const float SIGHTS_HEIGHT = 0.071f;
static const float SCOPE_HEIGHT = 0.104f;
// Through the scope: its round view (units of half the screen's height)
static const float SCOPE_VIEW_RADIUS = 0.93f;

typedef struct
{
	float position[3];
	float normal[3];
	float color[3];
} viewmodel_vertex_t;

typedef struct
{
	viewmodel_vertex_t *vertices;
	uint32_t count;
	uint32_t capacity;
} mesh_builder_t;

typedef struct
{
	float model_view[16];
	float projection[16];
	float parameters[4];
} vertex_uniforms_t;

typedef struct
{
	float sun_direction[4];
	float sun_light[4];
	float sky_light[4];
	float ground_light[4];
	float up[4];
	float flash[4];
	float flash_position[4];
	float carried[4];              // the carried light (rgb)
	float carried_position[4];     // in the camera's space
} fragment_uniforms_t;

static void add_vertex(mesh_builder_t *mesh, vec3_t position, vec3_t normal, const float color[3])
{
	viewmodel_vertex_t *vertex;

	if (mesh->count == mesh->capacity)
	{
		mesh->capacity = mesh->capacity > 0 ? mesh->capacity * 2 : 256;
		mesh->vertices = SDL_realloc(mesh->vertices, sizeof(viewmodel_vertex_t) * mesh->capacity);
	}
	vertex = &mesh->vertices[mesh->count++];
	vertex->position[0] = position.x;
	vertex->position[1] = position.y;
	vertex->position[2] = position.z;
	vertex->normal[0] = normal.x;
	vertex->normal[1] = normal.y;
	vertex->normal[2] = normal.z;
	SDL_memcpy(vertex->color, color, sizeof(vertex->color));
}

// A point turned by angle (radians) about X (nose up positive) then moved
static vec3_t place(vec3_t point, float angle, vec3_t center)
{
	float c = SDL_cosf(angle);
	float s = SDL_sinf(angle);

	return vec3_make(center.x + point.x, center.y + point.y * c - point.z * s, center.z + point.y * s + point.z * c);
}

static void add_quad(mesh_builder_t *mesh, vec3_t a, vec3_t b, vec3_t c, vec3_t d, vec3_t normal, const float color[3])
{
	add_vertex(mesh, a, normal, color);
	add_vertex(mesh, b, normal, color);
	add_vertex(mesh, c, normal, color);
	add_vertex(mesh, a, normal, color);
	add_vertex(mesh, c, normal, color);
	add_vertex(mesh, d, normal, color);
}

// A box: center, half sizes, turned about X by pitch (radians)
static void add_box(mesh_builder_t *mesh, vec3_t center, vec3_t half, float pitch, const float color[3])
{
	vec3_t corners[8];
	vec3_t zero = vec3_make(0.0f, 0.0f, 0.0f);

	for (int i = 0; i < 8; i++)
	{
		vec3_t local = vec3_make((i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z);

		corners[i] = place(local, pitch, center);
	}
	add_quad(mesh, corners[1], corners[3], corners[7], corners[5], place(vec3_make(1.0f, 0.0f, 0.0f), pitch, zero), color);
	add_quad(mesh, corners[0], corners[4], corners[6], corners[2], place(vec3_make(-1.0f, 0.0f, 0.0f), pitch, zero), color);
	add_quad(mesh, corners[2], corners[6], corners[7], corners[3], place(vec3_make(0.0f, 1.0f, 0.0f), pitch, zero), color);
	add_quad(mesh, corners[0], corners[1], corners[5], corners[4], place(vec3_make(0.0f, -1.0f, 0.0f), pitch, zero), color);
	add_quad(mesh, corners[4], corners[5], corners[7], corners[6], place(vec3_make(0.0f, 0.0f, 1.0f), pitch, zero), color);
	add_quad(mesh, corners[0], corners[2], corners[3], corners[1], place(vec3_make(0.0f, 0.0f, -1.0f), pitch, zero), color);
}

// A prism along Z (a barrel): its middle (x, y), from z to z, radius, sides
static void add_prism(mesh_builder_t *mesh, float x, float y, float z_near, float z_far, float radius, int sides, const float color[3])
{
	for (int i = 0; i < sides; i++)
	{
		float a0 = 2.0f * SDL_PI_F * (float)i / (float)sides;
		float a1 = 2.0f * SDL_PI_F * (float)(i + 1) / (float)sides;
		float am = 0.5f * (a0 + a1);
		vec3_t p0 = vec3_make(x + SDL_cosf(a0) * radius, y + SDL_sinf(a0) * radius, z_near);
		vec3_t p1 = vec3_make(x + SDL_cosf(a1) * radius, y + SDL_sinf(a1) * radius, z_near);
		vec3_t p2 = vec3_make(p1.x, p1.y, z_far);
		vec3_t p3 = vec3_make(p0.x, p0.y, z_far);

		add_quad(mesh, p0, p1, p2, p3, vec3_make(SDL_cosf(am), SDL_sinf(am), 0.0f), color);
		// (the far end's cap)
		add_vertex(mesh, vec3_make(x, y, z_far), vec3_make(0.0f, 0.0f, -1.0f), color);
		add_vertex(mesh, p3, vec3_make(0.0f, 0.0f, -1.0f), color);
		add_vertex(mesh, p2, vec3_make(0.0f, 0.0f, -1.0f), color);
	}
}

// A rifle of boxes (an AK's lines: dark steel, a wooden stock and
// handguard, a curved orange magazine), the barrel along -Z, the grip
// at the origin
static void build_rifle(mesh_builder_t *mesh)
{
	static const float steel[3] = { 0.07f, 0.07f, 0.075f };
	static const float dark_steel[3] = { 0.04f, 0.04f, 0.045f };
	static const float wood[3] = { 0.30f, 0.13f, 0.05f };
	static const float bakelite[3] = { 0.42f, 0.16f, 0.05f };
	const float degrees = SDL_PI_F / 180.0f;

	// The receiver and its cover
	add_box(mesh, vec3_make(0.0f, 0.02f, 0.03f), vec3_make(0.024f, 0.034f, 0.17f), 0.0f, steel);
	add_box(mesh, vec3_make(0.0f, 0.058f, 0.04f), vec3_make(0.021f, 0.008f, 0.15f), 0.0f, dark_steel);
	// The handguard, under and over
	add_box(mesh, vec3_make(0.0f, 0.012f, -0.24f), vec3_make(0.027f, 0.028f, 0.10f), 0.0f, wood);
	add_box(mesh, vec3_make(0.0f, 0.052f, -0.22f), vec3_make(0.015f, 0.011f, 0.11f), 0.0f, wood);
	// The barrel, the front sight, the muzzle brake
	add_prism(mesh, 0.0f, 0.02f, -0.34f, -0.62f, 0.010f, 8, dark_steel);
	add_prism(mesh, 0.0f, 0.052f, -0.33f, -0.56f, 0.006f, 6, steel);
	add_box(mesh, vec3_make(0.0f, 0.048f, -0.565f), vec3_make(0.008f, 0.022f, 0.008f), 0.0f, steel);
	add_prism(mesh, 0.0f, 0.02f, -0.60f, -0.66f, 0.014f, 8, steel);
	// The magazine (curved: three pieces, more forward the lower)
	add_box(mesh, vec3_make(0.0f, -0.04f, -0.07f), vec3_make(0.017f, 0.035f, 0.034f), 10.0f * degrees, bakelite);
	add_box(mesh, vec3_make(0.0f, -0.10f, -0.085f), vec3_make(0.017f, 0.035f, 0.034f), 22.0f * degrees, bakelite);
	add_box(mesh, vec3_make(0.0f, -0.155f, -0.11f), vec3_make(0.017f, 0.032f, 0.033f), 34.0f * degrees, bakelite);
	// The trigger guard, the pistol grip
	add_box(mesh, vec3_make(0.0f, -0.03f, 0.04f), vec3_make(0.006f, 0.004f, 0.035f), 0.0f, dark_steel);
	add_box(mesh, vec3_make(0.0f, -0.07f, 0.10f), vec3_make(0.016f, 0.05f, 0.02f), -18.0f * degrees, wood);
	// The stock and its butt plate
	add_box(mesh, vec3_make(0.0f, -0.005f, 0.32f), vec3_make(0.02f, 0.03f, 0.13f), -9.0f * degrees, wood);
	add_box(mesh, vec3_make(0.0f, -0.03f, 0.46f), vec3_make(0.022f, 0.055f, 0.02f), -9.0f * degrees, wood);
	add_box(mesh, vec3_make(0.0f, -0.032f, 0.483f), vec3_make(0.023f, 0.057f, 0.004f), -9.0f * degrees, dark_steel);
}

// The rifle with a scope over its receiver (on two rings), a longer barrel
static void build_scoped_rifle(mesh_builder_t *mesh)
{
	static const float scope_black[3] = { 0.025f, 0.025f, 0.03f };
	static const float lens[3] = { 0.05f, 0.08f, 0.12f };

	build_rifle(mesh);
	add_prism(mesh, 0.0f, 0.02f, -0.62f, -0.80f, 0.009f, 8, scope_black);
	// The rings and the scope's tube, wider at the front (the objective)
	add_box(mesh, vec3_make(0.0f, 0.078f, 0.06f), vec3_make(0.012f, 0.014f, 0.008f), 0.0f, scope_black);
	add_box(mesh, vec3_make(0.0f, 0.078f, -0.06f), vec3_make(0.012f, 0.014f, 0.008f), 0.0f, scope_black);
	add_prism(mesh, 0.0f, SCOPE_HEIGHT, 0.12f, -0.10f, 0.016f, 12, scope_black);
	add_prism(mesh, 0.0f, SCOPE_HEIGHT, -0.10f, -0.13f, 0.020f, 12, scope_black);
	add_prism(mesh, 0.0f, SCOPE_HEIGHT, -0.13f, -0.20f, 0.024f, 12, scope_black);
	add_prism(mesh, 0.0f, SCOPE_HEIGHT, -0.2f, -0.201f, 0.021f, 12, lens);
	// The turrets
	add_box(mesh, vec3_make(0.0f, SCOPE_HEIGHT + 0.022f, 0.0f), vec3_make(0.009f, 0.008f, 0.009f), 0.0f, scope_black);
	add_box(mesh, vec3_make(0.022f, SCOPE_HEIGHT, 0.0f), vec3_make(0.008f, 0.009f, 0.009f), 0.0f, scope_black);
}

// Through the scope: everything black but its round view, and the
// reticle: thin lines across it, thick posts from the edge in
static void build_scope_view(mesh_builder_t *mesh)
{
	static const float black[3] = { 0.0f, 0.0f, 0.0f };
	const int sides = 96;
	const float outer = 6.0f;
	vec3_t normal = vec3_make(0.0f, 0.0f, 1.0f);

	for (int i = 0; i < sides; i++)
	{
		float a0 = 2.0f * SDL_PI_F * (float)i / (float)sides;
		float a1 = 2.0f * SDL_PI_F * (float)(i + 1) / (float)sides;

		add_quad(mesh, vec3_make(SDL_cosf(a0) * SCOPE_VIEW_RADIUS, SDL_sinf(a0) * SCOPE_VIEW_RADIUS, 0.0f),
			vec3_make(SDL_cosf(a0) * outer, SDL_sinf(a0) * outer, 0.0f), vec3_make(SDL_cosf(a1) * outer, SDL_sinf(a1) * outer, 0.0f),
			vec3_make(SDL_cosf(a1) * SCOPE_VIEW_RADIUS, SDL_sinf(a1) * SCOPE_VIEW_RADIUS, 0.0f), normal, black);
	}
	for (int i = 0; i < 4; i++)
	{
		float dx = i == 0 ? 1.0f : (i == 1 ? -1.0f : 0.0f);
		float dy = i == 2 ? 1.0f : (i == 3 ? -1.0f : 0.0f);
		// (the thin line from the middle, then the post)
		float pieces[2][3] = { { 0.0f, 0.35f, 0.0018f }, { 0.35f, SCOPE_VIEW_RADIUS + 0.01f, 0.009f } };

		for (int p = 0; p < 2; p++)
		{
			float from = pieces[p][0];
			float to = pieces[p][1];
			float width = pieces[p][2];
			float side_x = dy != 0.0f ? width : 0.0f;
			float side_y = dx != 0.0f ? width : 0.0f;

			add_quad(mesh, vec3_make(dx * from - side_x, dy * from - side_y, 0.0f), vec3_make(dx * to - side_x, dy * to - side_y, 0.0f),
				vec3_make(dx * to + side_x, dy * to + side_y, 0.0f), vec3_make(dx * from + side_x, dy * from + side_y, 0.0f), normal, black);
		}
	}
}

// The crosshair: four short bars round the middle (units of half the
// screen's height)
static void build_crosshair(mesh_builder_t *mesh)
{
	static const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float gap = 0.012f;
	const float length = 0.022f;
	const float width = 0.0022f;
	vec3_t normal = vec3_make(0.0f, 0.0f, 1.0f);

	for (int i = 0; i < 4; i++)
	{
		float dx = i == 0 ? 1.0f : (i == 1 ? -1.0f : 0.0f);
		float dy = i == 2 ? 1.0f : (i == 3 ? -1.0f : 0.0f);
		float near_x = dx * gap;
		float near_y = dy * gap;
		float far_x = dx * (gap + length);
		float far_y = dy * (gap + length);
		float side_x = dy != 0.0f ? width : 0.0f;
		float side_y = dx != 0.0f ? width : 0.0f;

		add_quad(mesh, vec3_make(near_x - side_x, near_y - side_y, 0.0f), vec3_make(far_x - side_x, far_y - side_y, 0.0f),
			vec3_make(far_x + side_x, far_y + side_y, 0.0f), vec3_make(near_x + side_x, near_y + side_y, 0.0f), normal, white);
	}
}

static SDL_GPUBuffer *upload(const mesh_builder_t *mesh)
{
	uint32_t bytes = (uint32_t)(sizeof(viewmodel_vertex_t) * mesh->count);
	SDL_GPUBuffer *buffer = mesh->count > 0 ? gpu_create_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes) : NULL;

	if (buffer != NULL)
	{
		gpu_upload_buffer(buffer, 0, mesh->vertices, bytes);
	}
	return buffer;
}

bool viewmodel_create(viewmodel_t *viewmodel)
{
	static const SDL_GPUVertexBufferDescription input = { 0, sizeof(viewmodel_vertex_t), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
	static const SDL_GPUVertexAttribute attributes[3] =
	{
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 3 },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, sizeof(float) * 6 },
	};
	SDL_GPUShader *vertex = gpu_load_shader("viewmodel.vert", 0, 0, 1);
	SDL_GPUShader *fragment = gpu_load_shader("viewmodel.frag", 0, 0, 1);
	mesh_builder_t rifle = { NULL, 0, 0 };
	mesh_builder_t scoped = { NULL, 0, 0 };
	mesh_builder_t scope_view = { NULL, 0, 0 };
	mesh_builder_t crosshair = { NULL, 0, 0 };

	SDL_zerop(viewmodel);
	if (vertex != NULL && fragment != NULL)
	{
		gpu_pipeline_info_t info;

		SDL_zero(info);
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.vertex_buffers = &input;
		info.vertex_buffer_count = 1;
		info.vertex_attributes = attributes;
		info.vertex_attribute_count = 3;
		info.cull = SDL_GPU_CULLMODE_NONE;
		info.depth_test = true;
		info.depth_write = true;
		viewmodel->pipeline = gpu_make_pipeline(&info, "viewmodel");
	}
	if (vertex != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, vertex);
	}
	if (fragment != NULL)
	{
		SDL_ReleaseGPUShader(gpu.device, fragment);
	}
	build_rifle(&rifle);
	build_scoped_rifle(&scoped);
	build_scope_view(&scope_view);
	build_crosshair(&crosshair);
	viewmodel->rifle_buffer = upload(&rifle);
	viewmodel->rifle_vertex_count = rifle.count;
	viewmodel->scoped_buffer = upload(&scoped);
	viewmodel->scoped_vertex_count = scoped.count;
	viewmodel->scope_view_buffer = upload(&scope_view);
	viewmodel->scope_view_vertex_count = scope_view.count;
	SDL_free(scoped.vertices);
	SDL_free(scope_view.vertices);
	viewmodel->crosshair_buffer = upload(&crosshair);
	viewmodel->crosshair_vertex_count = crosshair.count;
	SDL_free(rifle.vertices);
	SDL_free(crosshair.vertices);
	viewmodel->created = viewmodel->pipeline != NULL && viewmodel->rifle_buffer != NULL && viewmodel->crosshair_buffer != NULL;
	return viewmodel->created;
}

void viewmodel_destroy(viewmodel_t *viewmodel)
{
	if (gpu.device != NULL)
	{
		if (viewmodel->pipeline != NULL)
		{
			SDL_ReleaseGPUGraphicsPipeline(gpu.device, viewmodel->pipeline);
		}
		if (viewmodel->rifle_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, viewmodel->rifle_buffer);
		}
		if (viewmodel->crosshair_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, viewmodel->crosshair_buffer);
		}
		if (viewmodel->scoped_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, viewmodel->scoped_buffer);
		}
		if (viewmodel->scope_view_buffer != NULL)
		{
			SDL_ReleaseGPUBuffer(gpu.device, viewmodel->scope_view_buffer);
		}
	}
	SDL_zerop(viewmodel);
}

static mat4_t translation(float x, float y, float z)
{
	mat4_t result = mat4_identity();

	result.m[12] = x;
	result.m[13] = y;
	result.m[14] = z;
	return result;
}

static mat4_t rotation(int axis, float radians)
{
	mat4_t result = mat4_identity();
	float c = SDL_cosf(radians);
	float s = SDL_sinf(radians);
	int a = (axis + 1) % 3;
	int b = (axis + 2) % 3;

	result.m[a * 4 + a] = c;
	result.m[a * 4 + b] = s;
	result.m[b * 4 + a] = -s;
	result.m[b * 4 + b] = c;
	return result;
}

// Where it is held now, in the camera's space: up or coming up, aimed or
// not, the kick, the reload's dip, the walk's bob
static mat4_t held_matrix(const viewmodel_t *viewmodel, const weapons_t *weapons)
{
	float raised = weapons_raised(weapons);
	float lowered = 1.0f - raised;
	float reload = weapons_reload_progress(weapons);
	float dip = SDL_sinf(reload * SDL_PI_F);
	float aim = weapons_aim(weapons);
	float loose = 1.0f - 0.75f * aim;
	float kick = weapons->kick * loose;
	float bob_x = SDL_sinf(viewmodel->bob_phase) * 0.006f * loose;
	float bob_y = -SDL_fabsf(SDL_cosf(viewmodel->bob_phase)) * 0.006f * loose;
	float sights = weapons_scoped(weapons) ? SCOPE_HEIGHT : SIGHTS_HEIGHT;
	float x = HELD_POSITION[0] + (0.0f - HELD_POSITION[0]) * aim;
	float y = HELD_POSITION[1] + (-sights - HELD_POSITION[1]) * aim;
	float z = HELD_POSITION[2] + (AIMED_DISTANCE - HELD_POSITION[2]) * aim;
	mat4_t matrix = translation(x + bob_x, y + bob_y - 0.32f * lowered * lowered - 0.10f * dip, z + 0.045f * kick);

	matrix = mat4_multiply(matrix, rotation(1, 0.05f * (1.0f - aim)));
	matrix = mat4_multiply(matrix, rotation(0, 0.07f * kick - 0.9f * lowered * lowered - 0.35f * dip));
	matrix = mat4_multiply(matrix, rotation(2, 0.45f * dip));
	return matrix;
}

static void camera_axes(const camera_t *camera, vec3_t *forward, vec3_t *right, vec3_t *up)
{
	// (the view matrix's rows: the camera's axes in the world)
	*right = vec3_make(camera->view.m[0], camera->view.m[4], camera->view.m[8]);
	*up = vec3_make(camera->view.m[1], camera->view.m[5], camera->view.m[9]);
	*forward = vec3_make(-camera->view.m[2], -camera->view.m[6], -camera->view.m[10]);
}

void viewmodel_muzzle_position(const viewmodel_t *viewmodel, const weapons_t *weapons, const camera_t *camera, float position_out[3])
{
	mat4_t held = held_matrix(viewmodel, weapons);
	vec4_t local = { MUZZLE[0], MUZZLE[1], MUZZLE[2], 1.0f };
	vec4_t view = mat4_transform(held, local);
	vec3_t forward;
	vec3_t right;
	vec3_t up;
	// (the viewmodel's narrower field of view than the world's: pulled in
	// a little, so the flash sits on the muzzle as drawn)
	float pull = 0.85f;

	camera_axes(camera, &forward, &right, &up);
	position_out[0] = camera->eye_position.x + (right.x * view.x + up.x * view.y) * pull - forward.x * view.z;
	position_out[1] = camera->eye_position.y + (right.y * view.x + up.y * view.y) * pull - forward.y * view.z;
	position_out[2] = camera->eye_position.z + (right.z * view.x + up.z * view.y) * pull - forward.z * view.z;
}

static float linear(float value)
{
	return SDL_powf(SDL_max(value, 0.0f), 2.2f);
}

void viewmodel_set_carried_light(viewmodel_t *viewmodel, const float position[3], const float light[3])
{
	for (int k = 0; k < 3; k++)
	{
		viewmodel->carried_position[k] = position != NULL ? position[k] : 0.0f;
		viewmodel->carried_light[k] = position != NULL && light != NULL ? light[k] : 0.0f;
	}
}

void viewmodel_draw(viewmodel_t *viewmodel, SDL_GPURenderPass *pass, const weapons_t *weapons, const camera_t *camera,
	const render_settings_t *lit, float aspect_ratio, float walking_speed, float seconds)
{
	vertex_uniforms_t vertex;
	fragment_uniforms_t fragment;
	vec3_t sun = terrain_renderer_sun_direction(lit);
	vec4_t sun_view;
	vec4_t up_view;
	SDL_GPUBufferBinding binding;

	if (!viewmodel->created || pass == NULL || !weapons_in_hand(weapons))
	{
		return;
	}
	viewmodel->bob_phase += seconds * walking_speed * 2.2f;
	SDL_zero(vertex);
	SDL_zero(fragment);
	{
		mat4_t gl = mat4_perspective(VIEWMODEL_FIELD_OF_VIEW * SDL_PI_F / 180.0f, SDL_max(aspect_ratio, 0.1f), VIEWMODEL_NEAR, VIEWMODEL_FAR);
		mat4_t held = held_matrix(viewmodel, weapons);

		gpu_clip_matrix(gl.m, vertex.projection);
		SDL_memcpy(vertex.model_view, held.m, sizeof(vertex.model_view));
	}
	vertex.parameters[0] = VIEWMODEL_DEPTH_SQUEEZE;
	vertex.parameters[2] = 1.0f / SDL_max(aspect_ratio, 0.1f);
	{
		vec4_t world_sun = { sun.x, sun.y, sun.z, 0.0f };
		vec4_t world_up = { 0.0f, 1.0f, 0.0f, 0.0f };

		sun_view = mat4_transform(camera->view, world_sun);
		up_view = mat4_transform(camera->view, world_up);
	}
	fragment.sun_direction[0] = sun_view.x;
	fragment.sun_direction[1] = sun_view.y;
	fragment.sun_direction[2] = sun_view.z;
	for (int k = 0; k < 3; k++)
	{
		float sky = linear(lit->sky_color[k]) * lit->ambient_intensity;

		fragment.sun_light[k] = linear(lit->sun_color[k]) * lit->sun_intensity * (sun.y > 0.0f ? 1.0f : 0.0f);
		fragment.sky_light[k] = sky;
		fragment.ground_light[k] = sky * 0.35f;
		fragment.flash[k] = (k == 0 ? 1.0f : (k == 1 ? 0.7f : 0.35f)) * 6.0f * weapons->flash;
	}
	{
		vec4_t carried = { viewmodel->carried_position[0], viewmodel->carried_position[1], viewmodel->carried_position[2], 1.0f };
		vec4_t carried_view = mat4_transform(camera->view, carried);

		fragment.carried_position[0] = carried_view.x;
		fragment.carried_position[1] = carried_view.y;
		fragment.carried_position[2] = carried_view.z;
		SDL_memcpy(fragment.carried, viewmodel->carried_light, sizeof(float) * 3);
	}
	fragment.up[0] = up_view.x;
	fragment.up[1] = up_view.y;
	fragment.up[2] = up_view.z;
	{
		mat4_t held = held_matrix(viewmodel, weapons);
		vec4_t muzzle = { MUZZLE[0], MUZZLE[1], MUZZLE[2] - 0.04f, 1.0f };
		vec4_t at = mat4_transform(held, muzzle);

		fragment.flash_position[0] = at.x;
		fragment.flash_position[1] = at.y;
		fragment.flash_position[2] = at.z;
	}
	SDL_BindGPUGraphicsPipeline(pass, viewmodel->pipeline);
	binding.offset = 0;
	if (weapons_through_scope(weapons))
	{
		// Through the scope: its view, not the rifle
		vertex.parameters[1] = 1.0f;
		fragment.sun_direction[3] = 1.0f;
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, &vertex, sizeof(vertex));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, &fragment, sizeof(fragment));
		binding.buffer = viewmodel->scope_view_buffer;
		SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, viewmodel->scope_view_vertex_count, 1, 0, 0);
		return;
	}
	SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, &vertex, sizeof(vertex));
	SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, &fragment, sizeof(fragment));
	if (weapons_scoped(weapons))
	{
		binding.buffer = viewmodel->scoped_buffer;
		SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, viewmodel->scoped_vertex_count, 1, 0, 0);
	}
	else
	{
		binding.buffer = viewmodel->rifle_buffer;
		SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, viewmodel->rifle_vertex_count, 1, 0, 0);
	}
	// The crosshair (while the weapon is up, not aimed), as bright as what
	// the light makes white (the exposure follows the light)
	if (weapons->state == WEAPON_STATE_READY && weapons_aim(weapons) < 0.15f)
	{
		vertex.parameters[1] = 1.0f;
		fragment.sun_direction[3] = 1.0f;
		for (int k = 0; k < 3; k++)
		{
			fragment.sun_light[k] = (fragment.sun_light[k] * SDL_max(sun.y, 0.0f) + fragment.sky_light[k]) * 0.9f + 0.02f;
		}
		SDL_PushGPUVertexUniformData(gpu.command_buffer, 0, &vertex, sizeof(vertex));
		SDL_PushGPUFragmentUniformData(gpu.command_buffer, 0, &fragment, sizeof(fragment));
		binding.buffer = viewmodel->crosshair_buffer;
		SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, viewmodel->crosshair_vertex_count, 1, 0, 0);
	}
}
