#version 450

layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_normal;
layout (location = 2) in vec2 v_uv;

layout (binding = 0, std140) uniform SceneUniforms {
	mat4 view_projection;
	mat4 light_view_projection;
	vec3 camera_position; float time;
	vec3 light_position; float shadow_bias;
	vec3 light_direction; float shadow_strength;
	float shadow_map_texel_size;
};

layout (binding = 1, std140) uniform ModelUniforms {
	mat4 model;
	vec3 albedo_color;
	float shininess;
	float use_texture;
	float use_triplanar;
};

void main() {
	vec4 world_position = model * vec4(v_position, 1.0);
	gl_Position = light_view_projection * world_position;
}
