#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(set = 0, binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) out vec3 fragNormal;

void main() {
    mat3 normalMatrix = mat3(transpose(inverse(ubo.model)));
    vec4 worldPosition = ubo.model * vec4(inPosition, 1.0);
    fragNormal = normalize(normalMatrix * inNormal);
    gl_Position = ubo.proj * ubo.view * worldPosition;
}
