#include <cstdint>
#include <climits>
#include <vector>
#include <iostream>
#include <fstream>
#include <cmath>

#include <veekay/veekay.hpp>  // Основная библиотека для работы с Vulkan и окном

#include <imgui.h>            // Библиотека для GUI
#include <vulkan/vulkan_core.h>

namespace {  // Анонимное пространство имен - все функции/переменные видны только в этом файле

// Константы камеры
constexpr float camera_fov = 70.0f;        // Угол обзора камеры в градусах
constexpr float camera_near_plane = 0.01f; // Ближняя плоскость отсечения
constexpr float camera_far_plane = 100.0f; // Дальняя плоскость отсечения

// Структура для представления матрицы 4x4
struct Matrix {
  float m[4][4];  // Данные матрицы в row-major порядке
};

// Структура для представления 3D вектора
struct Vector {
  float x, y, z;
};

// Структура вершины с позицией и цветом
struct Vertex {
  Vector position;  // Позиция вершины в 3D пространстве
  Vector color;     // RGB цвет вершины
};

// Константы, передаваемые в шейдеры через push constants
struct ShaderConstants {
  Matrix projection;  // Матрица проекции (перспективная/ортографическая)
  Matrix transform;   // Матрица преобразования модели (поворот, перемещение)
  Vector color;
};

// Структура для хранения Vulkan буфера и его памяти
struct VulkanBuffer {
  VkBuffer buffer;        // Handle буфера
  VkDeviceMemory memory;  // Handle выделенной памяти
};

// Глобальные переменные Vulkan
VkShaderModule vertex_shader_module;   // Модуль вершинного шейдера
VkShaderModule fragment_shader_module; // Модуль фрагментного шейдера
VkPipelineLayout pipeline_layout;      // Layout пайплайна (описывает ресурсы)
VkPipeline pipeline;                   // Графический пайплайн

// Буферы для геометрии
VulkanBuffer vertex_buffer;  // Буфер вершин
VulkanBuffer index_buffer;   // Индексный буфер

// Переменные для управления моделью
Vector model_position = {0.0f, 0.0f, 5.0f};  // Позиция модели в мировом пространстве
float model_rotation_x = 0.0f;                // Вращение вокруг оси X
float model_rotation_y = 0.0f;                // Вращение вокруг оси Y
float rotation_speed_x = 1.0f;                // Скорость вращения вокруг X
float rotation_speed_y = 1.0f;                // Скорость вращения вокруг Y
Vector model_color = {0.5f, 1.0f, 0.7f};      // Цвет тонирования модели
bool animate = false;                         // Флаг анимации
bool use_perspective = true;                  // Тип проекции: true - перспективная, false - ортографическая
float ortho_scale = 5.0f;                     // Масштаб для ортографической проекции

// Создает единичную матрицу
Matrix identity() {
  Matrix result{};
  result.m[0][0] = 1.0f;
  result.m[1][1] = 1.0f;
  result.m[2][2] = 1.0f;
  result.m[3][3] = 1.0f;
  return result;
}

// Создает матрицу перспективной проекции
Matrix perspective_projection(float fov, float aspect_ratio, float near, float far) {
  /*
    Меньший FOV: Объекты выглядят крупнее, меньше искажений, видно меньше сцены
    Больший FOV: Объекты выглядят мельче, сильные перспективные искажения, видно больше сцены
    Aspect = 1: Идеальный квадрат, нет растяжения
    Aspect > 1: Горизонтальное растяжение (широкий экран)
    Aspect < 1: Вертикальное растяжение (высокий экран)
    near - fat - диапазоны видимости от камеры
  */
  Matrix result{};
  const float radians = fov * M_PI / 180.0f;  // Перевод градусов в радианы угла обзора
  const float cot = 1.0f / tanf(radians / 2.0f);  // Котангенс половинного угла - масштаб проекции - сжатие чем меньше cot

  result.m[0][0] = cot / aspect_ratio;  // Масштабирование по X
  result.m[1][1] = cot;                 // Масштабирование по Y
  result.m[2][3] = 1.0f;                // Для перспективного деления

  // Преобразование глубины из [near, far] в [0, 1]
  result.m[2][2] = far / (far - near);
  result.m[3][2] = (-near * far) / (far - near);

  return result;
}

// Создает матрицу ортографической проекции
Matrix orthographic_projection(float aspect_ratio, float scale, float near, float far) {
  /*
    Aspect = 1: Идеальный квадрат, нет растяжения
    Aspect > 1: Горизонтальное растяжение (широкий экран)
    Aspect < 1: Вертикальное растяжение (высокий экран)
    Меньший scale: Объекты выглядят крупнее, видно меньше сцены (как "приближение")
    Больший scale: Объекты выглядят мельче, видно больше сцены (как "отдаление")
  */
  // Расчет границ видимой области - куб видимости(квадрат left/right/bottom/top, глубина - near->fat)
  float left = -scale * aspect_ratio;
  float right = scale * aspect_ratio;
  float bottom = -scale;
  float top = scale;

  Matrix result{};
  // Масштабирование и смещение для ортографической проекции
  result.m[0][0] = 2.0f / (right - left); // коэффициент масштабирования для умещения в диапазон [-1, 1]
  result.m[1][1] = 2.0f / (top - bottom); // коэффициент масштабирования для умещения в диапазон [-1, 1]
  result.m[2][2] = 1.0f / (far - near); // коэффициент масштабирования для умещения в диапазон [-1, 1]
  result.m[3][0] = -(right + left) / (right - left); // смещение для центрирования
  result.m[3][1] = -(top + bottom) / (top - bottom); // смещение для центрирования
  result.m[3][2] = -near / (far - near); // Смещает Z-координаты так, чтобы near plane соответствовала 0, а far plane - 1 
  result.m[3][3] = 1.0f;

  return result;
}

// Создает матрицу перемещения
Matrix translation(Vector vector) {
  Matrix result = identity();
  result.m[3][0] = vector.x;  // Смещение по X
  result.m[3][1] = vector.y;  // Смещение по Y
  result.m[3][2] = vector.z;  // Смещение по Z
  return result;
}

// Создает матрицу вращения вокруг произвольной оси
Matrix rotation(Vector axis, float angle) {
  Matrix result{};
  // Нормализация оси вращения
  float length = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
  axis.x /= length;
  axis.y /= length;
  axis.z /= length;

  float sina = sinf(angle);  // Синус угла
  float cosa = cosf(angle);  // Косинус угла
  float cosv = 1.0f - cosa;

  // Расчет элементов матрицы вращения (формула Родригеса)
  result.m[0][0] = (axis.x * axis.x * cosv) + cosa;
  result.m[0][1] = (axis.x * axis.y * cosv) + (axis.z * sina);
  result.m[0][2] = (axis.x * axis.z * cosv) - (axis.y * sina);

  result.m[1][0] = (axis.y * axis.x * cosv) - (axis.z * sina);
  result.m[1][1] = (axis.y * axis.y * cosv) + cosa;
  result.m[1][2] = (axis.y * axis.z * cosv) + (axis.x * sina);

  result.m[2][0] = (axis.z * axis.x * cosv) + (axis.y * sina);
  result.m[2][1] = (axis.z * axis.y * cosv) - (axis.x * sina);
  result.m[2][2] = (axis.z * axis.z * cosv) + cosa;

  result.m[3][3] = 1.0f;

  return result;
}

// Умножение двух матриц
Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix result{};
  for (int j = 0; j < 4; j++) {
    for (int i = 0; i < 4; i++) {
      for (int k = 0; k < 4; k++) {
        result.m[j][i] += a.m[j][k] * b.m[k][i];
      }
    }
  }
  return result;
}

