#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
// ПУНКТ 6: Фрагментный шейдер использует f_uv для сэмплирования текстур.
layout (location = 2) in vec2 f_uv;

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
	mat4 view_projection;
	vec3 ambient_light;
	vec3 directional_light_direction;
	vec3 directional_light_color;
	float directional_light_enabled;
	vec3 camera_position;
	float time;
	float ambient_light_enabled;
	float point_lights_enabled;
	float spotlight_enabled;
};

layout (binding = 1, std140) uniform ModelUniforms {
	mat4 model;
	vec3 albedo_color;
	vec3 specular_color;
	float shininess;
};

// ПУНКТ 9: albedo, specular, emissive, загружаются и используются
layout (set = 1, binding = 0) uniform sampler2D texture_albedo;
layout (set = 1, binding = 1) uniform sampler2D texture_specular;
layout (set = 1, binding = 2) uniform sampler2D texture_emissive;

// ПУНКТ 8: multiSample и distortUV реализуют сложное сэмплирование
vec2 distortUV(vec2 uv, vec3 worldPos, float time) {
    vec2 distorted = uv;
    
    vec2 center = vec2(0.5, 0.5);
    vec2 toCenter = uv - center;
    float angle = atan(toCenter.y, toCenter.x);
    float radius = length(toCenter);
    
	// Создает спиральные искажения от центра (водоворот)
    distorted += vec2(
        cos(angle + radius * 10.0 + time) * 0.02,
        sin(angle + radius * 10.0 + time) * 0.02
    );
    
	// Создает волны, зависящие от положения в мире
	// Зависит от положения в МИРЕ (worldPos)
    distorted.x += sin(worldPos.y * 3.0 + time * 2.0) * 0.01;
    distorted.y += cos(worldPos.x * 3.0 + time * 2.0) * 0.01;
    
	// Зависит от положения на ТЕКСТУРЕ (uv)
    distorted.x += sin(worldPos.z * 5.0 + time) * 0.005 * sin(uv.y * 20.0);
    distorted.y += cos(worldPos.z * 5.0 + time) * 0.005 * cos(uv.x * 20.0);
    
    return distorted;
}

// ПУНКТ 8: multiSample и distortUV реализуют сложное сэмплирование
vec4 multiSample(sampler2D tex, vec2 baseUV, vec3 worldPos, float time) {
	// Создаем аккумулятор для финального цвета
    vec4 finalColor = vec4(0.0);
	// Oтслеживает сумму весов для правильного усреднения
    float totalWeight = 0.0;
    
    vec2 mainUV = distortUV(baseUV, worldPos, time); // Искажаем координаты
    vec4 mainSample = texture(tex, mainUV);			 // Берем основной образец
    finalColor += mainSample * 0.4; 				 // Даем ему большой вес 40%
    totalWeight += 0.4;
    
	// 8 дополнительных сэмплов - 60%
    const int SAMPLES = 8;
    for (int i = 0; i < SAMPLES; i++) {
		// Вычисляем угол для кругового распределения
        float angle = float(i) * (3.14159 * 2.0 / float(SAMPLES));
		// Анимированное смещение (зависит от времени)
        float offset = sin(time + float(i)) * 0.01;
        
		// Создаем смещенные координаты по кругу
        vec2 sampleUV = distortUV(baseUV, worldPos, time) + 	  
                       vec2(cos(angle) * offset, sin(angle) * offset);
        vec4 sampleColor = texture(tex, sampleUV);							
        
		// Вес каждого дополнительного сэмпла
        float weight = 0.6 / float(SAMPLES);

		// Модификация цвета для каждого сэмпла
        float colorMod = sin(worldPos.x * 2.0 + angle + time) * 0.1 + 0.9;
        sampleColor.rgb *= colorMod;
        
        finalColor += sampleColor * weight;
        totalWeight += weight;
    }
    
    if (totalWeight > 0.0) {
        finalColor /= totalWeight;
    }
    
    return finalColor;
}

float random(vec2 st) {
    return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123);
}

// ПУНКТ 9: albedo, specular, emissive, загружаются и используются
vec3 calculateLighting(vec3 albedo, vec3 specularMap, vec3 emissive, vec3 normal, vec3 viewDir) {
    vec3 result = vec3(0.0);
    
    // Ambient lighting - всегда виден
    if (ambient_light_enabled > 0.5) {
        result += ambient_light * albedo;
    }
    
    // Directional lighting - зависит от интенсивности
    if (directional_light_enabled > 0.5) {
        vec3 lightDir = normalize(-directional_light_direction);
        
        // Diffuse
        float diff = max(dot(normal, lightDir), 0.0);
		// При сильном освещении: albedo текстура умножается на яркий свет, поэтому видна исходная текстура кирпича/дерева
		// При слабом освещении: diffuse становится почти нулевым, поэтому виден только очень темная версия albedo текстуры
        vec3 diffuse = directional_light_color * diff * albedo;
        
        // Specular (используем текстуру specular для управления интенсивностью бликов)
        vec3 reflectDir = reflect(-lightDir, normal);
        float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
        vec3 specular = directional_light_color * spec * specular_color * specularMap.r;
        
        result += diffuse + specular;
    }
    
    // Emissive (добавляется независимо от освещения)
    result += emissive;
    
    return result;
}

void main() {
    // ПУНКТ 6: Фрагментный шейдер использует f_uv для сэмплирования текстур.
    vec4 albedo_tex = multiSample(texture_albedo, f_uv, f_position, time);
    vec4 specular_tex = texture(texture_specular, f_uv);
    vec4 emissive_tex = texture(texture_emissive, f_uv);
    
    // Применяем цвет материала к текстурам
    vec3 albedo = albedo_tex.rgb * albedo_color;
    vec3 specularMap = specular_tex.rgb;
    vec3 emissive = emissive_tex.rgb;
    
    // Нормализуем нормаль и вычисляем направление взгляда
    vec3 normal = normalize(f_normal);
    vec3 viewDir = normalize(camera_position - f_position);
    
    // Расчет освещения с использованием всех текстур
    vec3 litColor = calculateLighting(albedo, specularMap, emissive, normal, viewDir);
    
    // ПУНКТ Черные параллельные линии (эффект старого телевизора)
	// f_uv.y * 800.0 - создает очень частые полосы
	// sin(...) - создает чередующиеся светлые и темные полосы
	// + time * 5.0 - анимирует линии, заставляя их "бежать"
	// При отдалении камеры текстура сжимается, поэтому линии становятся реже и шире
	// При повороте камеры искажается перспективная проекция текстурных координат
    float scanLine = sin(f_uv.y * 800.0 + time * 5.0) * 0.1 + 0.9;
    litColor *= scanLine;
    
    vec2 center = vec2(0.5, 0.5);
    float vignette = 1.0 - length(f_uv - center) * 0.5;
    vignette = clamp(vignette, 0.7, 1.0);
    litColor *= vignette;
    
    float noise = random(f_uv + time) * 0.05 + 0.95;
    litColor *= noise;
    
    final_color = vec4(litColor, albedo_tex.a);
}