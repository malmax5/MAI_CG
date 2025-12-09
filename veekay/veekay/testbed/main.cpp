#include <cstdint>
#include <climits>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cmath>
#include <limits>

#include <veekay/veekay.hpp>

#include <vulkan/vulkan_core.h>
#include <imgui.h>
#include <lodepng.h>

namespace {

constexpr uint32_t max_models = 1024;
constexpr uint32_t max_materials = 16;

struct Vertex {
	veekay::vec3 position;
	veekay::vec3 normal;
	veekay::vec2 uv;
	// NOTE: You can add more attributes
};

struct SceneUniforms {
	veekay::mat4 view_projection;
	veekay::mat4 light_view_projection;
	veekay::vec3 camera_position; float time;
	veekay::vec3 light_position; float shadow_bias;
	veekay::vec3 light_direction; float shadow_strength;
	float shadow_map_texel_size;
	veekay::vec3 _pad0;
};

struct ModelUniforms {
	veekay::mat4 model;
	veekay::vec3 albedo_color; float _pad0;
	float shininess;
	float use_texture;
	float use_triplanar;
	float _pad1;
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

// Material struct containing textures for a model
struct Material {
	veekay::graphics::Texture* diffuse_texture;
	veekay::graphics::Texture* specular_texture;
	veekay::graphics::Texture* emissive_texture;
	VkSampler sampler;
	VkDescriptorSet descriptor_set;
	float shininess;
	bool use_texture;
	bool use_triplanar; // Нетривиальное сэмплирование: triplanar mapping
};

struct Model {
	Mesh mesh;
	Transform transform;
	veekay::vec3 albedo_color;
	uint32_t material_index;
};

struct Camera {
	constexpr static float default_fov = 60.0f;
	constexpr static float default_near_plane = 0.01f;
	constexpr static float default_far_plane = 100.0f;

	veekay::vec3 position = {};
	veekay::vec3 rotation = {};

	float fov = default_fov;
	float near_plane = default_near_plane;
	float far_plane = default_far_plane;

	// NOTE: View matrix of camera (inverse of a transform)
	veekay::mat4 view() const;

	// NOTE: View and projection composition
	veekay::mat4 view_projection(float aspect_ratio) const;
};

/*
(2)
текстура глубины
хранит значения глубины для сцены, рендеренной с точки зрения источника света.
она используется для определения, находится ли точка в тени или на свету.
*/
struct ShadowMapResources {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkExtent2D extent{};
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// NOTE: Scene objects
inline namespace {
	Camera camera{
		.position = {0.0f, -1.5f, -5.0f}
	};

	std::vector<Model> models;
	std::vector<Material> materials;
	
	veekay::vec3 light_direction_input = {2.0f, -3.0f, 2.0f};
	float shadow_bias = 0.0015f;
	float shadow_strength = 0.65f;
	float shadow_ortho_size = 12.0f;
	constexpr float shadow_camera_distance = 20.0f;
	constexpr float shadow_near_plane = 1.0f;
	constexpr float shadow_far_plane = 50.0f;
	const veekay::vec3 scene_focus_point = {0.0f, -0.5f, 0.0f};
	float scene_time = 0.0f;
}

// NOTE: Vulkan objects
inline namespace {
	VkShaderModule vertex_shader_module;
	VkShaderModule fragment_shader_module;
	VkShaderModule shadow_vertex_shader_module;
	VkShaderModule shadow_fragment_shader_module;

	VkDescriptorPool descriptor_pool;
	VkDescriptorSetLayout descriptor_set_layout;

	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkPipeline shadow_pipeline;

	veekay::graphics::Buffer* scene_uniforms_buffer;
	veekay::graphics::Buffer* model_uniforms_buffer;

	Mesh plane_mesh;
	Mesh cube_mesh;
	Mesh sphere_mesh;
	Mesh pyramid_mesh;  // Новая меш-пирамида

	// Missing/default textures
	veekay::graphics::Texture* missing_texture;
	veekay::graphics::Texture* white_texture;
	veekay::graphics::Texture* black_texture;
	VkSampler default_sampler;

	// Loaded textures - удаляем ненужные текстуры
	veekay::graphics::Texture* lenna_texture;
	veekay::graphics::Texture* lenna_specular;
	veekay::graphics::Texture* lenna_emissive;
	
	veekay::graphics::Texture* checker_texture;
	veekay::graphics::Texture* checker_specular;
	
	veekay::graphics::Texture* brick_texture;
	veekay::graphics::Texture* brick_specular;
	veekay::graphics::Texture* brick_emissive;

	// Different samplers for different effects
	VkSampler sampler_linear_repeat;
	VkSampler sampler_nearest_repeat;
	VkSampler sampler_linear_clamp;
	VkSampler sampler_linear_mirror;
	ShadowMapResources shadow_map;

    /*
        (3)
        PFN_vkCmdBeginRenderingKHR - тип указателя на функцию vkCmdBeginRenderingKHR для динамического рендеринга
            -> динамическая библиотека так сказать
        их загрузка в loadDynamicRenderingFunctions
        начало рендеринга - vkCmdBeginRenderingKHR_ptr(cmd, &rendering_info);
        завершение рендеринга - vkCmdEndRenderingKHR_ptr(cmd);
    */
	PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR_ptr = nullptr;
	PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR_ptr = nullptr;
}

float toRadians(float degrees) {
	return degrees * float(M_PI) / 180.0f;
}

veekay::mat4 Transform::matrix() const {
	auto t = veekay::mat4::translation(position);
	auto s = veekay::mat4::scaling(scale);
	
	auto rx = veekay::mat4::rotation({1.0f, 0.0f, 0.0f}, rotation.x);
	auto ry = veekay::mat4::rotation({0.0f, 1.0f, 0.0f}, rotation.y);
	auto rz = veekay::mat4::rotation({0.0f, 0.0f, 1.0f}, rotation.z);
	
	return s * rz * ry * rx * t;
}

veekay::mat4 Camera::view() const {
	auto rx = veekay::mat4::rotation({1.0f, 0.0f, 0.0f}, -rotation.x);
	auto ry = veekay::mat4::rotation({0.0f, 1.0f, 0.0f}, -rotation.y);
	auto t = veekay::mat4::translation(-position);

	return t * ry * rx;
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
	auto projection = veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane);

