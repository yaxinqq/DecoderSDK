#version 450

layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 texColor = texture(uFontAtlas, fragUv);
    outColor = texColor * fragColor;
}
