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
uniform bool isCarPaint;
uniform bool hasDiffuseTexture;
uniform sampler2D texture_diffuse1;
uniform Light light;
uniform vec3 viewPos;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;

void main() {
    // Get base color from texture or material
    vec4 baseCol = material.baseColor;
    if (hasDiffuseTexture) {
        baseCol = texture(texture_diffuse1, TexCoords);
    }
    if (baseCol.a < 0.01) discard;
    
    // Calculate lighting
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(-light.direction);
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    
    // Ambient
    vec3 ambient = light.ambient * baseCol.rgb;
    
    // Diffuse
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = light.diffuse * diff * baseCol.rgb;
    
    // Specular
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    vec3 specular = light.specular * spec * baseCol.rgb;
    
    // Combine lighting
    vec3 result = ambient + diffuse + specular;
    
    // Only apply car color to CarPaint material
    if (isCarPaint) {
        result *= carColor;
    }
    
    FragColor = vec4(result, baseCol.a);
}