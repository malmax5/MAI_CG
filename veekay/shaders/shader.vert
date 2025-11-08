#version 450

layout(binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec3 ambient_light;
    float _pad0;
    vec3 directional_light_direction;
    float _pad1;
    vec3 directional_light_color;
    float directional_light_enabled;
    vec3 camera_position;
    float _pad3;
    float ambient_light_enabled;
    float point_lights_enabled;
    float spotlight_enabled;  // Добавляем флаг прожектора
} scene;

layout(binding = 1) uniform ModelUniforms {
    mat4 model;
    vec3 albedo_color;
    float _pad0;
    // НОВОЕ: свойства материала
    vec3 specular_color;
    float _pad1;
    float shininess;
    float _pad2[3];
} model;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 out_color;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec3 out_normal;
layout(location = 3) out vec3 out_world_position;
layout(location = 4) out vec3 out_camera_position;
layout(location = 5) out vec3 out_specular_color;
layout(location = 6) out float out_shininess;

void main() {
    mat4 mvp = scene.view_projection * model.model;
    gl_Position = mvp * vec4(in_position, 1.0);
    
    // Позиция в мировом пространстве
    out_world_position = vec3(model.model * vec4(in_position, 1.0));
    
    // УПРОЩЕННОЕ преобразование нормали - используем только вращательную часть матрицы
    mat3 normal_matrix = mat3(model.model);
    out_normal = normalize(normal_matrix * in_normal);
    
    out_color = model.albedo_color;
    out_uv = in_uv;
    out_camera_position = scene.camera_position;
	out_specular_color = model.specular_color;
    out_shininess = model.shininess;
}