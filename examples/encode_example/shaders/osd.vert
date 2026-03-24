#version 450

layout(location = 0) in vec2 inPosPx;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform PushConstants
{
    vec2 screenSize;
} pc;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;

void main()
{
    // normalized [-1, 1]
    vec2 ndc;
    ndc.x = (inPosPx.x / pc.screenSize.x) * 2.0 - 1.0;
    ndc.y = (inPosPx.y / pc.screenSize.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUv = inUv;
    fragColor = inColor;
}

