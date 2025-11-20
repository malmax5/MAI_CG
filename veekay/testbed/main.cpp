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

#include <lodepng.h>  // ПУНКТ 1: Библиотека для загрузки PNG файлов

namespace {

constexpr uint32_t max_models = 1024;

struct Vertex {
	veekay::vec3 position;
	veekay::vec3 normal;
	veekay::vec2 uv;  // ПУНКТ 5: Вершины содержат текстурные координаты
};

struct SceneUniforms {
    veekay::mat4 view_projection;
    veekay::vec3 ambient_light; float _pad0;
    veekay::vec3 directional_light_direction; float _pad1;
    veekay::vec3 directional_light_color; 
    float directional_light_enabled;
    veekay::vec3 camera_position; 
    float time; // НОВОЕ: время для анимации
    float ambient_light_enabled;
    float point_lights_enabled;
    float spotlight_enabled;
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

struct Spotlight {
    veekay::vec3 position; float _pad0;
    veekay::vec3 direction; float _pad1;
    veekay::vec3 color; float _pad2;
    float intensity;
    float cutOff;
    float outerCutOff;
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

	veekay::mat4 matrix() const;
};

// ОБНОВЛЕННАЯ СТРУКТУРА: Материал с тремя текстурами
struct Material {
    VkDescriptorSet texture_descriptor_set;
    veekay::graphics::Texture* albedo_texture;
    veekay::graphics::Texture* specular_texture;
    veekay::graphics::Texture* emissive_texture;
    VkSampler sampler;
};

struct Model {
    Mesh mesh;
    Transform transform;
    veekay::vec3 albedo_color;
    veekay::vec3 specular_color;
    float shininess;
    Material* material;
};

struct Camera {
    constexpr static float default_fov = 60.0f;
    constexpr static float default_near_plane = 0.01f;
    constexpr static float default_far_plane = 100.0f;

    veekay::vec3 position = {};
    veekay::vec3 rotation = {};
    veekay::vec3 target = {0.0f, 0.0f, 0.0f};
    veekay::vec3 up = {0.0f, 1.0f, 0.0f};

    veekay::vec3 saved_position = {};
    veekay::vec3 saved_rotation = {};

    float fov = default_fov;
    float near_plane = default_near_plane;
    float far_plane = default_far_plane;

    veekay::mat4 view() const;
    veekay::mat4 view_projection(float aspect_ratio) const;
    
    void save_state() {
        saved_position = position;
        saved_rotation = rotation;
    }
    
    void restore_state() {
        position = saved_position;
        rotation = saved_rotation;
    }
};

inline namespace {
	Camera camera{
		.position = {0.0f, -0.5f, -3.0f}
	};

	std::vector<Model> models;
}

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

	// НОВОЕ: Текстуры для разных каналов
	veekay::graphics::Texture* ground_albedo = nullptr;
	veekay::graphics::Texture* brick_albedo = nullptr;
	veekay::graphics::Texture* wood_albedo = nullptr;
	veekay::graphics::Texture* metal_albedo = nullptr;
	
	// Specular текстуры
	veekay::graphics::Texture* ground_specular = nullptr;
	veekay::graphics::Texture* brick_specular = nullptr;
	veekay::graphics::Texture* wood_specular = nullptr;
	veekay::graphics::Texture* metal_specular = nullptr;
	
	// Emissive текстуры
	veekay::graphics::Texture* ground_emissive = nullptr;
	veekay::graphics::Texture* brick_emissive = nullptr;
	veekay::graphics::Texture* wood_emissive = nullptr;
	veekay::graphics::Texture* metal_emissive = nullptr;

	constexpr uint32_t max_point_lights = 16;
	veekay::graphics::Buffer* point_lights_buffer;
	veekay::graphics::Buffer* spotlight_buffer;

	std::vector<PointLight> point_lights;
	Spotlight spotlight;

	// Материалы
	std::vector<Material> materials;
	VkDescriptorSetLayout material_descriptor_set_layout;
	VkDescriptorPool material_descriptor_pool;
	VkSampler common_sampler;
}

struct LightingControls {
    bool ambient_enabled = true;
    bool directional_enabled = true;
    bool point_lights_enabled = true;
    bool spotlight_enabled = true;

