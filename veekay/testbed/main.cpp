#include "veekay/input.hpp"
#include <cstdint>
#include <climits>
#include <vector>
#include <iostream>
#include <fstream>

#define _USE_MATH_DEFINES
#include <math.h>

#include <veekay/veekay.hpp>

#include <imgui.h>
#include <vulkan/vulkan_core.h>

#include <lodepng.h>

namespace {

constexpr uint32_t max_models = 1024;

struct Vertex {
	veekay::vec3 position;  // Позиция вершины в локальном пространстве
	veekay::vec3 normal;    // НОРМАЛЬ вершины - вектор перпендикулярный поверхности
	veekay::vec2 uv;
	// NOTE: You can add more attributes
};

struct SceneUniforms {
    veekay::mat4 view_projection;
    veekay::vec3 ambient_light; float _pad0;
    veekay::vec3 directional_light_direction; float _pad1;
    veekay::vec3 directional_light_color; 
    float directional_light_enabled;
    veekay::vec3 camera_position; float _pad3;
    float ambient_light_enabled;
    float point_lights_enabled;
    float spotlight_enabled;  // НОВОЕ: флаг включения прожектора
};

struct PointLight {
    veekay::vec3 position; float _pad0;
    veekay::vec3 color; float _pad1;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float enabled;
};

// НОВАЯ СТРУКТУРА: Прожектор
struct Spotlight {
    veekay::vec3 position; float _pad0;
    veekay::vec3 direction; float _pad1;
    veekay::vec3 color; float _pad2;
    float intensity;
    float cutOff;        // Внутренний угол (косинус)
    float outerCutOff;   // Внешний угол (косинус)
    float constant;
    float linear;
    float quadratic;
    float enabled;
};

struct ModelUniforms {
    veekay::mat4 model;
    veekay::vec3 albedo_color; float _pad0;
    veekay::vec3 specular_color; float _pad1;
    float shininess; float _pad2[3];
};

struct Mesh {
	veekay::graphics::Buffer* vertex_buffer;
	veekay::graphics::Buffer* index_buffer;
	uint32_t indices;
};

struct Transform {
	veekay::vec3 position = {};
	veekay::vec3 scale = {1.0f, 1.0f, 1.0f};
	veekay::vec3 rotation = {};

	// NOTE: Model matrix (translation, rotation and scaling)
	veekay::mat4 matrix() const;
};

struct Model {
    Mesh mesh;
    Transform transform;
    veekay::vec3 albedo_color;
    // НОВОЕ: specular параметры
    veekay::vec3 specular_color;
    float shininess;
};

struct Camera {
    constexpr static float default_fov = 60.0f;
    constexpr static float default_near_plane = 0.01f;
    constexpr static float default_far_plane = 100.0f;

    // === ПОЛОЖЕНИЕ И ОРИЕНТАЦИЯ КАМЕРЫ ===
    veekay::vec3 position = {};     // Позиция камеры в мировом пространстве
    veekay::vec3 rotation = {};     // Ориентация камеры (углы Эйлера: pitch, yaw, roll)
    
    // НОВОЕ: Поля для режима Look-At
    veekay::vec3 target = {0.0f, 0.0f, 0.0f};  // Точка, на которую смотрит камера
    veekay::vec3 up = {0.0f, 1.0f, 0.0f};      // Вектор "вверх" для камеры
    bool lookat_mode = false;                   // Режим камеры: FPS или Look-At

	// НОВОЕ: Сохраненное состояние камеры
    veekay::vec3 saved_position = {};
    veekay::vec3 saved_rotation = {};

    // Параметры проекции
    float fov = default_fov;
    float near_plane = default_near_plane;
    float far_plane = default_far_plane;

    // === МЕТОДЫ РАСЧЕТА МАТРИЦ ТРАНСФОРМАЦИИ КАМЕРЫ ===
    
    // NOTE: View matrix of camera (inverse of a transform)
    veekay::mat4 view() const;

    // NOTE: View and projection composition
    veekay::mat4 view_projection(float aspect_ratio) const;
    
    // НОВОЕ: Метод для вычисления матрицы Look-At
    veekay::mat4 lookat_view() const;
    
    // НОВОЕ: Метод для сохранения состояния камеры
    void save_state() {
        saved_position = position;
        saved_rotation = rotation;
    }
    
