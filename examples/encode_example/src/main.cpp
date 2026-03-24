#include "VulkanOsdDemoApp.h"

#include <cstring>
#include <iostream>
#include <string>

/**
 * @brief Demo入口。
 *
 * 用法：
 * - encode_demo <input_path> <out_path> <debug_mode>
 *
 * 说明：
 * - input_path 可以是本地文件路径，也可以是 RTSP/HTTP 等DecoderSDK支持的URL
 * - out_path 编码输出的文件路径
 * - debug_mode 是否开启debug模式，on - 开启，渲染+编码；off - 关闭，仅编码
 */
int main(int argc, char **argv)
{
    try {
        const std::string inputPath = argc >= 2 ? argv[1] : "D:/WorkSpace/test_video/test.mp4";
        const std::string outPath = argc >= 3 ? argv[2] : "./output.mp4";
        const bool debug = argc >= 4 ? strcmp(argv[3], "on") == 0 : true;

        std::cout << "Running in encode demo..." << std::endl;
        std::cout << "Input path: " << inputPath << std::endl;
        std::cout << "Ouput path: " << outPath << std::endl;
        std::cout << "Debug mode: " << debug << std::endl;

        VulkanOsdDemoApp app;
        return app.run(inputPath, outPath, debug);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 2;
    }
}