    bool point_light_enabled = true;
    float point_intensity = 2.0f;
    veekay::vec3 point_color = {1.0f, 1.0f, 1.0f};
    veekay::vec3 point_position = {0.0f, 3.0f, 0.0f};

    float spotlight_intensity = 3.0f;
    veekay::vec3 spotlight_color = {1.0f, 1.0f, 0.8f};
    veekay::vec3 spotlight_position = {0.0f, 2.0f, 2.0f};
    veekay::vec3 spotlight_direction = {0.0f, -0.5f, -1.0f};
    float spotlight_cutoff = 12.5f;
    float spotlight_outer_cutoff = 17.5f;

    float directional_intensity = 1.0f;
    veekay::vec3 directional_color = {0.8f, 0.8f, 0.6f};
    veekay::vec3 directional_direction = {0.5f, -1.0f, 0.5f};

    veekay::vec3 ambient_color = {0.1f, 0.1f, 0.1f};
};

LightingControls lighting_controls;

float toRadians(float degrees) {
	return degrees * float(M_PI) / 180.0f;
}

veekay::mat4 Transform::matrix() const {
    veekay::mat4 translation_matrix = veekay::mat4::translation(position);
    veekay::mat4 rotation_x = veekay::mat4::rotation({1.0f, 0.0f, 0.0f}, rotation.x);
    veekay::mat4 rotation_y = veekay::mat4::rotation({0.0f, 1.0f, 0.0f}, rotation.y);
    veekay::mat4 rotation_z = veekay::mat4::rotation({0.0f, 0.0f, 1.0f}, rotation.z);
    veekay::mat4 rotation_matrix = rotation_z * rotation_y * rotation_x;
    veekay::mat4 scale_matrix = veekay::mat4::scaling(scale);
    return translation_matrix * rotation_matrix * scale_matrix;
}

veekay::mat4 Camera::view() const {
    float yaw = rotation.y;
    float pitch = rotation.x;
    
    veekay::vec3 front = {
        cos(yaw) * cos(pitch),
        sin(pitch),
        sin(yaw) * cos(pitch)
    };
    
    front = veekay::vec3::normalized(front);
    veekay::vec3 world_up = {0.0f, 1.0f, 0.0f};
    veekay::vec3 right = veekay::vec3::normalized(veekay::vec3::cross(front, world_up));
    veekay::vec3 up = veekay::vec3::cross(right, front);
    
    veekay::mat4 view_matrix = veekay::mat4::identity();
    view_matrix[0] = veekay::vec4({right.x, up.x, -front.x, 0.0f});
    view_matrix[1] = veekay::vec4({right.y, up.y, -front.y, 0.0f});
    view_matrix[2] = veekay::vec4({right.z, up.z, -front.z, 0.0f});
    view_matrix[3] = veekay::vec4({
        -veekay::vec3::dot(right, position),
        -veekay::vec3::dot(up, position),
        veekay::vec3::dot(front, position),
        1.0f
    });
    
    return view_matrix;
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
    auto projection = veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane);
    return view() * projection;
}

// ПУНКТ 1: Функция загрузки текстуры из файла
// ПУНКТ 2: Cоздание текстур через veekay::graphics::Texture
veekay::graphics::Texture* loadTextureFromFile(VkCommandBuffer cmd, const char* filename) {
    std::vector<unsigned char> image;
    unsigned width, height;
    
    unsigned error = lodepng::decode(image, width, height, filename);
    if (error) {
        std::cerr << "Failed to load texture '" << filename << "': " << lodepng_error_text(error) << std::endl;
        return nullptr;
    }
    
    std::cout << "Loaded texture: " << filename << " (" << width << "x" << height << ")" << std::endl;
    
    try {
        return new veekay::graphics::Texture(cmd, width, height, 
                                           VK_FORMAT_R8G8B8A8_UNORM, 
                                           image.data());
    } catch (const std::exception& e) {
        std::cerr << "Failed to create texture from '" << filename << "': " << e.what() << std::endl;
        return nullptr;
    }
}

// НОВАЯ ФУНКЦИЯ: Создание простой текстуры программно
veekay::graphics::Texture* createSolidColorTexture(VkCommandBuffer cmd, uint32_t color, int width = 2, int height = 2) {
    std::vector<uint32_t> pixels(width * height, color);
    return new veekay::graphics::Texture(cmd, width, height, VK_FORMAT_B8G8R8A8_UNORM, pixels.data());
}