// Загружает шейдерный модуль из скомпилированного файла
VkShaderModule loadShaderModule(const char* path) {
  // Открытие файла и чтение бинарных данных
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  size_t size = file.tellg();
  std::vector<uint32_t> buffer(size / sizeof(uint32_t));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(buffer.data()), size);
  file.close();

  // Создание шейдерного модуля Vulkan
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

// Создает Vulkan буфер и выделяет для него память
VulkanBuffer createBuffer(size_t size, void *data, VkBufferUsageFlags usage) {
  VkDevice& device = veekay::app.vk_device;
  VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;
  
  VulkanBuffer result{};

  { // Создание объекта буфера
    VkBufferCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,  // Тип буфера (вершинный, индексный и т.д.)
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,  // Буфер используется только одной очередью
    };

    if (vkCreateBuffer(device, &info, nullptr, &result.buffer) != VK_SUCCESS) {
      std::cerr << "Failed to create Vulkan buffer\n";
      return {};
    }
  }

  { // Выделение памяти для буфера
    // Получение требований к памяти для буфера
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);

    // Получение информации о типах памяти GPU
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

    // Поиск подходящего типа памяти (видимой и для CPU и для GPU)
    const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t index = UINT_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      const VkMemoryType& type = properties.memoryTypes[i];
      if ((requirements.memoryTypeBits & (1 << i)) &&
          (type.propertyFlags & flags) == flags) {
        index = i;
        break;
      }
    }

    if (index == UINT_MAX) {
      std::cerr << "Failed to find required memory type to allocate Vulkan buffer\n";
      return {};
    }

    // Выделение памяти
    VkMemoryAllocateInfo info{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = index,
    };

    if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS) {
      std::cerr << "Failed to allocate Vulkan buffer memory\n";
      return {};
    }

    // Привязка памяти к буферу
    if (vkBindBufferMemory(device, result.buffer, result.memory, 0) != VK_SUCCESS) {
      std::cerr << "Failed to bind Vulkan buffer memory\n";
      return {};
    }

    // Копирование данных в буфер
    void* device_data;
    vkMapMemory(device, result.memory, 0, requirements.size, 0, &device_data);
    memcpy(device_data, data, size);
    vkUnmapMemory(device, result.memory);
  }

  return result;
}