	return view() * projection;
}

veekay::vec3 normalizeOrFallback(const veekay::vec3& value, const veekay::vec3& fallback) {
	const float length_sq = veekay::vec3::squaredLength(value);
	if (length_sq < 1e-4f) {
		return veekay::vec3::normalized(fallback);
	}
	return veekay::vec3::normalized(value);
}

veekay::mat4 lookAtLH(const veekay::vec3& eye, const veekay::vec3& target, const veekay::vec3& up) {
	veekay::vec3 zaxis = veekay::vec3::normalized(target - eye);
	veekay::vec3 xaxis = veekay::vec3::normalized(veekay::vec3::cross(up, zaxis));
	veekay::vec3 yaxis = veekay::vec3::cross(zaxis, xaxis);

	veekay::mat4 result = veekay::mat4::identity();

	result[0][0] = xaxis.x;
	result[0][1] = yaxis.x;
	result[0][2] = zaxis.x;
	result[1][0] = xaxis.y;
	result[1][1] = yaxis.y;
	result[1][2] = zaxis.y;
	result[2][0] = xaxis.z;
	result[2][1] = yaxis.z;
	result[2][2] = zaxis.z;

	result[3][0] = -veekay::vec3::dot(xaxis, eye);
	result[3][1] = -veekay::vec3::dot(yaxis, eye);
	result[3][2] = -veekay::vec3::dot(zaxis, eye);
	result[3][3] = 1.0f;

	return result;
}

veekay::mat4 orthographicOffCenterLH(float left, float right,
                                     float bottom, float top,
                                     float near_plane, float far_plane) {
	veekay::mat4 result{};

	result[0][0] = 2.0f / (right - left);
	result[1][1] = 2.0f / (top - bottom);
	result[2][2] = 1.0f / (far_plane - near_plane);
	result[3][0] = (left + right) / (left - right);
	result[3][1] = (top + bottom) / (bottom - top);
	result[3][2] = near_plane / (near_plane - far_plane);
	result[3][3] = 1.0f;

	return result;
}

// Загрузка функций
bool loadDynamicRenderingFunctions() {
	VkDevice device = veekay::app.vk_device;
	vkCmdBeginRenderingKHR_ptr = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
		vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
	vkCmdEndRenderingKHR_ptr = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
		vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));

	return vkCmdBeginRenderingKHR_ptr && vkCmdEndRenderingKHR_ptr;
}

// (2) создание формата глубины
VkFormat findSupportedDepthFormat() {
	const VkFormat candidates[] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT,
	};

	for (VkFormat format : candidates) {
		VkFormatProperties props{};
		vkGetPhysicalDeviceFormatProperties(veekay::app.vk_physical_device, format, &props);
		if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			return format;
		}
	}

	return VK_FORMAT_UNDEFINED;
}

void destroyShadowMapResources(); // forward declaration

// (4) Создание сэмплера с поддержкой сравнения значений
VkSampler createShadowSampler() {
	VkSamplerCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.maxAnisotropy = 1.0f,
		.compareEnable = VK_TRUE,
		.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		.minLod = 0.0f,
		.maxLod = 1.0f,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
	};

	VkSampler sampler = VK_NULL_HANDLE;
	if (vkCreateSampler(veekay::app.vk_device, &info, nullptr, &sampler) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow sampler\n";
		return VK_NULL_HANDLE;
	}

	return sampler;
}

// (2) создание текстуры глубины
bool createShadowMapResources() {
	VkDevice device = veekay::app.vk_device;
	VkPhysicalDevice physical_device = veekay::app.vk_physical_device;

	shadow_map.extent = {2048, 2048};
	shadow_map.format = findSupportedDepthFormat();

	if (shadow_map.format == VK_FORMAT_UNDEFINED) {
		std::cerr << "Failed to find supported depth format for shadow map\n";
		destroyShadowMapResources();
		return false;
	}

	VkImageCreateInfo image_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = shadow_map.format,
		.extent = {shadow_map.extent.width, shadow_map.extent.height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	if (vkCreateImage(device, &image_info, nullptr, &shadow_map.image) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow map image\n";
		destroyShadowMapResources();
		return false;
	}

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(device, shadow_map.image, &requirements);

	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

	uint32_t index = UINT32_MAX;
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
		if ((requirements.memoryTypeBits & (1 << i)) &&
		    (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			index = i;
			break;
		}
	}

	if (index == UINT32_MAX) {
		std::cerr << "Failed to find device local memory for shadow map\n";
		destroyShadowMapResources();
		return false;
	}

	VkMemoryAllocateInfo alloc_info{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = index,
	};

	if (vkAllocateMemory(device, &alloc_info, nullptr, &shadow_map.memory) != VK_SUCCESS) {
		std::cerr << "Failed to allocate shadow map memory\n";
		destroyShadowMapResources();
		return false;
	}

	if (vkBindImageMemory(device, shadow_map.image, shadow_map.memory, 0) != VK_SUCCESS) {
		std::cerr << "Failed to bind shadow map memory\n";
		destroyShadowMapResources();
		return false;
	}

	VkImageViewCreateInfo view_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = shadow_map.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = shadow_map.format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	if (vkCreateImageView(device, &view_info, nullptr, &shadow_map.view) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow map view\n";
		destroyShadowMapResources();
		return false;
	}

	shadow_map.sampler = createShadowSampler();
	if (shadow_map.sampler == VK_NULL_HANDLE) {
		destroyShadowMapResources();
		return false;
	}

	shadow_map.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

void destroyShadowMapResources() {
	VkDevice device = veekay::app.vk_device;
	if (shadow_map.sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, shadow_map.sampler, nullptr);
		shadow_map.sampler = VK_NULL_HANDLE;
	}
	if (shadow_map.view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, shadow_map.view, nullptr);
		shadow_map.view = VK_NULL_HANDLE;
	}
	if (shadow_map.image != VK_NULL_HANDLE) {
		vkDestroyImage(device, shadow_map.image, nullptr);
		shadow_map.image = VK_NULL_HANDLE;
	}
	if (shadow_map.memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, shadow_map.memory, nullptr);
		shadow_map.memory = VK_NULL_HANDLE;
	}
	shadow_map.layout = VK_IMAGE_LAYOUT_UNDEFINED;
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

