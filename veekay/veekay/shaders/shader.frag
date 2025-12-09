#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;
layout (location = 3) in vec4 f_light_space_pos_1;
layout (location = 4) in vec4 f_light_space_pos_2;

layout (location = 0) out vec4 final_color;

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

layout (binding = 2) uniform sampler2D shadow_map_1; // (5) Использование теневых карт
layout (binding = 3) uniform sampler2D shadow_map_2; // (5) Использование теневых карт
float calculateShadow(vec4 light_space_pos, sampler2D shadow_map) { // (5) Использование теневых карт
    vec3 proj = light_space_pos.xyz / light_space_pos.w;
    proj = proj * 0.5 + 0.5;

    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 1.0;

    float bias = 0.005;
    float current = proj.z - bias;

    // PCF фильтрация
    vec2 texel = 1.0 / textureSize(shadow_map, 0);
    float shadow = 0.0;

    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float depth = texture(shadow_map, proj.xy + vec2(x, y) * texel).r;
            shadow += current > depth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

// Интеграция с моделью Блинна-Фонга
void main() {
    vec3 N = normalize(f_normal);
    vec3 V = normalize(camera_position - f_position);
    vec3 albedo = albedo_color;

    vec3 color = albedo * 0.1 * albedo; // ambient

    // Light 1
    {
        vec3 L = normalize(-light_direction_1);
        vec3 H = normalize(L + V);
        float diff = max(dot(N, L), 0.0);
        float spec = pow(max(dot(N, H), 0.0), 32.0);
        float shadow = calculateShadow(f_light_space_pos_1, shadow_map_1);
        color += shadow * (diff + spec * 0.5) * albedo * light_color_1;
    }

    // Light 2
    {
        vec3 L = normalize(-light_direction_2);
        vec3 H = normalize(L + V);
        float diff = max(dot(N, L), 0.0);
        float spec = pow(max(dot(N, H), 0.0), 32.0);
        float shadow = calculateShadow(f_light_space_pos_2, shadow_map_2);
        color += shadow * (diff + spec * 0.5) * albedo * light_color_2;
    }

    final_color = vec4(color, 1.0);
}