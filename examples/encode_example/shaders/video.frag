#version 450

layout(set = 0, binding = 0) uniform sampler2D textureY;
layout(set = 0, binding = 1) uniform sampler2D textureUV;

layout(location = 0) in vec2 texturePos;

layout(location = 0) out vec4 outColor;

void main()
{
    // 采样Y和UV纹理
    float y = texture(textureY, texturePos).r;
    vec2 uv = texture(textureUV, texturePos).rg;

    // 常量偏移和转换矩阵
    const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
    const mat3 yuv2rgb_mat = mat3(
        1.16438356,  0.0,           1.79274107,
        1.16438356, -0.21324861, -0.53290932,
        1.16438356,  2.11240178,  0.0
    );

    // YUV → RGB
    vec3 rgb = (vec3(y, uv.x, uv.y) - yuv2rgb_ofs) * yuv2rgb_mat;

    outColor = vec4(rgb, 1.0);
}