#version 330 core
out vec4 FragColor;

struct Material {
    vec4 baseColor;
    float metallic;
    float roughness;
};

struct Light {
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

uniform Material material;
uniform vec3 carColor;
uniform bool isCarPaint;  // New uniform to identify CarPaint material
uniform Light light;
uniform vec3 viewPos;

in vec3 FragPos;
in vec3 Normal;

void main() {
    if (material.baseColor.a < 0.01) discard;
    
    // Calculate lighting
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(-light.direction);
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    
    // Ambient
    vec3 ambient = light.ambient * material.baseColor.rgb;
    
    // Diffuse
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = light.diffuse * diff * material.baseColor.rgb;
    
    // Specular
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    vec3 specular = light.specular * spec * material.baseColor.rgb;
    
    // Combine lighting
    vec3 result = ambient + diffuse + specular;
    
    // Only apply car color to CarPaint material
    if (isCarPaint) {
        result *= carColor;
    }
    
    FragColor = vec4(result, material.baseColor.a);
}