// Load PNG texture using lodepng
veekay::graphics::Texture* loadPngTexture(VkCommandBuffer cmd, const char* path) {
	std::vector<unsigned char> image;
	unsigned width, height;
	
	unsigned error = lodepng::decode(image, width, height, path);
	if (error) {
		std::cerr << "Failed to load PNG: " << path << " - " << lodepng_error_text(error) << "\n";
		return nullptr;
	}

	std::cout << "Loaded texture: " << path << " (" << width << "x" << height << ")\n";

	// Convert RGBA to BGRA for Vulkan
	for (size_t i = 0; i < image.size(); i += 4) {
		std::swap(image[i], image[i + 2]); // Swap R and B
	}

	return new veekay::graphics::Texture(cmd, width, height, VK_FORMAT_B8G8R8A8_UNORM, image.data());
}

// Generate procedural checker texture
veekay::graphics::Texture* generateCheckerTexture(VkCommandBuffer cmd, uint32_t size, 
                                                   uint32_t color1, uint32_t color2, uint32_t checker_size) {
	std::vector<uint32_t> pixels(size * size);
	
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			bool is_checker = ((x / checker_size) + (y / checker_size)) % 2 == 0;
			pixels[y * size + x] = is_checker ? color1 : color2;
		}
	}
	
	return new veekay::graphics::Texture(cmd, size, size, VK_FORMAT_B8G8R8A8_UNORM, pixels.data());
}

// Generate procedural brick texture
veekay::graphics::Texture* generateBrickTexture(VkCommandBuffer cmd, uint32_t size) {
	std::vector<uint32_t> pixels(size * size);
	
	uint32_t brick_color = 0xff3355aa;  // Brownish red in BGRA
	uint32_t mortar_color = 0xff888888; // Gray mortar
	
	uint32_t brick_width = size / 4;
	uint32_t brick_height = size / 8;
	uint32_t mortar_size = 4;
	
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			uint32_t row = y / brick_height;
			uint32_t offset = (row % 2) * (brick_width / 2);
			
			uint32_t local_x = (x + offset) % brick_width;
			uint32_t local_y = y % brick_height;
			
			bool is_mortar = local_x < mortar_size || local_y < mortar_size;
			
			// Add some variation to brick color
			float noise = (float)((x * 7 + y * 13) % 20) / 100.0f;
			uint32_t r = std::min(255u, (uint32_t)(((brick_color >> 16) & 0xff) * (1.0f + noise)));
			uint32_t g = std::min(255u, (uint32_t)(((brick_color >> 8) & 0xff) * (1.0f + noise)));
			uint32_t b = std::min(255u, (uint32_t)((brick_color & 0xff) * (1.0f + noise)));
			uint32_t varied_brick = 0xff000000 | (r << 16) | (g << 8) | b;
			
			pixels[y * size + x] = is_mortar ? mortar_color : varied_brick;
		}
	}
	
	return new veekay::graphics::Texture(cmd, size, size, VK_FORMAT_B8G8R8A8_UNORM, pixels.data());
}

// Generate specular map from diffuse (brighter areas = more specular)
veekay::graphics::Texture* generateSpecularFromDiffuse(VkCommandBuffer cmd, const char* path, float intensity) {
	std::vector<unsigned char> image;
	unsigned width, height;
	
	unsigned error = lodepng::decode(image, width, height, path);
	if (error) {
		return nullptr;
	}
	
	std::vector<uint32_t> specular(width * height);
	
	for (size_t i = 0; i < width * height; ++i) {
		// Calculate luminance
		float r = image[i * 4 + 0] / 255.0f;
		float g = image[i * 4 + 1] / 255.0f;
		float b = image[i * 4 + 2] / 255.0f;
		
		float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
		luminance = std::pow(luminance, 2.0f) * intensity; // Enhance bright areas
		luminance = std::min(1.0f, luminance);
		
		uint8_t spec = (uint8_t)(luminance * 255);
		specular[i] = 0xff000000 | (spec << 16) | (spec << 8) | spec;
	}
	
	return new veekay::graphics::Texture(cmd, width, height, VK_FORMAT_B8G8R8A8_UNORM, specular.data());
}

