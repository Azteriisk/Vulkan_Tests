#version 460

layout(location = 0) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(N, lightDir), 0.0);
    float ambient = 0.35;
    vec3 baseColor = vec3(0.75, 0.85, 1.0);
    vec3 color = baseColor * (ambient + diffuse * 0.65);
    outColor = vec4(color, 1.0);
}
