#version 330 core
out vec4 FragColor;

struct Material {
    vec4 baseColor;
    float metallic;
    float roughness;
};

uniform Material material;

void main() {
    if (material.baseColor.a < 0.01) discard;
    FragColor = material.baseColor;
}