// ПУНКТ 3: Создание сэмплера с разумными параметрами
VkSampler createTextureSampler() {
    VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 12.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };

    VkSampler sampler;
    if (vkCreateSampler(veekay::app.vk_device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan texture sampler\n";
        return VK_NULL_HANDLE;
    }
    return sampler;
}

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
	if (vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &result) != VK_SUCCESS) {
		return nullptr;
	}

	return result;
}

void initialize(VkCommandBuffer cmd) {
	VkDevice& device = veekay::app.vk_device;
	VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

	{ // Build graphics pipeline
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

		stage_infos[0] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_shader_module,
			.pName = "main",
		};

		stage_infos[1] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_shader_module,
			.pName = "main",
		};

		VkVertexInputBindingDescription buffer_binding{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		VkVertexInputAttributeDescription attributes[] = {
			{
				.location = 0,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = offsetof(Vertex, position),
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

		VkPipelineVertexInputStateCreateInfo input_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &buffer_binding,
			.vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
			.pVertexAttributeDescriptions = attributes,
		};

		VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		VkPipelineRasterizationStateCreateInfo raster_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_BACK_BIT,
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1.0f,
		};

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

		VkPipelineViewportStateCreateInfo viewport_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.pViewports = &viewport,
			.scissorCount = 1,
			.pScissors = &scissor,
		};

		VkPipelineDepthStencilStateCreateInfo depth_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = true,
			.depthWriteEnable = true,
			.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		};

		VkPipelineColorBlendAttachmentState attachment_info{
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			                  VK_COLOR_COMPONENT_G_BIT |
			                  VK_COLOR_COMPONENT_B_BIT |
			                  VK_COLOR_COMPONENT_A_BIT,
		};

		VkPipelineColorBlendStateCreateInfo blend_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.logicOpEnable = false,
			.logicOp = VK_LOGIC_OP_COPY,
			.attachmentCount = 1,
			.pAttachments = &attachment_info
		};

		{ // Descriptor pool for main uniform buffers
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
    		    },
    		};
			
			VkDescriptorPoolCreateInfo info{
    		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    		    .maxSets = 2,
    		    .poolSizeCount = sizeof(pools) / sizeof(pools[0]),
    		    .pPoolSizes = pools,
    		};

			if (vkCreateDescriptorPool(device, &info, nullptr, &descriptor_pool) != VK_SUCCESS) {
    		    std::cerr << "Failed to create Vulkan descriptor pool\n";
    		    veekay::app.running = false;
    		    return;
    		}
		}

		{ // Descriptor set layout for main uniform buffers (set = 0)
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
    		    {
    		        .binding = 3,
    		        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    		        .descriptorCount = 1,
    		        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    		    },
    		};

			VkDescriptorSetLayoutCreateInfo info{
    		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    		    .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
    		    .pBindings = bindings,
    		};
		
    		if (vkCreateDescriptorSetLayout(device, &info, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
    		    std::cerr << "Failed to create Vulkan descriptor set layout\n";
    		    veekay::app.running = false;
    		    return;
    		}
		}

		{ // Allocate main descriptor set
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

		// ПУНКТ 7: Создание layout для наборов дескрипторов материалов
		{
			VkDescriptorSetLayoutBinding material_bindings[3] = {
				{
					.binding = 0,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 2,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				}
			};

			VkDescriptorSetLayoutCreateInfo material_layout_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 3,
				.pBindings = material_bindings,
			};

			if (vkCreateDescriptorSetLayout(device, &material_layout_info, nullptr, 
											&material_descriptor_set_layout) != VK_SUCCESS) {
				std::cerr << "Failed to create material descriptor set layout\n";
				veekay::app.running = false;
				return;
			}
		}

		// ОБНОВЛЕННЫЙ ПУЛ: больше дескрипторов для трех текстур на материал
		{
			VkDescriptorPoolSize material_pool_size = {
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 10 * 3, // 10 материалов × 3 текстуры
			};

			VkDescriptorPoolCreateInfo material_pool_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 10,
				.poolSizeCount = 1,
				.pPoolSizes = &material_pool_size,
			};

			if (vkCreateDescriptorPool(device, &material_pool_info, nullptr, 
								   &material_descriptor_pool) != VK_SUCCESS) {
				std::cerr << "Failed to create material descriptor pool\n";
				veekay::app.running = false;
				return;
			}
		}

		// Pipeline layout с двумя наборами дескрипторов
		{
			std::vector<VkDescriptorSetLayout> set_layouts = {
				descriptor_set_layout,        // set = 0: uniform buffers
				material_descriptor_set_layout // set = 1: textures
			};

			VkPipelineLayoutCreateInfo layout_info{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
				.pSetLayouts = set_layouts.data(),
			};

			if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan pipeline layout\n";
				veekay::app.running = false;
				return;
			}
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

		if (vkCreateGraphicsPipelines(device, nullptr, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline\n";
			veekay::app.running = false;
			return;
		}
	}

	// Создание uniform буферов
	scene_uniforms_buffer = new veekay::graphics::Buffer(
		sizeof(SceneUniforms), nullptr, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	model_uniforms_buffer = new veekay::graphics::Buffer(
		max_models * sizeof(ModelUniforms), nullptr, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	
	point_lights_buffer = new veekay::graphics::Buffer(
    	max_point_lights * sizeof(PointLight), nullptr, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	
	spotlight_buffer = new veekay::graphics::Buffer(sizeof(Spotlight), nullptr, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	
	PointLight initial_light = {};
	std::fill_n(static_cast<PointLight*>(point_lights_buffer->mapped_region), max_point_lights, initial_light);
	
	point_lights.push_back(PointLight{
	    .position = lighting_controls.point_position,
	    .color = lighting_controls.point_color,
	    .intensity = lighting_controls.point_intensity,
	    .constant = 1.0f,
	    .linear = 0.09f,
	    .quadratic = 0.032f,
	    .enabled = lighting_controls.point_light_enabled ? 1.0f : 0.0f
	});

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

	// Создание fallback текстуры
	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.mipLodBias = 0.0f,
			.anisotropyEnable = VK_FALSE,
			.maxAnisotropy = 1.0f,
			.compareEnable = VK_FALSE,
			.compareOp = VK_COMPARE_OP_ALWAYS,
			.minLod = 0.0f,
			.maxLod = 0.0f,
			.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
			.unnormalizedCoordinates = VK_FALSE,
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

		missing_texture = new veekay::graphics::Texture(cmd, 2, 2, VK_FORMAT_B8G8R8A8_UNORM, pixels);
	}

	// ПУНКТ 3: Создание общего сэмплера
	common_sampler = createTextureSampler();
	if (!common_sampler) {
		std::cerr << "Failed to create common texture sampler\n";
		veekay::app.running = false;
		return;
	}

	// Загрузка albedo текстур
	ground_albedo = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Ground.png");
	brick_albedo = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Brick.png");
	// wood_albedo = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Wood.png");
	wood_albedo = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Wood.png");

	// ПУНКТ 9: Загрузка дополнительных текстур (specular и emissive)
	// Загрузка specular текстур (можно использовать реальные или создать искусственные)
	ground_specular = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Ground_Specular.png");
	if (!ground_specular) {
		// Создаем простую specular карту если файл не найден
		ground_specular = createSolidColorTexture(cmd, 0xFF202020); // Темно-серая
	}
	
	brick_specular = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Brick_Specular.png");
	if (!brick_specular) {
		brick_specular = createSolidColorTexture(cmd, 0xFF404040); // Средне-серая
	}
	
	wood_specular = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Wood_Specular.png");
	if (!wood_specular) {
		wood_specular = createSolidColorTexture(cmd, 0xFF606060); // Светло-серая
	}

	// Загрузка emissive текстур
	ground_emissive = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Ground_Emissive.png");
	if (!ground_emissive) {
		// Создаем черную текстуру по умолчанию
		ground_emissive = createSolidColorTexture(cmd, 0xFF000000); // Черная
	}
	
	brick_emissive = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Brick_Emissive.png");
	if (!brick_emissive) {
		// Создаем emissive текстуру с красными пятнами для демонстрации
		uint32_t emissive_pixels[] = {
			0xFF000000, 0xFFFF0000,
			0xFF000000, 0xFF000000,
		};
		brick_emissive = new veekay::graphics::Texture(cmd, 2, 2, VK_FORMAT_B8G8R8A8_UNORM, emissive_pixels);
	}
	
	wood_emissive = loadTextureFromFile(cmd, "/home/cbf/MAI/Sem_5/CG/Lab3/veekay/texture/Texture_Wood_Emissive.png");
	if (!wood_emissive) {
		// Создаем emissive текстуру с синими пятнами
		uint32_t emissive_pixels[] = {
			0xFF000000, 0xFF0000FF,
			0xFF0000FF, 0xFF000000,
		};
		wood_emissive = new veekay::graphics::Texture(cmd, 2, 2, VK_FORMAT_B8G8R8A8_UNORM, emissive_pixels);
	}

	// Fallback если основные текстуры не загрузились
	if (!ground_albedo) ground_albedo = missing_texture;
	if (!brick_albedo) brick_albedo = missing_texture;
	if (!wood_albedo) wood_albedo = missing_texture;
	
	// Material 0: Ground (трава/земля)
	{
	    uint32_t ground_spec_pixels[16] = {
	    	0xFF101010, 0xFF151515, 0xFF101010, 0xFF202020,
	    	0xFF151515, 0xFF101010, 0xFF252525, 0xFF101010, 
	    	0xFF101010, 0xFF303030, 0xFF101010, 0xFF151515,
	    	0xFF202020, 0xFF101010, 0xFF151515, 0xFF101010
		};
		ground_specular = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, ground_spec_pixels);

		// Emissive: несколько светящихся грибов/камней
		uint32_t ground_emissive_pixels[16] = {
		    0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
		    0xFF000000, 0xFF404000, 0xFF000000, 0xFF000000,
		    0xFF000000, 0xFF000000, 0xFF603000, 0xFF000000,
		    0xFF000000, 0xFF000000, 0xFF000000, 0xFF304000
		};
		ground_emissive = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, ground_emissive_pixels);
	}

	// Material 1: Brick (кирпич)
	{
		// Specular: кирпичи слабо отражают
		uint32_t brick_spec_pixels[16] = {
		    0xFF404040, 0xFF101010, 0xFF404040, 0xFF101010,
		    0xFF101010, 0xFF404040, 0xFF101010, 0xFF404040,
		    0xFF404040, 0xFF101010, 0xFF404040, 0xFF101010,
		    0xFF101010, 0xFF404040, 0xFF101010, 0xFF404040
		};
		brick_specular = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, brick_spec_pixels);

		// Emissive: лава в трещинах
		uint32_t brick_emissive_pixels[16] = {
		    0xFF000000, 0xFFFF2000, 0xFF000000, 0xFFFF2000,
		    0xFFFF2000, 0xFF000000, 0xFFFF4000, 0xFF000000,
		    0xFF000000, 0xFFFF2000, 0xFF000000, 0xFFFF2000,
		    0xFFFF2000, 0xFF000000, 0xFFFF2000, 0xFF000000
		};
		brick_emissive = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, brick_emissive_pixels);
	}

	// Material 2: Wood (дерево)
	{
	    // Specular
		uint32_t wood_spec_pixels[16] = {
		    0xFF202020, 0xFF606060, 0xFF202020, 0xFF606060,
		    0xFF606060, 0xFF202020, 0xFF606060, 0xFF202020,
		    0xFF202020, 0xFF606060, 0xFF202020, 0xFF606060,
		    0xFF606060, 0xFF202020, 0xFF606060, 0xFF202020
		};
		wood_specular = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, wood_spec_pixels);

		// Emissivе 0xFFFF8000
		uint32_t wood_emissive_pixels[16] = {
		    0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF,
		    0xFFFF8000, 0xFFFF8000, 0xFFFF8000, 0xFFFF8000,
		    0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF,
		    0xFFFF8000, 0xFFFF8000, 0xFFFF8000, 0xFFFF8000
		};
		wood_emissive = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, wood_emissive_pixels);
	}