// Generate emissive map (glowing areas)
veekay::graphics::Texture* generateEmissiveTexture(VkCommandBuffer cmd, uint32_t size, uint32_t pattern) {
	std::vector<uint32_t> pixels(size * size);
	
	float cx = size / 2.0f;
	float cy = size / 2.0f;
	
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			float dx = (x - cx) / cx;
			float dy = (y - cy) / cy;
			float dist = std::sqrt(dx * dx + dy * dy);
			
			float glow = 0.0f;
			
			if (pattern == 0) {
				// Circular glow pattern
				glow = std::max(0.0f, 1.0f - dist * 1.5f);
				glow = std::pow(glow, 3.0f);
			} else if (pattern == 1) {
				// Grid pattern for sci-fi look
				bool grid_x = ((x / 16) % 4 == 0);
				bool grid_y = ((y / 16) % 4 == 0);
				glow = (grid_x || grid_y) ? 0.8f : 0.0f;
				glow *= std::max(0.0f, 1.0f - dist);
			} else {
				// Stripe pattern
				glow = std::sin((x + y) * 0.1f) > 0.7f ? 0.9f : 0.0f;
			}
			
			// Cyan/green emissive color
			uint8_t r = (uint8_t)(glow * 100);
			uint8_t g = (uint8_t)(glow * 255);
			uint8_t b = (uint8_t)(glow * 200);
			
			pixels[y * size + x] = 0xff000000 | (r << 16) | (g << 8) | b;
		}
	}
	
	return new veekay::graphics::Texture(cmd, size, size, VK_FORMAT_B8G8R8A8_UNORM, pixels.data());
}

// Generate brick specular texture
veekay::graphics::Texture* generateBrickSpecular(VkCommandBuffer cmd, uint32_t size) {
	std::vector<uint32_t> pixels(size * size);
	
	uint32_t brick_width = size / 4;
	uint32_t brick_height = size / 8;
	uint32_t mortar_size = 4;
	
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			uint32_t row = y / brick_height;
			uint32_t offset = (row % 2) * (brick_width / 2);
			
			uint32_t local_x = (x + offset) % brick_width;
			uint32_t local_y = y % brick_height;
			
			bool is_mortar = local_x < mortar_size || local_y < mortar_size;
			
			// Mortar is rougher (less specular), bricks are shinier
			uint8_t spec = is_mortar ? 20 : 100;
			
			pixels[y * size + x] = 0xff000000 | (spec << 16) | (spec << 8) | spec;
		}
	}
	
	return new veekay::graphics::Texture(cmd, size, size, VK_FORMAT_B8G8R8A8_UNORM, pixels.data());
}

VkSampler createSampler(VkFilter filter, VkSamplerAddressMode addressMode) {
	VkDevice& device = veekay::app.vk_device;
	
	VkSamplerCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = filter,
		.minFilter = filter,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = addressMode,
		.addressModeV = addressMode,
		.addressModeW = addressMode,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_TRUE,
		.maxAnisotropy = 8.0f,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = 1.0f,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	};
	
	VkSampler sampler;
	if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
		std::cerr << "Failed to create sampler\n";
		return VK_NULL_HANDLE;
	}
	
	return sampler;
}

