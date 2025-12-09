#version 450

layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_normal;
layout (location = 2) in vec2 v_uv;

layout (location = 0) out vec3 f_position;
layout (location = 1) out vec3 f_normal;
layout (location = 2) out vec2 f_uv;
layout (location = 3) out vec4 f_light_space_pos_1;
layout (location = 4) out vec4 f_light_space_pos_2;

layout (binding = 0, std140) uniform SceneUniforms {
    mat4 view_projection;
    
    mat4 light_view_projection_1;
    vec3 light_direction_1;
    float _pad0_1;
    vec3 light_color_1;
    float _pad1_1;
    
    mat4 light_view_projection_2;
    vec3 light_direction_2;
    float _pad0_2;
    vec3 light_color_2;
    float _pad1_2;
    
    vec3 camera_position;
    float _pad2;
};

layout (binding = 1, std140) uniform ModelUniforms {
    mat4 model;
    vec3 albedo_color;
    float _pad;
};

void main() {
    vec4 world_pos = model * vec4(v_position, 1.0);
    vec4 world_normal = model * vec4(v_normal, 0.0);

    gl_Position = view_projection * world_pos;

    f_position = world_pos.xyz;
    f_normal   = normalize(world_normal.xyz);
    f_uv       = v_uv;

    f_light_space_pos_1 = light_view_projection_1 * world_pos; // (5) Вычисление координат в пространстве источника света
    f_light_space_pos_2 = light_view_projection_2 * world_pos; // (5) Вычисление координат в пространстве источника света
}