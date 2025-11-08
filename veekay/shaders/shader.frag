#version 450

layout(location = 0) in vec3 in_color;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec3 in_world_position;
layout(location = 4) in vec3 in_camera_position;
layout(location = 5) in vec3 in_specular_color;
layout(location = 6) in float in_shininess;

layout(location = 0) out vec4 out_color;

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

struct PointLight {
    vec3 position;
    float _pad0;
    vec3 color;
    float _pad1;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float enabled;
};

// НОВАЯ СТРУКТУРА: Прожектор
struct Spotlight {
    vec3 position;
    float _pad0;
    vec3 direction;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    float cutOff;
    float outerCutOff;
    float constant;
    float linear;
    float quadratic;
    float enabled;
};

layout(binding = 2) buffer PointLights {
    PointLight point_lights[];
};

// НОВЫЙ БАЙНДИНГ: для прожектора
layout(binding = 3) uniform SpotlightUniforms {
    Spotlight spotlight;
};

// ОСНОВНАЯ ФУНКЦИЯ РАСЧЕТА МОДЕЛИ БЛИНН-ФОНГА
vec3 calculateBlinnPhong(vec3 light_dir, vec3 light_color, vec3 normal, vec3 view_dir, vec3 specular_color, float shininess) {
    // === БЛИНН-ФОНГ: полусферический вектор вместо отраженного ===
    
    // Полусферический вектор (halfway vector) - середина между направлением света и направлением взгляда
    vec3 half_dir = normalize(light_dir + view_dir);
    
    // Specular компонент: скалярное произведение нормали и полусферического вектора
    // max(dot(normal, half_dir), 0.0) - косинус угла между нормалью и полусферическим вектором
    // pow(..., shininess) - коэффициент блеска (чем больше - тем ярче и меньше пятно)
    float spec = pow(max(dot(normal, half_dir), 0.0), shininess);
    
    // Возвращаем specular цвет: цвет света * specular интенсивность * цвет материала
    return light_color * spec * specular_color;
}

vec3 calculatePointLight(PointLight light, vec3 normal, vec3 frag_pos, vec3 view_dir, vec3 specular_color, float shininess) {
    if (light.enabled < 0.5) return vec3(0.0);
    
    vec3 light_dir = normalize(light.position - frag_pos);
    
    // Защита от нулевых нормалей
    if (length(normal) < 0.001) {
        normal = vec3(0.0, 1.0, 0.0);
    } else {
        normal = normalize(normal);
    }
    
    // Диффузная составляющая
    float diff = max(dot(normal, light_dir), 0.0);
    
    // Specular составляющая (Блинн-Фонг)
    vec3 specular = calculateBlinnPhong(light_dir, light.color, normal, view_dir, specular_color, shininess);
    
    // ЗАКОН ОБРАТНЫХ КВАДРАТОВ
    float distance = length(light.position - frag_pos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + 
                         light.quadratic * (distance * distance));
	//
    
    vec3 diffuse = light.color * diff * light.intensity * attenuation;
    specular *= light.intensity * attenuation;
    
    return min(diffuse + specular, vec3(3.0));
}

// Аналогично обновим функцию прожектора
vec3 calculateSpotlight(Spotlight light, vec3 normal, vec3 frag_pos, vec3 view_dir, vec3 specular_color, float shininess) {
    if (light.enabled < 0.5) return vec3(0.0);
    
    vec3 light_dir = normalize(light.position - frag_pos);
    float distance = length(light.position - frag_pos);
    
    // Затухание
    float attenuation = 1.0 / (light.constant + light.linear * distance + 
                         light.quadratic * (distance * distance));
    
    // Угловой расчет
    float theta = dot(light_dir, normalize(-light.direction));
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
    
    // Нормализация
    if (length(normal) < 0.001) {
        normal = vec3(0.0, 1.0, 0.0);
    } else {
        normal = normalize(normal);
    }
    
    // Диффузная составляющая
    float diff = max(dot(normal, light_dir), 0.0);
    
    // Specular составляющая (Блинн-Фонг)
    vec3 specular = calculateBlinnPhong(light_dir, light.color, normal, view_dir, specular_color, shininess);
    
    vec3 diffuse = light.color * diff * light.intensity * attenuation * intensity;
    specular *= light.intensity * attenuation * intensity;
    
    return min(diffuse + specular, vec3(4.0));
}

void main() {
    vec3 final_color = vec3(0.0);
    vec3 view_dir = normalize(in_camera_position - in_world_position);
    vec3 normal = normalize(in_normal);
    
    // Рассеянное освещение (только диффузное)
    if (scene.ambient_light_enabled > 0.5) {
        final_color += scene.ambient_light * in_color;
    }
    
    // Направленное освещение с Блинн-Фонгом
	if (scene.directional_light_enabled > 0.5) {
	    vec3 light_dir = normalize(-scene.directional_light_direction);
	
	    // Диффузная составляющая (Lambert)
	    float diff = max(dot(normal, light_dir), 0.0);
	    vec3 diffuse = scene.directional_light_color * diff * in_color;
	
	    // === SPECULAR КОМПОНЕНТ ДЛЯ НАПРАВЛЕННОГО СВЕТА (БЛИНН-ФОНГ) ===
	    vec3 specular = calculateBlinnPhong(light_dir, scene.directional_light_color, normal, view_dir, in_specular_color, in_shininess);
	
	    final_color += diffuse + specular;  // Диффузный + Specular
	}
    
    // Точечные источники с Блинн-Фонгом
	if (scene.point_lights_enabled > 0.5) {
	    vec3 point_contrib = vec3(0.0);
	    for(int i = 0; i < 3; i++) {
	        if (point_lights[i].enabled > 0.5) {
	            // calculatePointLight ВНУТРИ использует calculateBlinnPhong
	            point_contrib += calculatePointLight(point_lights[i], normal, in_world_position, view_dir, in_specular_color, in_shininess);
	        }
	    }
	    final_color += point_contrib * in_color;
	}
    
    // Прожектор с Блинн-Фонгом
    if (scene.spotlight_enabled > 0.5) {
        vec3 spotlight_contrib = calculateSpotlight(spotlight, normal, in_world_position, view_dir, in_specular_color, in_shininess);
        final_color += spotlight_contrib * in_color;
    }
    
    // Tone mapping и гамма-коррекция
    final_color = clamp(final_color, 0.0, 4.0);
    final_color = final_color / (final_color + vec3(1.0));
    final_color = pow(final_color, vec3(1.0/2.2));
    
    out_color = vec4(final_color, 1.0);
}