// Create sphere mesh
Mesh createSphereMesh(uint32_t segments, uint32_t rings) {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	
	for (uint32_t ring = 0; ring <= rings; ++ring) {
		float phi = float(M_PI) * float(ring) / float(rings);
		
		for (uint32_t seg = 0; seg <= segments; ++seg) {
			float theta = 2.0f * float(M_PI) * float(seg) / float(segments);
			
			float x = std::sin(phi) * std::cos(theta);
			float y = std::cos(phi);
			float z = std::sin(phi) * std::sin(theta);
			
			float u = float(seg) / float(segments);
			float v = float(ring) / float(rings);
			
			vertices.push_back({
				{x * 0.5f, y * 0.5f, z * 0.5f},
				{x, y, z},
				{u, v}
			});
		}
	}
	
	for (uint32_t ring = 0; ring < rings; ++ring) {
		for (uint32_t seg = 0; seg < segments; ++seg) {
			uint32_t curr = ring * (segments + 1) + seg;
			uint32_t next = curr + segments + 1;
			
			indices.push_back(curr);
			indices.push_back(next);
			indices.push_back(curr + 1);
			
			indices.push_back(curr + 1);
			indices.push_back(next);
			indices.push_back(next + 1);
		}
	}
	
	Mesh mesh;
	mesh.vertex_buffer = new veekay::graphics::Buffer(
		vertices.size() * sizeof(Vertex), vertices.data(),
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	
	mesh.index_buffer = new veekay::graphics::Buffer(
		indices.size() * sizeof(uint32_t), indices.data(),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	
	mesh.indices = uint32_t(indices.size());
	
	return mesh;
}

// Create pyramid mesh (тетраэдр)
Mesh createPyramidMesh() {
	Mesh pyramid_mesh;

	std::vector<Vertex> vertices = {
        // Основание пирамиды (квадрат) - Y = -0.5
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}}, // 0
        {{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}}, // 1  
        {{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}}, // 2
        {{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, // 3
        
        // Вершина пирамиды - Y = +0.5
        {{0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f}}, // 4
    };

    // Вычисляем нормали для боковых граней
    veekay::vec3 front_normal = {0.0f, 0.0f, 1.0f};
    veekay::vec3 right_normal = {1.0f, 0.0f, 0.0f};
    veekay::vec3 back_normal = {0.0f, 0.0f, -1.0f};
    veekay::vec3 left_normal = {-1.0f, 0.0f, 0.0f};

    std::vector<Vertex> final_vertices = {
        // Основание (все вершины с нормалью вниз)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}}, // 0
        {{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}}, // 1
        {{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}}, // 2
        {{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, // 3
        
        // Передняя грань (треугольник 3-2-4)
        {{-0.5f, -0.5f, +0.5f}, front_normal, {0.0f, 0.0f}}, // 4
        {{+0.5f, -0.5f, +0.5f}, front_normal, {1.0f, 0.0f}}, // 5
        {{0.0f, 0.5f, 0.0f}, front_normal, {0.5f, 1.0f}},    // 6
        
        // Правая грань (треугольник 2-1-4)
        {{+0.5f, -0.5f, +0.5f}, right_normal, {0.0f, 0.0f}}, // 7
        {{+0.5f, -0.5f, -0.5f}, right_normal, {1.0f, 0.0f}}, // 8
        {{0.0f, 0.5f, 0.0f}, right_normal, {0.5f, 1.0f}},    // 9
        
        // Задняя грань (треугольник 1-0-4)
        {{+0.5f, -0.5f, -0.5f}, back_normal, {0.0f, 0.0f}},  // 10
        {{-0.5f, -0.5f, -0.5f}, back_normal, {1.0f, 0.0f}},  // 11
        {{0.0f, 0.5f, 0.0f}, back_normal, {0.5f, 1.0f}},     // 12
        
        // Левая грань (треугольник 0-3-4)
        {{-0.5f, -0.5f, -0.5f}, left_normal, {0.0f, 0.0f}},  // 13
        {{-0.5f, -0.5f, +0.5f}, left_normal, {1.0f, 0.0f}},  // 14
        {{0.0f, 0.5f, 0.0f}, left_normal, {0.5f, 1.0f}},     // 15
    };

    // ИСПРАВЛЕННЫЕ ИНДЕКСЫ - правильный порядок для clockwise
    std::vector<uint32_t> indices = {
        // Основание (по часовой стрелке)
        0, 2, 1,
        0, 3, 2,
        
        // Боковые грани (по часовой стрелке)
        // Передняя грань
        4, 6, 5,
        // Правая грань  
        7, 9, 8,
        // Задняя грань
        10, 12, 11,
        // Левая грань
        13, 15, 14
    };

    pyramid_mesh.vertex_buffer = new veekay::graphics::Buffer(
        final_vertices.size() * sizeof(Vertex), final_vertices.data(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    pyramid_mesh.index_buffer = new veekay::graphics::Buffer(
        indices.size() * sizeof(uint32_t), indices.data(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    pyramid_mesh.indices = uint32_t(indices.size());

	return pyramid_mesh;
}

uint32_t createMaterial(veekay::graphics::Texture* diffuse,
                        veekay::graphics::Texture* specular,
                        veekay::graphics::Texture* emissive,
                        VkSampler sampler,
                        float shininess,
                        bool use_texture,
                        bool use_triplanar) {
	VkDevice& device = veekay::app.vk_device;
	
	Material mat;
	mat.diffuse_texture = diffuse ? diffuse : missing_texture;
	mat.specular_texture = specular ? specular : white_texture;
	mat.emissive_texture = emissive ? emissive : black_texture;
	mat.sampler = sampler ? sampler : default_sampler;
	mat.shininess = shininess;
	mat.use_texture = use_texture;
	mat.use_triplanar = use_triplanar;
	
	// Allocate descriptor set for this material
	VkDescriptorSetAllocateInfo alloc_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &descriptor_set_layout,
	};
	
	if (vkAllocateDescriptorSets(device, &alloc_info, &mat.descriptor_set) != VK_SUCCESS) {
		std::cerr << "Failed to allocate descriptor set for material\n";
		return 0;
	}
	
	// Update descriptor set with buffers and textures
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
	};
	
	VkDescriptorImageInfo diffuse_info{
		.sampler = mat.sampler,
		.imageView = mat.diffuse_texture->view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo specular_info{
		.sampler = mat.sampler,
		.imageView = mat.specular_texture->view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo emissive_info{
		.sampler = mat.sampler,
		.imageView = mat.emissive_texture->view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkDescriptorImageInfo shadow_info{
		.sampler = shadow_map.sampler,
		.imageView = shadow_map.view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	
	VkWriteDescriptorSet write_infos[] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = mat.descriptor_set,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &buffer_infos[0],
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = mat.descriptor_set,
			.dstBinding = 1,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.pBufferInfo = &buffer_infos[1],
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = mat.descriptor_set,
			.dstBinding = 2,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &diffuse_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = mat.descriptor_set,
			.dstBinding = 3,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &specular_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = mat.descriptor_set,
			.dstBinding = 4,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &emissive_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = mat.descriptor_set,
			.dstBinding = 5,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &shadow_info,
		},
	};
	
	vkUpdateDescriptorSets(device, sizeof(write_infos) / sizeof(write_infos[0]), write_infos, 0, nullptr);
	
	materials.push_back(mat);
	return uint32_t(materials.size() - 1);
}

void initialize(VkCommandBuffer cmd) {
	VkDevice& device = veekay::app.vk_device;
	VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

	{ // NOTE: Build graphics pipeline
		vertex_shader_module = loadShaderModule("/home/cbf/MAI/Sem_5/CG/Lab4_2/lab4/shaders/shader.vert.spv");
		if (!vertex_shader_module) {
			std::cerr << "Failed to load Vulkan vertex shader from file\n";
			veekay::app.running = false;
			return;
		}

		fragment_shader_module = loadShaderModule("/home/cbf/MAI/Sem_5/CG/Lab4_2/lab4/shaders/shader.frag.spv");
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
					.descriptorCount = 64,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
					.descriptorCount = 64,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 80,
				}
			};
			
			VkDescriptorPoolCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = max_materials,
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

		// NOTE: Descriptor set layout specification with texture bindings
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
					// Diffuse texture
					.binding = 2,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					// Specular texture
					.binding = 3,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					// Emissive texture
					.binding = 4,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					// Shadow map
					.binding = 5,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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

		// NOTE: Declare external data sources
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

		if (!loadDynamicRenderingFunctions()) {
			std::cerr << "Failed to load dynamic rendering functions\n";
			veekay::app.running = false;
			return;
		}

		if (!createShadowMapResources()) {
			std::cerr << "Failed to set up shadow map resources\n";
			veekay::app.running = false;
			return;
		}

		shadow_vertex_shader_module = loadShaderModule("/home/cbf/MAI/Sem_5/CG/Lab4_2/lab4/shaders/shadow.vert.spv");
		if (!shadow_vertex_shader_module) {
			std::cerr << "Failed to load shadow vertex shader\n";
			veekay::app.running = false;
			return;
		}

		shadow_fragment_shader_module = loadShaderModule("/home/cbf/MAI/Sem_5/CG/Lab4_2/lab4/shaders/shadow.frag.spv");
		if (!shadow_fragment_shader_module) {
			std::cerr << "Failed to load shadow fragment shader\n";
			veekay::app.running = false;
			return;
		}

		VkPipelineShaderStageCreateInfo shadow_stage_infos[2] = {
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = shadow_vertex_shader_module,
				.pName = "main",
			},
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = shadow_fragment_shader_module,
				.pName = "main",
			}
		};

		VkVertexInputBindingDescription shadow_binding{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		VkVertexInputAttributeDescription shadow_attributes[] = {
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

		VkPipelineVertexInputStateCreateInfo shadow_input_state{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &shadow_binding,
			.vertexAttributeDescriptionCount = sizeof(shadow_attributes) / sizeof(shadow_attributes[0]),
			.pVertexAttributeDescriptions = shadow_attributes,
		};

		VkPipelineInputAssemblyStateCreateInfo shadow_assembly{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		VkViewport shadow_viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = static_cast<float>(shadow_map.extent.width),
			.height = static_cast<float>(shadow_map.extent.height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};

		VkRect2D shadow_scissor{
			.offset = {0, 0},
			.extent = shadow_map.extent,
		};

		VkPipelineViewportStateCreateInfo shadow_viewport_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.pViewports = &shadow_viewport,
			.scissorCount = 1,
			.pScissors = &shadow_scissor,
		};

		VkPipelineRasterizationStateCreateInfo shadow_raster{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.depthClampEnable = VK_FALSE,
			.rasterizerDiscardEnable = VK_FALSE,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_FRONT_BIT,  // Cull front faces to avoid shadow acne on back faces
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.depthBiasEnable = VK_TRUE,
			.depthBiasConstantFactor = 4.0f,
			.depthBiasClamp = 0.0f,
			.depthBiasSlopeFactor = 1.5f,
			.lineWidth = 1.0f,
		};

		VkPipelineMultisampleStateCreateInfo shadow_msaa{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};

		VkPipelineDepthStencilStateCreateInfo shadow_depth_state{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE,
			.depthWriteEnable = VK_TRUE,
			.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		};

		VkPipelineColorBlendStateCreateInfo shadow_blend_state{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 0,
			.pAttachments = nullptr,
		};

		VkPipelineRenderingCreateInfo rendering_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.depthAttachmentFormat = shadow_map.format,
		};

		VkGraphicsPipelineCreateInfo shadow_info{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &rendering_info,
			.stageCount = 2,
			.pStages = shadow_stage_infos,
			.pVertexInputState = &shadow_input_state,
			.pInputAssemblyState = &shadow_assembly,
			.pViewportState = &shadow_viewport_info,
			.pRasterizationState = &shadow_raster,
			.pMultisampleState = &shadow_msaa,
			.pDepthStencilState = &shadow_depth_state,
			.pColorBlendState = &shadow_blend_state,
			.layout = pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
			.subpass = 0,
		};

		if (vkCreateGraphicsPipelines(device, nullptr, 1, &shadow_info, nullptr, &shadow_pipeline) != VK_SUCCESS) {
			std::cerr << "Failed to create shadow graphics pipeline\n";
			veekay::app.running = false;
			return;
		}
	}

	scene_uniforms_buffer = new veekay::graphics::Buffer(
		sizeof(SceneUniforms),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	model_uniforms_buffer = new veekay::graphics::Buffer(
		max_models * veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms)),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	// Create samplers with different parameters
	sampler_linear_repeat = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	sampler_nearest_repeat = createSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
	sampler_linear_clamp = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
	sampler_linear_mirror = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
	default_sampler = sampler_linear_repeat;

	// Create default/fallback textures
	{
		uint32_t missing_pixels[] = {
			0xff000000, 0xffff00ff,
			0xffff00ff, 0xff000000,
		};
		missing_texture = new veekay::graphics::Texture(cmd, 2, 2, VK_FORMAT_B8G8R8A8_UNORM, missing_pixels);
		
		uint32_t white_pixel = 0xffffffff;
		white_texture = new veekay::graphics::Texture(cmd, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, &white_pixel);
		
		uint32_t black_pixel = 0xff000000;
		black_texture = new veekay::graphics::Texture(cmd, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, &black_pixel);
	}

	// Load textures from files and generate procedural textures
	// Удаляем загрузку ненужных текстур, оставляем только базовые
	{
		// Создаем белую текстуру для пола
		uint32_t white_color = 0xffffffff;
		white_texture = new veekay::graphics::Texture(cmd, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, &white_color);
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
			{{5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {5.0f, 0.0f}},
			{{5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {5.0f, 5.0f}},
			{{-5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 5.0f}},
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
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		cube_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		cube_mesh.indices = uint32_t(indices.size());
	}

	// Create sphere mesh
	sphere_mesh = createSphereMesh(32, 16);

	// Create pyramid mesh
	pyramid_mesh = createPyramidMesh();

	// Create materials with plain colors
	// Material 0: Белый пол
	uint32_t mat_white = createMaterial(
		white_texture, white_texture, black_texture,
		sampler_linear_repeat, 32.0f, false, false);

	uint32_t mat_red = createMaterial(
		white_texture, white_texture, black_texture,
		sampler_linear_repeat, 64.0f, false, false);
    
	uint32_t mat_green = createMaterial(
		white_texture, white_texture, black_texture,
		sampler_linear_repeat, 64.0f, false, false);

	uint32_t mat_blue = createMaterial(
		white_texture, white_texture, black_texture,
		sampler_linear_repeat, 64.0f, false, false);

	// NOTE: Add models to scene
	// Floor - белый
	models.emplace_back(Model{
		.mesh = plane_mesh,
		.transform = Transform{},
		.albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
		.material_index = mat_white
	});

	// Cube - красный
	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {-2.0f, -0.5f, 0.0f},
			.scale = {1.0f, 1.0f, 1.0f},
		},
		.albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
		.material_index = mat_red
	});

	// Pyramid - зеленая
	models.emplace_back(Model{
		.mesh = pyramid_mesh,
		.transform = Transform{
			.position = {0.0f, -1.0f, 0.0f},
			.scale = {1.0f, 1.0f, 1.0f},
		},
		.albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
		.material_index = mat_green
	});

	// Sphere - синий
	models.emplace_back(Model{
		.mesh = sphere_mesh,
		.transform = Transform{
			.position = {2.0f, -0.5f, 0.0f},
			.scale = {1.0f, 1.0f, 1.0f},
		},
		.albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f},
		.material_index = mat_blue
	});
}

// NOTE: Destroy resources here, do not cause leaks in your program!
void shutdown() {
	VkDevice& device = veekay::app.vk_device;

	// Destroy textures
	if (lenna_texture && lenna_texture != missing_texture) delete lenna_texture;
	if (lenna_specular && lenna_specular != white_texture) delete lenna_specular;
	if (lenna_emissive) delete lenna_emissive;
	
	delete checker_texture;
	delete checker_specular;
	
	delete brick_texture;
	delete brick_specular;
	delete brick_emissive;
	
	delete missing_texture;
	delete white_texture;
	delete black_texture;

	// Destroy samplers
	vkDestroySampler(device, sampler_linear_repeat, nullptr);
	vkDestroySampler(device, sampler_nearest_repeat, nullptr);
	vkDestroySampler(device, sampler_linear_clamp, nullptr);
	vkDestroySampler(device, sampler_linear_mirror, nullptr);

	// Destroy meshes
	delete pyramid_mesh.index_buffer;
	delete pyramid_mesh.vertex_buffer;
	delete sphere_mesh.index_buffer;
	delete sphere_mesh.vertex_buffer;
	delete cube_mesh.index_buffer;
	delete cube_mesh.vertex_buffer;
	delete plane_mesh.index_buffer;
	delete plane_mesh.vertex_buffer;

	delete model_uniforms_buffer;
	delete scene_uniforms_buffer;

	vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

	if (shadow_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(device, shadow_pipeline, nullptr);
		shadow_pipeline = VK_NULL_HANDLE;
	}
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	if (shadow_fragment_shader_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(device, shadow_fragment_shader_module, nullptr);
		shadow_fragment_shader_module = VK_NULL_HANDLE;
	}
	if (shadow_vertex_shader_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(device, shadow_vertex_shader_module, nullptr);
		shadow_vertex_shader_module = VK_NULL_HANDLE;
	}
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);

	destroyShadowMapResources();
}

void update(double time) {
	scene_time = float(time);
	
	ImGui::Begin("Simple Scene Controls");
	
	ImGui::Text("Camera Position: (%.2f, %.2f, %.2f)", 
		camera.position.x, camera.position.y, camera.position.z);
	
	ImGui::Separator();
	ImGui::Text("Light Controls:");
	ImGui::SliderFloat3("Light Direction", &light_direction_input.x, -1.0f, 1.0f);
	ImGui::SliderFloat("Shadow Bias", &shadow_bias, 0.0001f, 0.01f);
	ImGui::SliderFloat("Shadow Strength", &shadow_strength, 0.0f, 1.0f);
	ImGui::SliderFloat("Shadow Ortho Size", &shadow_ortho_size, 2.0f, 15.0f);
	
	ImGui::Separator();
	ImGui::Text("Hold LMB + WASD/QZ to move camera");
	ImGui::Text("Objects: Red Cube, Green Pyramid, Blue Sphere");
	
	ImGui::End();

	if (!ImGui::IsWindowHovered()) {
		using namespace veekay::input;

		if (mouse::isButtonDown(mouse::Button::left)) {
			auto move_delta = mouse::cursorDelta();

			camera.rotation.x += move_delta.y * 0.005f;
			camera.rotation.y += move_delta.x * 0.005f;
			
			auto view = camera.view();

			veekay::vec3 right = {view[0][0], view[1][0], view[2][0]};
			veekay::vec3 up = {view[0][1], view[1][1], view[2][1]};
			veekay::vec3 front = {view[0][2], view[1][2], view[2][2]};

			float speed = 0.1f;
			
			if (keyboard::isKeyDown(keyboard::Key::w))
				camera.position += front * speed;

			if (keyboard::isKeyDown(keyboard::Key::s))
				camera.position -= front * speed;

			if (keyboard::isKeyDown(keyboard::Key::d))
				camera.position += right * speed;

			if (keyboard::isKeyDown(keyboard::Key::a))
				camera.position -= right * speed;

			if (keyboard::isKeyDown(keyboard::Key::q))
				camera.position += up * speed;

			if (keyboard::isKeyDown(keyboard::Key::z))
				camera.position -= up * speed;
		}
	}

	// Убираем анимацию для статичной сцены

	shadow_bias = std::max(0.00005f, shadow_bias);
	shadow_strength = std::clamp(shadow_strength, 0.0f, 1.0f);
	shadow_ortho_size = std::max(2.0f, shadow_ortho_size);

	const veekay::vec3 normalized_light_dir =
		normalizeOrFallback(light_direction_input, veekay::vec3{0.0f, -1.0f, 0.0f});
	veekay::vec3 focus_point = scene_focus_point;
	veekay::vec3 light_world_position = focus_point - normalized_light_dir * shadow_camera_distance;

	veekay::vec3 up_vector = {0.0f, -1.0f, 0.0f};
	if (std::abs(veekay::vec3::dot(up_vector, normalized_light_dir)) > 0.95f) {
		up_vector = {0.0f, 0.0f, 1.0f};
	}

	const float ortho_extent = std::max(2.0f, shadow_ortho_size);
	const float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);

    /*
        (1)
        матрица вида источника света
        матрица, которая переводит вершины из мировых координат в систему координат источника света
    */
	veekay::mat4 light_view = lookAtLH(light_world_position, focus_point, up_vector);
    /*
        (1)
        матрица проекции источника света
        ортографическая проекция, которая определяет объем видимости света.
        все, что внутри этого объема, будет отбрасывать/принимать тени.
    */
	veekay::mat4 light_projection = orthographicOffCenterLH(
		-ortho_extent, ortho_extent,
		-ortho_extent, ortho_extent,
		shadow_near_plane,
		shadow_far_plane);
	veekay::mat4 light_view_projection = light_view * light_projection; // (1) подготовка матрицы проекции

	const float texel_size = shadow_map.extent.width > 0
		? 1.0f / static_cast<float>(shadow_map.extent.width)
		: 0.0f;

	SceneUniforms scene_uniforms{};
	scene_uniforms.view_projection = camera.view_projection(aspect_ratio);
	scene_uniforms.light_view_projection = light_view_projection;
	scene_uniforms.camera_position = camera.position;
	scene_uniforms.time = scene_time;
	scene_uniforms.light_position = light_world_position;
	scene_uniforms.shadow_bias = shadow_bias;
	scene_uniforms.light_direction = normalized_light_dir;
	scene_uniforms.shadow_strength = shadow_strength;
	scene_uniforms.shadow_map_texel_size = texel_size;

	std::vector<ModelUniforms> model_uniforms(models.size());
	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		const Material& mat = materials[model.material_index];
		ModelUniforms& uniforms = model_uniforms[i];

		uniforms.model = model.transform.matrix();
		uniforms.albedo_color = model.albedo_color;
		uniforms.shininess = mat.shininess;
		uniforms.use_texture = mat.use_texture ? 1.0f : 0.0f;
		uniforms.use_triplanar = mat.use_triplanar ? 1.0f : 0.0f;
	}

	*(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;

	const size_t alignment =
		veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

	for (size_t i = 0, n = model_uniforms.size(); i < n; ++i) {
		const ModelUniforms& uniforms = model_uniforms[i];

		char* const pointer = static_cast<char*>(model_uniforms_buffer->mapped_region) + i * alignment;
		*reinterpret_cast<ModelUniforms*>(pointer) = uniforms;
	}
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
	vkResetCommandBuffer(cmd, 0);

	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(cmd, &begin_info);

	const size_t model_uniforms_alignment =
		veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

	auto draw_models = [&](VkCommandBuffer buffer) {
		VkDeviceSize zero_offset = 0;
		VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
		VkBuffer current_index_buffer = VK_NULL_HANDLE;

		for (size_t i = 0, n = models.size(); i < n; ++i) {
			const Model& model = models[i];
			const Mesh& mesh = model.mesh;
			const Material& mat = materials[model.material_index];

			if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
				current_vertex_buffer = mesh.vertex_buffer->buffer;
				vkCmdBindVertexBuffers(buffer, 0, 1, &current_vertex_buffer, &zero_offset);
			}

			if (current_index_buffer != mesh.index_buffer->buffer) {
				current_index_buffer = mesh.index_buffer->buffer;
				vkCmdBindIndexBuffer(buffer, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
			}

			uint32_t offset = uint32_t(i * model_uniforms_alignment);
			vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
			                        0, 1, &mat.descriptor_set, 1, &offset);

			vkCmdDrawIndexed(buffer, mesh.indices, 1, 0, 0, 0);
		}
	};

	VkImageSubresourceRange shadow_range{
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	VkAccessFlags src_access = (shadow_map.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)
		: static_cast<VkAccessFlags>(0);

	VkImageMemoryBarrier prepare_shadow{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_access,
		.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.oldLayout = shadow_map.layout,
		.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = shadow_map.image,
		.subresourceRange = shadow_range,
	};

	VkPipelineStageFlags shadow_src_stage =
		shadow_map.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			: VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	vkCmdPipelineBarrier(cmd, shadow_src_stage,
	                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &prepare_shadow);

	shadow_map.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkClearDepthStencilValue shadow_clear{1.0f, 0};
	VkRenderingAttachmentInfo depth_attachment{
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = shadow_map.view,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = {.depthStencil = shadow_clear},
	};

	VkRenderingInfo rendering_info{
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {
			.extent = shadow_map.extent,
		},
		.layerCount = 1,
		.colorAttachmentCount = 0,
		.pColorAttachments = nullptr,
		.pDepthAttachment = &depth_attachment,
	};
    
	vkCmdBeginRenderingKHR_ptr(cmd, &rendering_info);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
	draw_models(cmd);
	vkCmdEndRenderingKHR_ptr(cmd);

	VkImageMemoryBarrier depth_to_sample{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = shadow_map.image,
		.subresourceRange = shadow_range,
	};

	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &depth_to_sample);

	shadow_map.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkClearValue clear_color{.color = {{0.02f, 0.02f, 0.05f, 1.0f}}};
	VkClearValue clear_depth{.depthStencil = {1.0f, 0}};
	VkClearValue clear_values[] = {clear_color, clear_depth};

	VkRenderPassBeginInfo pass_info{
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

	vkCmdBeginRenderPass(cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	draw_models(cmd);
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