    // НОВОЕ: Метод для восстановления состояния камеры
    void restore_state() {
        position = saved_position;
        rotation = saved_rotation;
    }
};

veekay::mat4 Camera::lookat_view() const {
    // Используем встроенную функцию lookAt для создания матрицы вида
    // position - позиция камеры в мировом пространстве
    // target - точка, на которую смотрит камера  
    // up - вектор "вверх" для камеры (обычно {0,1,0})
    return veekay::mat4::lookAt(position, target, up);
}

// NOTE: Scene objects
inline namespace {
	Camera camera{
		.position = {0.0f, -0.5f, -3.0f}
	};

	std::vector<Model> models;
}

// NOTE: Vulkan objects
inline namespace {
	VkShaderModule vertex_shader_module;
	VkShaderModule fragment_shader_module;

	VkDescriptorPool descriptor_pool;
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorSet descriptor_set;

	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;

	veekay::graphics::Buffer* scene_uniforms_buffer;
	veekay::graphics::Buffer* model_uniforms_buffer;

	Mesh plane_mesh;
	Mesh cube_mesh;

	veekay::graphics::Texture* missing_texture;
	VkSampler missing_texture_sampler;

	veekay::graphics::Texture* texture;
	VkSampler texture_sampler;

	constexpr uint32_t max_point_lights = 16;
	veekay::graphics::Buffer* point_lights_buffer;

	// НОВЫЙ БУФЕР: для прожектора
	veekay::graphics::Buffer* spotlight_buffer;

	std::vector<PointLight> point_lights;
	Spotlight spotlight;  // НОВЫЙ: один прожектор
}

inline namespace {
    // Структура для хранения всех настроек освещения
	struct LightingControls {
	    bool ambient_enabled = true;        // Включение рассеянного света
	    bool directional_enabled = true;    // Включение направленного света
	    bool point_lights_enabled = true;   // Включение точечных источников
	    bool spotlight_enabled = true;      // Включение прожектора
	
	    // ТОЧЕЧНЫЙ ИСТОЧНИК
	    bool point_light_enabled = true;    // Включение конкретного точечного источника
	    float point_intensity = 2.0f;       // Интенсивность точечного света
	    veekay::vec3 point_color = {1.0f, 1.0f, 1.0f};  // Цвет точечного света
	    veekay::vec3 point_position = {0.0f, 3.0f, 0.0f}; // Позиция точечного света
	
	    // ПРОЖЕКТОР
	    float spotlight_intensity = 3.0f;   // Интенсивность прожектора
	    veekay::vec3 spotlight_color = {1.0f, 1.0f, 0.8f};  // Цвет прожектора
	    veekay::vec3 spotlight_position = {0.0f, 2.0f, 2.0f}; // Позиция прожектора
	    veekay::vec3 spotlight_direction = {0.0f, -0.5f, -1.0f}; // Направление прожектора
	    float spotlight_cutoff = 12.5f;     // Внутренний угол (градусы)
	    float spotlight_outer_cutoff = 17.5f; // Внешний угол (градусы)
	
	    // НАПРАВЛЕННЫЙ СВЕТ
	    float directional_intensity = 1.0f; // Интенсивность направленного света
	    veekay::vec3 directional_color = {0.8f, 0.8f, 0.6f}; // Цвет направленного света
	    veekay::vec3 directional_direction = {0.5f, -1.0f, 0.5f}; // Направление (как солнце)
	
	    // РАССЕЯННЫЙ СВЕТ
	    veekay::vec3 ambient_color = {0.1f, 0.1f, 0.1f}; // Цвет рассеянного освещения
	};
    
    LightingControls lighting_controls;
}

float toRadians(float degrees) {
	return degrees * float(M_PI) / 180.0f;
}

veekay::mat4 Transform::matrix() const {
    veekay::mat4 translation_matrix = veekay::mat4::translation(position);
    
    // Создаем матрицу вращения из углов Эйлера
    veekay::mat4 rotation_x = veekay::mat4::rotation({1.0f, 0.0f, 0.0f}, rotation.x);
    veekay::mat4 rotation_y = veekay::mat4::rotation({0.0f, 1.0f, 0.0f}, rotation.y);
    veekay::mat4 rotation_z = veekay::mat4::rotation({0.0f, 0.0f, 1.0f}, rotation.z);
    
    veekay::mat4 rotation_matrix = rotation_z * rotation_y * rotation_x;
    veekay::mat4 scale_matrix = veekay::mat4::scaling(scale);
    
    return translation_matrix * rotation_matrix * scale_matrix;
}

veekay::mat4 Camera::view() const {
    // Если включен режим Look-At, используем соответствующую матрицу
    if (lookat_mode) {
        return lookat_view();
    } else {
        // РАСЧЕТ МАТРИЦЫ ВИДА ДЛЯ FPS-КАМЕРЫ
        // Старая реализация FPS-камеры на основе углов Эйлера
        
        // Извлекаем углы вращения из вектора rotation
        float yaw = rotation.y;   // Горизонтальное вращение (влево/вправо)
        float pitch = rotation.x; // Вертикальное вращение (вверх/вниз)
        
        // === ВЫЧИСЛЕНИЕ ВЕКТОРОВ НАПРАВЛЕНИЯ КАМЕРЫ ===
        
        // Вычисляем вектор "вперед" (направление взгляда) из углов Эйлера
        veekay::vec3 front = {
            cos(yaw) * cos(pitch),  // X компонента
            sin(pitch),             // Y компонента (вертикаль)
            sin(yaw) * cos(pitch)   // Z компонента
        };
        
        // Нормализуем вектор направления
        front = veekay::vec3::normalized(front);
        
        // Мировой вектор "вверх" (обычно ось Y)
        veekay::vec3 world_up = {0.0f, 1.0f, 0.0f};
        
        // Вычисляем вектор "вправо" - перпендикулярен направлению взгляда и мировому "вверх"
        veekay::vec3 right = veekay::vec3::normalized(veekay::vec3::cross(front, world_up));
        
        // Пересчитываем "локальный вверх" камеры
        veekay::vec3 up = veekay::vec3::cross(right, front);
        
        // ПОСТРОЕНИЕ МАТРИЦЫ ВИДА (VIEW MATRIX)
        // Матрица вида = обратная (inverse) матрице трансформации камеры
        veekay::mat4 view_matrix = veekay::mat4::identity();
        
        // Заполняем матрицу вида осями локальной системы координат камеры
        view_matrix[0] = veekay::vec4({right.x, up.x, -front.x, 0.0f});  // ось X (право)
        view_matrix[1] = veekay::vec4({right.y, up.y, -front.y, 0.0f});  // ось Y (вверх)
        view_matrix[2] = veekay::vec4({right.z, up.z, -front.z, 0.0f});  // ось Z (назад)
        
        // Позиция камеры (трансляция)
        view_matrix[3] = veekay::vec4({
            -veekay::vec3::dot(right, position),    // Сдвиг по X
            -veekay::vec3::dot(up, position),       // Сдвиг по Y
            veekay::vec3::dot(front, position),     // Сдвиг по Z
            1.0f
        });
        
        return view_matrix;
    }
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
    // Создаем матрицу проекции (перспективная проекция)
    auto projection = veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane);
    
