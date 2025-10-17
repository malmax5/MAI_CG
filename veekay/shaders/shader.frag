#version 450

layout(push_constant) uniform Constants {
  mat4 projection;
  mat4 transform;
  vec3 color;
} constants;

layout(location = 0) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
  // Multiply per-vertex color with uniform color for tinting effect
  outColor = vec4(fragColor * constants.color, 1.0);
}