// Material 3: Metal (металл) - бело-металлический вариант
{
    // Albedo: яркий бело-серебристый металл с легкой текстурой
    uint32_t metal_albedo_pixels[] = {
        0xFFE8E8E8, 0xFFF0F0F0, 0xFFE8E8E8, 0xFFF0F0F0,
        0xFFF0F0F0, 0xFFE8E8E8, 0xFFF0F0F0, 0xFFE8E8E8,
        0xFFE8E8E8, 0xFFF0F0F0, 0xFFE8E8E8, 0xFFF0F0F0, 
        0xFFF0F0F0, 0xFFE8E8E8, 0xFFF0F0F0, 0xFFE8E8E8
    };
    metal_albedo = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, metal_albedo_pixels);

    // Specular: ОЧЕНЬ яркая с шероховатостями для металлического блеска
    uint32_t metal_spec_pixels[16] = {
        0xFFFFFFFF, 0xFFF8F8F8, 0xFFFFFFFF, 0xFFF0F0F0,
        0xFFF8F8F8, 0xFFFFFFFF, 0xFFF0F0F0, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFF0F0F0, 0xFFFFFFFF, 0xFFF8F8F8,
        0xFFF0F0F0, 0xFFFFFFFF, 0xFFF8F8F8, 0xFFFFFFFF
    };
    metal_specular = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, metal_spec_pixels);

    // Emissive: СЛАБОЕ голубое свечение для холодного металлического эффекта 0xFF000080
    uint32_t metal_emissive_pixels[16] = {
        0xFF101020, 0xFF080818, 0xFF101020, 0xFF080818,
        0xFF080818, 0xFF101020, 0xFF080818, 0xFF101020,
        0xFF101020, 0xFF080818, 0xFF101020, 0xFF080818,
        0xFF080818, 0xFF101020, 0xFF080818, 0xFF101020
    };
    metal_emissive = new veekay::graphics::Texture(cmd, 4, 4, VK_FORMAT_B8G8R8A8_UNORM, metal_emissive_pixels);
}

	// СОЗДАНИЕ МАТЕРИАЛОВ С ТРЕМЯ ТЕКСТУРАМИ
	std::vector<veekay::graphics::Texture*> albedo_textures = {ground_albedo, brick_albedo, wood_albedo, metal_albedo};
	std::vector<veekay::graphics::Texture*> specular_textures = {ground_specular, brick_specular, wood_specular, metal_specular};
	std::vector<veekay::graphics::Texture*> emissive_textures = {ground_emissive, brick_emissive, wood_emissive, metal_emissive};

	// ПУНКТ 4: Записать в набор дескрипторов новую привязку (дескриптор – изображение+сэмплер)
	// ПУНКТ 7: Система материалов с разными наборами дескрипторов, привязываемых в рендеринге.
	for (size_t i = 0; i < albedo_textures.size(); i++) {
		VkDescriptorSetAllocateInfo alloc_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = material_descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &material_descriptor_set_layout,
		};

		VkDescriptorSet descriptor_set;
		if (vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set) != VK_SUCCESS) {
			std::cerr << "Failed to allocate material descriptor set\n";
			continue;
		}

		VkDescriptorImageInfo albedo_image_info = {
			.sampler = common_sampler,
			.imageView = albedo_textures[i]->view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		VkDescriptorImageInfo specular_image_info = {
			.sampler = common_sampler,
			.imageView = specular_textures[i]->view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		VkDescriptorImageInfo emissive_image_info = {
			.sampler = common_sampler,
			.imageView = emissive_textures[i]->view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		VkWriteDescriptorSet writes[3] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &albedo_image_info,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 1,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &specular_image_info,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 2,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &emissive_image_info,
			}
		};

		vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

		materials.push_back(Material{
			.texture_descriptor_set = descriptor_set,
			.albedo_texture = albedo_textures[i],
			.specular_texture = specular_textures[i],
			.emissive_texture = emissive_textures[i],
			.sampler = common_sampler
		});
	}

	// Обновление основного набора дескрипторов
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
	        {
	            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	            .dstSet = descriptor_set,
	            .dstBinding = 3,
	            .dstArrayElement = 0,
	            .descriptorCount = 1,
	            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	            .pBufferInfo = &buffer_infos[3],
	        },
	    };

	    vkUpdateDescriptorSets(device, sizeof(write_infos) / sizeof(write_infos[0]), write_infos, 0, nullptr);
	}

	// Инициализация plane mesh
	{
		std::vector<Vertex> vertices = {
			{{-5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
			{{5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
			{{5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
		};

		std::vector<uint32_t> indices = {0, 1, 2, 2, 3, 0};

		plane_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		plane_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		plane_mesh.indices = uint32_t(indices.size());
	}

	// Инициализация cube mesh
	{
		std::vector<Vertex> vertices = {
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        {{+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{+0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},

        {{+0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{+0.5f, -0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{+0.5f, +0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{+0.5f, +0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        {{+0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{+0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},

        {{-0.5f, -0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, +0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, +0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        {{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        {{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},

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
			vertices.size() * sizeof(Vertex), vertices.data(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		cube_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		cube_mesh.indices = uint32_t(indices.size());
	}

	// ПУНКТ 7: Создание моделей с разными материалами
	models.emplace_back(Model{
	    .mesh = plane_mesh,
	    .transform = Transform{},
	    .albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
	    .specular_color = veekay::vec3{1.0f, 1.0f, 1.0f}, // Белый для сильных бликов
	    .shininess = 32.0f,
	    .material = &materials[0]  // Ground material
	});
	
	models.emplace_back(Model{
	    .mesh = cube_mesh,
	    .transform = Transform{
	        .position = {-2.0f, -0.5f, -1.5f},
	    },
	    .albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
	    .specular_color = veekay::vec3{1.0f, 0.5f, 0.3f}, // Теплый цвет бликов
	    .shininess = 64.0f,
	    .material = &materials[1]  // Brick material
	});
	
	models.emplace_back(Model{
	    .mesh = cube_mesh,
	    .transform = Transform{
	        .position = {1.5f, -0.5f, -0.5f},
	    },
	    .albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
	    .specular_color = veekay::vec3{0.5f, 0.7f, 0.3f}, // Золотистые блики
	    .shininess = 128.0f,
	    .material = &materials[2]  // Wood material
	});
	
	models.emplace_back(Model{
	    .mesh = cube_mesh,
	    .transform = Transform{
	        .position = {0.0f, -0.5f, 1.0f},
	    },
	    .albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
	    .specular_color = veekay::vec3{0.8f, 0.8f, 1.0f}, // Холодные металлические блики
	    .shininess = 256.0f,
	    .material = &materials[3]  // Metal material
	});
}

void shutdown() {
	VkDevice& device = veekay::app.vk_device;

	// Освобождение ресурсов материалов
	for (auto& material : materials) {
		if (material.albedo_texture != missing_texture) {
			delete material.albedo_texture;
		}
		if (material.specular_texture != missing_texture && 
		    material.specular_texture != material.albedo_texture) {
			delete material.specular_texture;
		}
		if (material.emissive_texture != missing_texture && 
		    material.emissive_texture != material.albedo_texture) {
			delete material.emissive_texture;
		}
	}
	materials.clear();

	if (common_sampler) {
		vkDestroySampler(device, common_sampler, nullptr);
	}

	vkDestroyDescriptorPool(device, material_descriptor_pool, nullptr);
	vkDestroyDescriptorSetLayout(device, material_descriptor_set_layout, nullptr);

	vkDestroySampler(device, missing_texture_sampler, nullptr);
	delete missing_texture;

	delete cube_mesh.index_buffer;
	delete cube_mesh.vertex_buffer;

	delete plane_mesh.index_buffer;
	delete plane_mesh.vertex_buffer;

	delete model_uniforms_buffer;
	delete scene_uniforms_buffer;

	delete point_lights_buffer;
	delete spotlight_buffer;

	vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
    ImGui::Begin("Lighting Controls");
    
    ImGui::Checkbox("Ambient Light##ambient", &lighting_controls.ambient_enabled);
    ImGui::Checkbox("Directional Light##directional", &lighting_controls.directional_enabled);
    ImGui::Checkbox("Point Lights##point", &lighting_controls.point_lights_enabled);
    ImGui::Checkbox("Spotlight##spot", &lighting_controls.spotlight_enabled);
    
    ImGui::Separator();
    
    if (lighting_controls.directional_enabled) {
        ImGui::Text("Directional Light:");
        ImGui::SliderFloat("Intensity##dir_intensity", &lighting_controls.directional_intensity, 0.0f, 2.0f);
        ImGui::ColorEdit3("Color##dir_color", &lighting_controls.directional_color.x);
        ImGui::SliderFloat3("Direction##dir_direction", &lighting_controls.directional_direction.x, -1.0f, 1.0f);
    }
    
    if (lighting_controls.ambient_enabled) {
        ImGui::Text("Ambient Light:");
        ImGui::ColorEdit3("Ambient Color##amb_color", &lighting_controls.ambient_color.x);
    }
    
    if (lighting_controls.point_lights_enabled) {
        ImGui::Text("Point Light:");
        ImGui::Checkbox("Enabled##point_enabled", &lighting_controls.point_light_enabled);
        if (lighting_controls.point_light_enabled) {
            ImGui::SliderFloat("Intensity##point_intensity", &lighting_controls.point_intensity, 0.0f, 3.0f);
            ImGui::ColorEdit3("Color##point_color", &lighting_controls.point_color.x);
            ImGui::SliderFloat3("Position##point_pos", &lighting_controls.point_position.x, -5.0f, 5.0f);
        }
    }
    
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

    if (!ImGui::IsWindowHovered()) {
        using namespace veekay::input;

        if (mouse::isButtonDown(mouse::Button::left)) {
            auto move_delta = mouse::cursorDelta();

            camera.rotation.y -= move_delta.x * 0.01f;
            camera.rotation.x -= move_delta.y * 0.01f;

            const float maxPitch = toRadians(89.0f);
            if (camera.rotation.x > maxPitch) camera.rotation.x = maxPitch;
            if (camera.rotation.x < -maxPitch) camera.rotation.x = -maxPitch;

            while (camera.rotation.y > float(M_PI)) camera.rotation.y -= 2.0f * float(M_PI);
            while (camera.rotation.y < -float(M_PI)) camera.rotation.y += 2.0f * float(M_PI);

            float yaw = camera.rotation.y;
            float pitch = camera.rotation.x;
            
            veekay::vec3 front = {
                cos(yaw) * cos(pitch),
                sin(pitch),
                sin(yaw) * cos(pitch)
            };
            
            front = veekay::vec3::normalized(front);
            veekay::vec3 world_up = {0.0f, 1.0f, 0.0f};
            veekay::vec3 right = veekay::vec3::normalized(veekay::vec3::cross(front, world_up));
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

    float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);
    SceneUniforms scene_uniforms{
        .view_projection = camera.view_projection(aspect_ratio),
        .ambient_light = lighting_controls.ambient_color,
        .directional_light_direction = veekay::vec3::normalized(lighting_controls.directional_direction),
        .directional_light_color = lighting_controls.directional_color * lighting_controls.directional_intensity,
        .directional_light_enabled = lighting_controls.directional_enabled ? 1.0f : 0.0f,
        .camera_position = camera.position,
        .time = static_cast<float>(time),
        .ambient_light_enabled = lighting_controls.ambient_enabled ? 1.0f : 0.0f,
        .point_lights_enabled = lighting_controls.point_lights_enabled ? 1.0f : 0.0f,
        .spotlight_enabled = lighting_controls.spotlight_enabled ? 1.0f : 0.0f,
    };

    point_lights[0].position = lighting_controls.point_position;
    point_lights[0].color = lighting_controls.point_color;
    point_lights[0].intensity = lighting_controls.point_intensity;
    point_lights[0].enabled = lighting_controls.point_light_enabled ? 1.0f : 0.0f;

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
	    uniforms.specular_color = model.specular_color;
	    uniforms.shininess = model.shininess;
	}

	*(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;
    std::copy(model_uniforms.begin(), model_uniforms.end(),
              static_cast<ModelUniforms*>(model_uniforms_buffer->mapped_region));

    std::copy(point_lights.begin(), point_lights.end(),
              static_cast<PointLight*>(point_lights_buffer->mapped_region));

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

    { // Start recording rendering commands
        VkCommandBufferBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &info);
    }

    { // Use current swapchain framebuffer and clear it
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

        // Bind descriptor set for uniform buffers (set = 0)
        uint32_t offset = i * sizeof(ModelUniforms);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 1, &descriptor_set, 1, &offset);

        // ПУНКТ 7: Привязка набора дескрипторов для текстур материала
        if (model.material) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                    1, 1, &model.material->texture_descriptor_set, 0, nullptr);
        }

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