    // Композиция: view * projection
    // Сначала применяем преобразование вида (камера), затем проекцию
    return view() * projection;
}

// NOTE: Loads shader byte code from file
// NOTE: Your shaders are compiled via CMake with this code too, look it up
VkShaderModule loadShaderModule(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	size_t size = file.tellg();
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	file.close();

	VkShaderModuleCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = buffer.data(),
	};

	VkShaderModule result;
	if (vkCreateShaderModule(veekay::app.vk_device, &
	                         info, nullptr, &result) != VK_SUCCESS) {
		return nullptr;
	}

	return result;
}

void initialize(VkCommandBuffer cmd) {
	VkDevice& device = veekay::app.vk_device;
	VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

	{ // NOTE: Build graphics pipeline
		vertex_shader_module = loadShaderModule("../../veekay/shaders/shader.vert.spv");
		if (!vertex_shader_module) {
			std::cerr << "Failed to load Vulkan vertex shader from file\n";
			veekay::app.running = false;
			return;
		}

		fragment_shader_module = loadShaderModule("../../veekay/shaders/shader.frag.spv");
		if (!fragment_shader_module) {
			std::cerr << "Failed to load Vulkan fragment shader from file\n";
			veekay::app.running = false;
			return;
		}

		VkPipelineShaderStageCreateInfo stage_infos[2];

		// NOTE: Vertex shader stage
		stage_infos[0] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_shader_module,
			.pName = "main",
		};

		// NOTE: Fragment shader stage
		stage_infos[1] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_shader_module,
			.pName = "main",
		};

		// NOTE: How many bytes does a vertex take?
		VkVertexInputBindingDescription buffer_binding{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		// NOTE: Declare vertex attributes
		VkVertexInputAttributeDescription attributes[] = {
			{
				.location = 0, // NOTE: First attribute
				.binding = 0, // NOTE: First vertex buffer
				.format = VK_FORMAT_R32G32B32_SFLOAT, // NOTE: 3-component vector of floats
				.offset = offsetof(Vertex, position), // NOTE: Offset of "position" field in a Vertex struct
			},
			{
				.location = 1,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = offsetof(Vertex, normal),
			},
			{
				.location = 2,
				.binding = 0,
				.format = VK_FORMAT_R32G32_SFLOAT,
				.offset = offsetof(Vertex, uv),
			},
		};

		// NOTE: Describe inputs
		VkPipelineVertexInputStateCreateInfo input_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &buffer_binding,
			.vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
			.pVertexAttributeDescriptions = attributes,
		};

		// NOTE: Every three vertices make up a triangle,
		//       so our vertex buffer contains a "list of triangles"
		VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		// NOTE: Declare clockwise triangle order as front-facing
		//       Discard triangles that are facing away
		//       Fill triangles, don't draw lines instaed
		VkPipelineRasterizationStateCreateInfo raster_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_BACK_BIT,
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1.0f,
		};

		// NOTE: Use 1 sample per pixel
		VkPipelineMultisampleStateCreateInfo sample_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			.sampleShadingEnable = false,
			.minSampleShading = 1.0f,
		};

		VkViewport viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = static_cast<float>(veekay::app.window_width),
			.height = static_cast<float>(veekay::app.window_height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};

		VkRect2D scissor{
			.offset = {0, 0},
			.extent = {veekay::app.window_width, veekay::app.window_height},
		};

		// NOTE: Let rasterizer draw on the entire window
		VkPipelineViewportStateCreateInfo viewport_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,

			.viewportCount = 1,
			.pViewports = &viewport,

			.scissorCount = 1,
			.pScissors = &scissor,
		};

		// NOTE: Let rasterizer perform depth-testing and overwrite depth values on condition pass
		VkPipelineDepthStencilStateCreateInfo depth_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = true,
			.depthWriteEnable = true,
			.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		};

		// NOTE: Let fragment shader write all the color channels
		VkPipelineColorBlendAttachmentState attachment_info{
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			                  VK_COLOR_COMPONENT_G_BIT |
			                  VK_COLOR_COMPONENT_B_BIT |
			                  VK_COLOR_COMPONENT_A_BIT,
		};

		// NOTE: Let rasterizer just copy resulting pixels onto a buffer, don't blend
		VkPipelineColorBlendStateCreateInfo blend_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,

			.logicOpEnable = false,
			.logicOp = VK_LOGIC_OP_COPY,

			.attachmentCount = 1,
			.pAttachments = &attachment_info
		};

		{
			VkDescriptorPoolSize pools[] = {
    		    {
    		        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    		        .descriptorCount = 8,
    		    },
    		    {
    		        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
    		        .descriptorCount = 8,
    		    },
    		    {
    		        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    		        .descriptorCount = 8,
    		    }
    		};
			
			VkDescriptorPoolCreateInfo info{
    		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    		    .maxSets = 1,
    		    .poolSizeCount = sizeof(pools) / sizeof(pools[0]),
    		    .pPoolSizes = pools,
    		};

			if (vkCreateDescriptorPool(device, &info, nullptr,
    		                           &descriptor_pool) != VK_SUCCESS) {
    		    std::cerr << "Failed to create Vulkan descriptor pool\n";
    		    veekay::app.running = false;
    		    return;
    		}
		}

		// NOTE: Descriptor set layout specification
		{
			VkDescriptorSetLayoutBinding bindings[] = {
    		    {
    		        .binding = 0,
    		        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    		        .descriptorCount = 1,
    		        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    		    },
    		    {
    		        .binding = 1,
    		        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
    		        .descriptorCount = 1,
    		        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    		    },
    		    {
    		        .binding = 2,
    		        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    		        .descriptorCount = 1,
    		        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    		    },
    		    // НОВЫЙ БАЙНДИНГ: для прожектора
    		    {
    		        .binding = 3,
    		        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    		        .descriptorCount = 1,
    		        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    		    },
    		};

			VkDescriptorSetLayoutCreateInfo info{
    		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    		    .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
    		    .pBindings = bindings,
    		};
		
    		if (vkCreateDescriptorSetLayout(device, &info, nullptr,
    		                                &descriptor_set_layout) != VK_SUCCESS) {
    		    std::cerr << "Failed to create Vulkan descriptor set layout\n";
    		    veekay::app.running = false;
    		    return;
    		}
		}

		{
			VkDescriptorSetAllocateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = descriptor_pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &descriptor_set_layout,
			};

			if (vkAllocateDescriptorSets(device, &info, &descriptor_set) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor set\n";
				veekay::app.running = false;
				return;
			}
		}

		// NOTE: Declare external data sources, only push constants this time
		VkPipelineLayoutCreateInfo layout_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &descriptor_set_layout,
		};

		// NOTE: Create pipeline layout
		if (vkCreatePipelineLayout(device, &layout_info,
		                           nullptr, &pipeline_layout) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline layout\n";
			veekay::app.running = false;
			return;
		}
		
		VkGraphicsPipelineCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2,
			.pStages = stage_infos,
			.pVertexInputState = &input_state_info,
			.pInputAssemblyState = &assembly_state_info,
			.pViewportState = &viewport_info,
			.pRasterizationState = &raster_info,
			.pMultisampleState = &sample_info,
			.pDepthStencilState = &depth_info,
			.pColorBlendState = &blend_info,
			.layout = pipeline_layout,
			.renderPass = veekay::app.vk_render_pass,
		};

		// NOTE: Create graphics pipeline
		if (vkCreateGraphicsPipelines(device, nullptr,
		                              1, &info, nullptr, &pipeline) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline\n";
			veekay::app.running = false;
			return;
		}
	}

	scene_uniforms_buffer = new veekay::graphics::Buffer(
		sizeof(SceneUniforms),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	model_uniforms_buffer = new veekay::graphics::Buffer(
		max_models * sizeof(ModelUniforms),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	
	point_lights_buffer = new veekay::graphics::Buffer(
    	max_point_lights * sizeof(PointLight),
    	nullptr,
    	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); // Тип: STORAGE буфер
	
	spotlight_buffer = new veekay::graphics::Buffer(
    	sizeof(Spotlight),
    	nullptr,
    	VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); // Тип: STORAGE буфер
	
	PointLight initial_light = {};
	std::fill_n(static_cast<PointLight*>(point_lights_buffer->mapped_region),
	            max_point_lights, initial_light);
	
	// Инициализация одного центрального источника света
	point_lights.push_back(PointLight{
	    .position = lighting_controls.point_position,
	    .color = lighting_controls.point_color,
	    .intensity = lighting_controls.point_intensity,
	    .constant = 1.0f,
	    .linear = 0.09f,
	    .quadratic = 0.032f,
	    .enabled = lighting_controls.point_light_enabled ? 1.0f : 0.0f
	});

	// Инициализация прожектора
	spotlight = Spotlight{
	    .position = lighting_controls.spotlight_position,
	    .direction = veekay::vec3::normalized(lighting_controls.spotlight_direction),
	    .color = lighting_controls.spotlight_color,
	    .intensity = lighting_controls.spotlight_intensity,
	    .cutOff = cos(toRadians(lighting_controls.spotlight_cutoff)),
	    .outerCutOff = cos(toRadians(lighting_controls.spotlight_outer_cutoff)),
	    .constant = 1.0f,
	    .linear = 0.09f,
	    .quadratic = 0.032f,
	    .enabled = lighting_controls.spotlight_enabled ? 1.0f : 0.0f
	};

	// NOTE: This texture and sampler is used when texture could not be loaded
	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &missing_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		uint32_t pixels[] = {
			0xff000000, 0xffff00ff,
			0xffff00ff, 0xff000000,
		};

		missing_texture = new veekay::graphics::Texture(cmd, 2, 2,
		                                                VK_FORMAT_B8G8R8A8_UNORM,
		                                                pixels);
	}

	{
	// Обновим дескрипторы
	{
	    VkDescriptorBufferInfo buffer_infos[] = {
	        {
	            .buffer = scene_uniforms_buffer->buffer,
	            .offset = 0,
	            .range = sizeof(SceneUniforms),
	        },
	        {
	            .buffer = model_uniforms_buffer->buffer,
	            .offset = 0,
	            .range = sizeof(ModelUniforms),
	        },
	        {
	            .buffer = point_lights_buffer->buffer,
	            .offset = 0,
	            .range = max_point_lights * sizeof(PointLight),
	        },
	        // НОВЫЙ ДЕСКРИПТОР: для прожектора
	        {
	            .buffer = spotlight_buffer->buffer,
	            .offset = 0,
	            .range = sizeof(Spotlight),
	        },
	    };

	    VkWriteDescriptorSet write_infos[] = {
	        {
	            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet = descriptor_set,
	            .dstBinding = 0,
	            .dstArrayElement = 0,
	            .descriptorCount = 1,
	            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	            .pBufferInfo = &buffer_infos[0],
	        },
	        {
	            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet = descriptor_set,
	            .dstBinding = 1,
	            .dstArrayElement = 0,
	            .descriptorCount = 1,
	            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
	            .pBufferInfo = &buffer_infos[1],
	        },
	        {
	            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet = descriptor_set,
	            .dstBinding = 2,
	            .dstArrayElement = 0,
	            .descriptorCount = 1,
	            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	            .pBufferInfo = &buffer_infos[2],
	        },
	        // НОВЫЙ ДЕСКРИПТОР: для прожектора
	        {
	            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet = descriptor_set,
	            .dstBinding = 3,
	            .dstArrayElement = 0,
	            .descriptorCount = 1,
	            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	            .pBufferInfo = &buffer_infos[3],
	        },
	    };

	    vkUpdateDescriptorSets(device, sizeof(write_infos) / sizeof(write_infos[0]),
	                           write_infos, 0, nullptr);
	}
	}

	// NOTE: Plane mesh initialization
	{
		// (v0)------(v1)
		//  |  \       |
		//  |   `--,   |
		//  |       \  |
		// (v3)------(v2)
		std::vector<Vertex> vertices = {
			{{-5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
			{{5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
			{{5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
		};

		std::vector<uint32_t> indices = {
			0, 1, 2, 2, 3, 0
		};

		plane_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		plane_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		plane_mesh.indices = uint32_t(indices.size());
	}

	// NOTE: Cube mesh initialization
	{
		std::vector<Vertex> vertices = {
        // Передняя грань
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        {{+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{+0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},

        // Правая грань
        {{+0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{+0.5f, -0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{+0.5f, +0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{+0.5f, +0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        // Задняя грань
        {{+0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{+0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},

        // Левая грань
        {{-0.5f, -0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, +0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, +0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        // Нижняя грань
        {{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        {{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},

        // Верхняя грань
        {{-0.5f, +0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{+0.5f, +0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{+0.5f, +0.5f, +0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, +0.5f, +0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
    };

		std::vector<uint32_t> indices = {
			0, 1, 2, 2, 3, 0,
			4, 5, 6, 6, 7, 4,
			8, 9, 10, 10, 11, 8,
			12, 13, 14, 14, 15, 12,
			16, 17, 18, 18, 19, 16,
			20, 21, 22, 22, 23, 20,
		};

		cube_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		cube_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		cube_mesh.indices = uint32_t(indices.size());
	}

	// NOTE: Add models to scene
models.emplace_back(Model{
    .mesh = plane_mesh,
    .transform = Transform{},
    .albedo_color = veekay::vec3{0.8f, 0.8f, 0.8f},
    // НОВОЕ: specular параметры
    .specular_color = veekay::vec3{0.5f, 0.5f, 0.5f},
    .shininess = 32.0f
});

models.emplace_back(Model{
    .mesh = cube_mesh,
    .transform = Transform{
        .position = {-2.0f, -0.5f, -1.5f},
    },
    .albedo_color = veekay::vec3{1.0f, 0.0f, 0.0f},
    .specular_color = veekay::vec3{0.7f, 0.3f, 0.3f},
    .shininess = 64.0f
});

models.emplace_back(Model{
    .mesh = cube_mesh,
    .transform = Transform{
        .position = {1.5f, -0.5f, -0.5f},
    },
    .albedo_color = veekay::vec3{0.0f, 1.0f, 0.0f},
    .specular_color = veekay::vec3{0.3f, 0.7f, 0.3f},
    .shininess = 128.0f
});

models.emplace_back(Model{
    .mesh = cube_mesh,
    .transform = Transform{
        .position = {0.0f, -0.5f, 1.0f},
    },
    .albedo_color = veekay::vec3{0.0f, 0.0f, 1.0f},
    .specular_color = veekay::vec3{0.3f, 0.3f, 0.7f},
    .shininess = 256.0f
});
}

// NOTE: Destroy resources here, do not cause leaks in your program!
void shutdown() {
	VkDevice& device = veekay::app.vk_device;

	vkDestroySampler(device, missing_texture_sampler, nullptr);
	delete missing_texture;

	delete cube_mesh.index_buffer;
	delete cube_mesh.vertex_buffer;

	delete plane_mesh.index_buffer;
	delete plane_mesh.vertex_buffer;

	delete model_uniforms_buffer;
	delete scene_uniforms_buffer;

	delete point_lights_buffer;
	delete spotlight_buffer;  // НОВОЕ: удаляем буфер прожектора

	vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
    ImGui::Begin("Lighting Controls");
    
    // Основные переключатели с уникальными идентификаторами
    ImGui::Checkbox("Ambient Light##ambient", &lighting_controls.ambient_enabled);
    ImGui::Checkbox("Directional Light##directional", &lighting_controls.directional_enabled);
    ImGui::Checkbox("Point Lights##point", &lighting_controls.point_lights_enabled);
    ImGui::Checkbox("Spotlight##spot", &lighting_controls.spotlight_enabled);
    
    ImGui::Separator();
    
    // Управление камерой
    ImGui::Text("Camera Controls:");
    
    // Переключатель режима камеры
    bool previous_lookat_mode = camera.lookat_mode;
    ImGui::Checkbox("Look-At Mode##camera_mode", &camera.lookat_mode);
    
    // Если режим изменился, сохраняем состояние
    if (previous_lookat_mode != camera.lookat_mode) {
        if (camera.lookat_mode) {
            camera.save_state();
            camera.target = {0.0f, 0.0f, 0.0f};
        } else {
            camera.restore_state();
        }
    }
    
    // В Look-At режиме показываем дополнительные контролы
    if (camera.lookat_mode) {
        ImGui::SliderFloat3("Camera Position##cam_pos", &camera.position.x, -10.0f, 10.0f);
        ImGui::SliderFloat3("Look-At Target##cam_target", &camera.target.x, -10.0f, 10.0f);
        ImGui::SliderFloat3("Up Vector##cam_up", &camera.up.x, -1.0f, 1.0f);
        
        if (ImGui::Button("Reset Camera##cam_reset")) {
            camera.position = {0.0f, 0.0f, -5.0f};
            camera.target = {0.0f, 0.0f, 0.0f};
            camera.up = {0.0f, 1.0f, 0.0f};
        }
    }
    
    ImGui::Separator();
    
    // Управление направленным светом с уникальными идентификаторами
    if (lighting_controls.directional_enabled) {
        ImGui::Text("Directional Light:");
        ImGui::SliderFloat("Intensity##dir_intensity", &lighting_controls.directional_intensity, 0.0f, 2.0f);
        ImGui::ColorEdit3("Color##dir_color", &lighting_controls.directional_color.x);
        ImGui::SliderFloat3("Direction##dir_direction", &lighting_controls.directional_direction.x, -1.0f, 1.0f);
    }
    
    // Управление рассеянным светом
    if (lighting_controls.ambient_enabled) {
        ImGui::Text("Ambient Light:");
        ImGui::ColorEdit3("Ambient Color##amb_color", &lighting_controls.ambient_color.x);
    }
    
    // Управление точечным источником
    if (lighting_controls.point_lights_enabled) {
        ImGui::Text("Point Light:");
        ImGui::Checkbox("Enabled##point_enabled", &lighting_controls.point_light_enabled);
        if (lighting_controls.point_light_enabled) {
            ImGui::SliderFloat("Intensity##point_intensity", &lighting_controls.point_intensity, 0.0f, 3.0f);
            ImGui::ColorEdit3("Color##point_color", &lighting_controls.point_color.x);
            ImGui::SliderFloat3("Position##point_pos", &lighting_controls.point_position.x, -5.0f, 5.0f);
        }
    }
    
    // Управление прожектором
    if (lighting_controls.spotlight_enabled) {
        ImGui::Text("Spotlight:");
        ImGui::SliderFloat("Spot Intensity##spot_intensity", &lighting_controls.spotlight_intensity, 0.0f, 5.0f);
        ImGui::ColorEdit3("Spot Color##spot_color", &lighting_controls.spotlight_color.x);
        ImGui::SliderFloat3("Spot Position##spot_pos", &lighting_controls.spotlight_position.x, -5.0f, 5.0f);
        ImGui::SliderFloat3("Spot Direction##spot_dir", &lighting_controls.spotlight_direction.x, -1.0f, 1.0f);
        ImGui::SliderFloat("Spot Cutoff##spot_cutoff", &lighting_controls.spotlight_cutoff, 1.0f, 30.0f);
        ImGui::SliderFloat("Spot Outer Cutoff##spot_outer", &lighting_controls.spotlight_outer_cutoff, 5.0f, 45.0f);
    }
    
    ImGui::End();

    // (1)
    // Условие: управление работает ТОЛЬКО в FPS-режиме И когда курсор НЕ над окнами ImGui
    if (!camera.lookat_mode && !ImGui::IsWindowHovered()) {
        using namespace veekay::input;

        // Проверяем: если зажата ЛЕВАЯ кнопка мыши - тогда обрабатываем вращение камеры
        if (mouse::isButtonDown(mouse::Button::left)) {
            // Получаем насколько переместился курсор с прошлого кадра (deltaX, deltaY)
            auto move_delta = mouse::cursorDelta();

            // Yaw (горизонтальное вращение) - двигаем камеру ВЛЕВО/ВПРАВО
            // move_delta.x - изменение по горизонтали (чем быстрее двигаем мышь - больше значение)
            // 0.01f - чувствительность мыши (можно регулировать)
            camera.rotation.y -= move_delta.x * 0.01f;  // Yaw (horizontal)
            
            // Pitch (вертикальное вращение) - двигаем камеру ВВЕРХ/ВНИЗ  
            camera.rotation.x -= move_delta.y * 0.01f;  // Pitch (vertical)

            // Максимальный угол наклона вверх/вниз (89 градусов)
            const float maxPitch = toRadians(89.0f);
            if (camera.rotation.x > maxPitch) camera.rotation.x = maxPitch;
            if (camera.rotation.x < -maxPitch) camera.rotation.x = -maxPitch;

            // Нормализация Yaw угла (чтобы значение оставалось в диапазоне -PI до +PI)
            while (camera.rotation.y > float(M_PI)) camera.rotation.y -= 2.0f * float(M_PI);
            while (camera.rotation.y < -float(M_PI)) camera.rotation.y += 2.0f * float(M_PI);

            // Преобразуем углы Эйлера (pitch, yaw) в вектор направления "вперед"
            float yaw = camera.rotation.y;   // Горизонтальный угол
            float pitch = camera.rotation.x; // Вертикальный угол
            
            // Вычисляем вектор направления камеры
            veekay::vec3 front = {
                cos(yaw) * cos(pitch),
                sin(pitch),
                sin(yaw) * cos(pitch)
            };
            
            front = veekay::vec3::normalized(front);
            // Вектор "вверх" в мировом пространстве
            veekay::vec3 world_up = {0.0f, 1.0f, 0.0f};
            // Вектор "вправо" - перпендикулярен направлению взгляда и вектору "вверх"
            veekay::vec3 right = veekay::vec3::normalized(veekay::vec3::cross(front, world_up));
            // Пересчитываем "локальный вверх" камеры
            veekay::vec3 up = veekay::vec3::cross(right, front);

            float move_speed = 0.1f;
            
            if (keyboard::isKeyDown(keyboard::Key::w))
                camera.position -= front * move_speed;

            if (keyboard::isKeyDown(keyboard::Key::s))
                camera.position += front * move_speed;

            if (keyboard::isKeyDown(keyboard::Key::d))
                camera.position += right * move_speed;

            if (keyboard::isKeyDown(keyboard::Key::a))
                camera.position -= right * move_speed;
        }
    }

    // Обновление uniform буферов (остается без изменений)
    float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);
    SceneUniforms scene_uniforms{
        .view_projection = camera.view_projection(aspect_ratio),
        .ambient_light = lighting_controls.ambient_color,
        .directional_light_direction = veekay::vec3::normalized(lighting_controls.directional_direction),
        .directional_light_color = lighting_controls.directional_color * lighting_controls.directional_intensity,
        .directional_light_enabled = lighting_controls.directional_enabled ? 1.0f : 0.0f,
        .camera_position = camera.position,
        .ambient_light_enabled = lighting_controls.ambient_enabled ? 1.0f : 0.0f,
        .point_lights_enabled = lighting_controls.point_lights_enabled ? 1.0f : 0.0f,
        .spotlight_enabled = lighting_controls.spotlight_enabled ? 1.0f : 0.0f,
    };

    // Обновление одного центрального источника света
    point_lights[0].position = lighting_controls.point_position;
    point_lights[0].color = lighting_controls.point_color;
    point_lights[0].intensity = lighting_controls.point_intensity;
    point_lights[0].enabled = lighting_controls.point_light_enabled ? 1.0f : 0.0f;

    // НОВОЕ: Обновление прожектора
    spotlight.position = lighting_controls.spotlight_position;
    spotlight.direction = veekay::vec3::normalized(lighting_controls.spotlight_direction);
    spotlight.color = lighting_controls.spotlight_color;
    spotlight.intensity = lighting_controls.spotlight_intensity;
    spotlight.cutOff = cos(toRadians(lighting_controls.spotlight_cutoff));
    spotlight.outerCutOff = cos(toRadians(lighting_controls.spotlight_outer_cutoff));
    spotlight.enabled = lighting_controls.spotlight_enabled ? 1.0f : 0.0f;


	std::vector<ModelUniforms> model_uniforms(models.size());
	for (size_t i = 0, n = models.size(); i < n; ++i) {
	    const Model& model = models[i];
	    ModelUniforms& uniforms = model_uniforms[i];

	    uniforms.model = model.transform.matrix();
	    uniforms.albedo_color = model.albedo_color;
	    // НОВОЕ: передаем specular параметры
	    uniforms.specular_color = model.specular_color;
	    uniforms.shininess = model.shininess;
	}

	*(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;
    std::copy(model_uniforms.begin(),
              model_uniforms.end(),
              static_cast<ModelUniforms*>(model_uniforms_buffer->mapped_region));

    std::copy(point_lights.begin(),
              point_lights.end(),
              static_cast<PointLight*>(point_lights_buffer->mapped_region));

    // НОВОЕ: Копируем данные прожектора
    *(Spotlight*)spotlight_buffer->mapped_region = spotlight;

	 VkMappedMemoryRange memory_ranges[4] = {};
    
    memory_ranges[0] = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = scene_uniforms_buffer->memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };
    
    memory_ranges[1] = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 
        .memory = model_uniforms_buffer->memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };
    
    memory_ranges[2] = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = point_lights_buffer->memory, 
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };
    
    // НОВОЕ: синхронизация буфера прожектора
    memory_ranges[3] = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = spotlight_buffer->memory, 
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };
    
    vkFlushMappedMemoryRanges(veekay::app.vk_device, 4, memory_ranges);
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
	vkResetCommandBuffer(cmd, 0);

	{ // NOTE: Start recording rendering commands
		VkCommandBufferBeginInfo info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		vkBeginCommandBuffer(cmd, &info);
	}

	{ // NOTE: Use current swapchain framebuffer and clear it
		VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
		VkClearValue clear_depth{.depthStencil = {1.0f, 0}};

		VkClearValue clear_values[] = {clear_color, clear_depth};

		VkRenderPassBeginInfo info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = veekay::app.vk_render_pass,
			.framebuffer = framebuffer,
			.renderArea = {
				.extent = {
					veekay::app.window_width,
					veekay::app.window_height
				},
			},
			.clearValueCount = 2,
			.pClearValues = clear_values,
		};

		vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	VkDeviceSize zero_offset = 0;

	VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
	VkBuffer current_index_buffer = VK_NULL_HANDLE;

	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		const Mesh& mesh = model.mesh;

		if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
			current_vertex_buffer = mesh.vertex_buffer->buffer;
			vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
		}

		if (current_index_buffer != mesh.index_buffer->buffer) {
			current_index_buffer = mesh.index_buffer->buffer;
			vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
		}

		uint32_t offset = i * sizeof(ModelUniforms);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
		                    0, 1, &descriptor_set, 1, &offset);

		vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

} // namespace

int main() {
	return veekay::run({
		.init = initialize,
		.shutdown = shutdown,
		.update = update,
		.render = render,
	});
}