// Освобождает ресурсы буфера
void destroyBuffer(const VulkanBuffer& buffer) {
  VkDevice& device = veekay::app.vk_device;
  vkFreeMemory(device, buffer.memory, nullptr);
  vkDestroyBuffer(device, buffer.buffer, nullptr);
}

// Основная функция инициализации приложения
void initialize() {
  VkDevice& device = veekay::app.vk_device;
  VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

  { // Создание графического пайплайна
    // Загрузка шейдеров
    vertex_shader_module = loadShaderModule("/home/cbf/MAI/Sem_5/CG/Lab1/veekay/shaders/shader.vert.spv");
    if (!vertex_shader_module) {
      std::cerr << "Failed to load Vulkan vertex shader from file\n";
      veekay::app.running = false;
      return;
    }

    fragment_shader_module = loadShaderModule("/home/cbf/MAI/Sem_5/CG/Lab1/veekay/shaders/shader.frag.spv");
    if (!fragment_shader_module) {
      std::cerr << "Failed to load Vulkan fragment shader from file\n";
      veekay::app.running = false;
      return;
    }

    // Описание стадий шейдеров
    VkPipelineShaderStageCreateInfo stage_infos[2];
    stage_infos[0] = VkPipelineShaderStageCreateInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vertex_shader_module,
      .pName = "main",  // Точка входа шейдера
    };

    stage_infos[1] = VkPipelineShaderStageCreateInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = fragment_shader_module,
      .pName = "main",
    };

    // Описание формата вершин
    VkVertexInputBindingDescription buffer_binding{
      .binding = 0,                    // Номер привязки
      .stride = sizeof(Vertex),        // Размер одной вершины в байтах
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,  // Частота ввода (на вершину)
    };

    // Описание атрибутов вершины
    VkVertexInputAttributeDescription attributes[] = {
      { // Атрибут позиции
        .location = 0,                 // location в шейдере
        .binding = 0,                  // Привязка к буферу
        .format = VK_FORMAT_R32G32B32_SFLOAT,  // Формат данных (3 float)
        .offset = offsetof(Vertex, position),  // Смещение в структуре Vertex
      },
      { // Атрибут цвета
        .location = 1,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(Vertex, color),
      },
    };

    // Состояние ввода вершин
    VkPipelineVertexInputStateCreateInfo input_state_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1,
      .pVertexBindingDescriptions = &buffer_binding,
      .vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
      .pVertexAttributeDescriptions = attributes,
    };

    // Примитивы - список треугольников
    VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,  // Тип примитивов
    };

    // Состояние растеризации
    VkPipelineRasterizationStateCreateInfo raster_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,     // Режим заполнения полигонов
      .cullMode = VK_CULL_MODE_BACK_BIT,       // Отсечение задних граней
      .frontFace = VK_FRONT_FACE_CLOCKWISE,    // Определение лицевой стороны
      .lineWidth = 1.0f,                       // Толщина линий
    };

    // Мультисэмплинг (отключен)
    VkPipelineMultisampleStateCreateInfo sample_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,  // 1 сэмпл на пиксель
      .sampleShadingEnable = false,
      .minSampleShading = 1.0f,
    };

    // Область вывода (весь экран)
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

    // Тестирование глубины
    VkPipelineDepthStencilStateCreateInfo depth_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = true,                    // Включить тест глубины
      .depthWriteEnable = true,                   // Разрешить запись глубины
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,  // Функция сравнения глубины
    };

    // Смешивание цветов
    VkPipelineColorBlendAttachmentState attachment_info{
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |  // Разрешить запись всех каналов
                        VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo blend_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = false,          // Отключить логические операции
      .logicOp = VK_LOGIC_OP_COPY,
      .attachmentCount = 1,
      .pAttachments = &attachment_info
    };

    // Push constants - быстрый способ передачи данных в шейдеры
    VkPushConstantRange push_constants{
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |  // Доступно в вершинном и фрагментном шейдерах
                    VK_SHADER_STAGE_FRAGMENT_BIT,
      .size = sizeof(ShaderConstants),  // Размер передаваемых данных
    };

    // Создание layout пайплайна
    VkPipelineLayoutCreateInfo layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push_constants,
    };

    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
      std::cerr << "Failed to create Vulkan pipeline layout\n";
      veekay::app.running = false;
      return;
    }
    
    // Сборка всей информации для создания графического пайплайна
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
      .renderPass = veekay::app.vk_render_pass,  // Render pass из фреймворка
    };

    // Создание графического пайплайна
    if (vkCreateGraphicsPipelines(device, nullptr, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
      std::cerr << "Failed to create Vulkan pipeline\n";
      veekay::app.running = false;
      return;
    }
  }

  // Создание геометрии октаэдра
  /*
    6 вершин расположены вдоль осей координат
    Каждая вершина имеет позицию (x,y,z) и цвет (r,g,b)
    Все вершины находятся на расстоянии 1 единица от центра
  */
  Vertex vertices[] = {
    { {0.0f,  1.0f,  0.0f}, {1.0f, 0.0f, 0.0f} }, // Верхняя вершина - красная
    { {0.0f, -1.0f,  0.0f}, {0.0f, 0.0f, 1.0f} }, // Нижняя вершина - синяя
    { {1.0f,  0.0f,  0.0f}, {0.0f, 1.0f, 0.0f} }, // Правая - зеленая
    { {-1.0f, 0.0f,  0.0f}, {1.0f, 1.0f, 0.0f} }, // Левая - желтая
    { {0.0f,  0.0f,  1.0f}, {0.0f, 1.0f, 1.0f} }, // Передняя - голубая
    { {0.0f,  0.0f, -1.0f}, {1.0f, 0.0f, 1.0f} }, // Задняя - пурпурная
  };

  // Индексы для построения 8 треугольников октаэдра(связь с vertices)
  uint32_t indices[] = {
    0, 4, 2,  // Верх-перед-право
    0, 3, 4,  // Верх-лево-перед
    0, 5, 3,  // Верх-зад-лево
    0, 2, 5,  // Верх-право-зад
    1, 5, 2,  // Низ-зад-право
    1, 3, 5,  // Низ-лево-зад
    1, 4, 3,  // Низ-перед-лево
    1, 2, 4   // Низ-правоx-перед
  };

  // Создание вершинного и индексного буферов
  vertex_buffer = createBuffer(sizeof(vertices), vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  index_buffer = createBuffer(sizeof(indices), indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

// Очистка ресурсов при завершении работы
void shutdown() {
  VkDevice& device = veekay::app.vk_device;

  // Уничтожение буферов
  destroyBuffer(index_buffer);
  destroyBuffer(vertex_buffer);

  // Уничтожение пайплайна и связанных ресурсов
  vkDestroyPipeline(device, pipeline, nullptr);
  vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
  vkDestroyShaderModule(device, fragment_shader_module, nullptr);
  vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

// Обновление состояния приложения (вызывается каждый кадр)
void update(double time) {
  // Создание GUI для управления параметрами
  ImGui::Begin("Controls:");
  ImGui::InputFloat3("Translation", reinterpret_cast<float*>(&model_position));  // Позиция модели
  ImGui::SliderFloat("Rotation Speed X", &rotation_speed_x, 0.0f, 5.0f);        // Скорость вращения X
  ImGui::SliderFloat("Rotation Speed Y", &rotation_speed_y, 0.0f, 5.0f);        // Скорость вращения Y
  ImGui::ColorEdit3("Color", reinterpret_cast<float*>(&model_color));            // Цвет модели
  ImGui::Checkbox("Perspective Projection", &use_perspective);                   // Переключение типа проекции
  if (!use_perspective) {
    ImGui::SliderFloat("Ortho Scale", &ortho_scale, 0.1f, 10.0f);               // Масштаб ортографической проекции
  }
  if (ImGui::Button("Animation")) {
    animate = !animate;  // Включение/выключение анимации
  }
  ImGui::End();

  // Обновление анимации
  float t = static_cast<float>(time);
  // Постоянное вращение модели
  model_rotation_x = fmodf(t * rotation_speed_x, 2.0f * static_cast<float>(M_PI));
  model_rotation_y = fmodf(t * rotation_speed_y, 2.0f * static_cast<float>(M_PI));
  
  // Анимация перемещения по траектории (фигура восьмерки)
  if (animate) {
    float a = 2.0f; // Масштаб траектории
    float sin_t = sinf(t);
    float cos_t = cosf(t);
    float denom = sin_t * sin_t + 1.0f;
    model_position.x = a * sqrtf(2.0f) * cos_t / denom;  // X координата по лемнискате
    model_position.y = a * sqrtf(2.0f) * sin_t * cos_t / denom;  // Y координата
    model_position.z = 5.0f;  // Фиксированная Z координата
  }
}

// Функция рендеринга (вызывается каждый кадр)
void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
  // Сброс командного буфера для новой записи
  vkResetCommandBuffer(cmd, 0);

  { // Начало записи команд
    VkCommandBufferBeginInfo info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,  // Буфер используется один раз
    };
    vkBeginCommandBuffer(cmd, &info);
  }

  { // Начало render pass
    VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};  // Темно-серый цвет очистки
    VkClearValue clear_depth{.depthStencil = {1.0f, 0}};            // Очистка глубины

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

    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);  // Начало render pass
  }

  { // Рендеринг модели
    // Привязка графического пайплайна
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Привязка вершинного буфера
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);

    // Привязка индексного буфера
    vkCmdBindIndexBuffer(cmd, index_buffer.buffer, offset, VK_INDEX_TYPE_UINT32);

    // Расчет матриц преобразования
    float aspect = float(veekay::app.window_width) / float(veekay::app.window_height);
    
    // Выбор типа проекции
    Matrix proj;
    if (use_perspective) {
      proj = perspective_projection(camera_fov, aspect, camera_near_plane, camera_far_plane);
    } else {
      proj = orthographic_projection(aspect, ortho_scale, camera_near_plane, camera_far_plane);
    }
    
    // Создание матрицы преобразования модели (вращение + перемещение)
    ShaderConstants constants{
      .projection = proj,
      .transform = multiply(
        rotation({1.0f, 0.0f, 0.0f}, model_rotation_x),  // Вращение вокруг X
        multiply(
          rotation({0.0f, 1.0f, 0.0f}, model_rotation_y),  // Вращение вокруг Y
          translation(model_position)  // Перемещение
        )
      ),
      .color = model_color,
    };

    // Передача констант в шейдеры через push constants
    vkCmdPushConstants(cmd, pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ShaderConstants), &constants);

    // Отрисовка индексами (8 треугольников × 3 вершины = 24 индекса)
    vkCmdDrawIndexed(cmd, 24, 1, 0, 0, 0);
  }

  // Завершение render pass и командного буфера
  vkCmdEndRenderPass(cmd);
  vkEndCommandBuffer(cmd);
}

} // namespace

// Точка входа приложения
int main() {
  // Запуск фреймворка с callback функциями
  return veekay::run({
    .init = initialize,    // Функция инициализации
    .shutdown = shutdown,  // Функция очистки
    .update = update,      // Функция обновления
    .render = render,      // Функция рендеринга
  });
}