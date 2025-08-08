# DecoderSDK

一个高性能的音视频解码SDK，支持多种硬件加速技术和实时流处理。

## 项目简介

DecoderSDK是一个基于FFmpeg的现代化音视频解码库，提供了简洁易用的C++ API接口。该SDK专为高性能音视频应用设计，支持多种硬件加速技术、实时流处理、录制功能以及完善的事件系统。

## 主要特性

### 🚀 核心功能
- **多格式支持**: 支持主流音视频格式解码
- **硬件加速**: 支持CUDA、DXVA2、D3D11VA、VAAPI等多种硬件加速
- **实时流处理**: 支持RTSP、RTMP等实时流协议
- **高性能队列**: 内置高效的帧队列和包队列管理
- **同步控制**: 精确的音视频同步机制
- **录制功能**: 支持实时流录制为本地文件

### 🎯 高级特性
- **异步操作**: 支持异步打开和操作
- **事件系统**: 完善的事件回调机制
- **预缓冲**: 可配置的预缓冲策略
- **自动重连**: 网络流自动重连机制
- **帧率控制**: 内置帧率控制和播放速度调节
- **解码退化**: 支持解码失败时的退化处理
- **跨平台**: 支持Windows、Linux等多平台
- **内存管理**: 智能的内存管理和资源回收
- **性能监控**: 内置解码性能统计和监控

## 运行平台

### 支持的操作系统
- **Windows**: Windows 10及以上版本
- **Linux**: 
  - 统信UOS
  - 银河麒麟
  - 其他主流Linux发行版

### 支持的架构
- x86_64 (Intel/AMD 64位)
- aarch64 (ARM 64位，Linux平台)

### 硬件加速支持
- **Windows**: CUDA、D3D11VA、DXVA2
- **Linux**: CUDA、VAAPI

## 系统要求

### 编译环境
- **编译器**: 
  - Windows: MSVC 2019及以上
  - Linux: GCC 7.0及以上 或 Clang 6.0及以上
  - macOS: Xcode 11及以上
- **CMake**: 3.14及以上版本
- **C++标准**: C++17

### 依赖库
- **FFmpeg**: 音视频处理核心库
- **spdlog**: 高性能日志库
- **fmt**: 字符串格式化库
- **nlohmann/json**: JSON处理库
- **eventpp**: 事件处理库
- **magic_enum**: 枚举反射库

## 快速开始

### 编译安装

#### Windows
```bash
# 克隆项目
git clone <repository-url>
cd DecoderSDK

# 设置FFmpeg路径（必需）
set FFMPEG_ROOT_DIR=C:\path\to\ffmpeg

# 创建构建目录
mkdir build
cd build

# 配置项目
cmake .. -DFFMPEG_ROOT_DIR=%FFMPEG_ROOT_DIR%

# 编译
cmake --build . --config Release

# 安装（可选）
cmake --install .
```

#### Linux
```bash
# 安装依赖
sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev

# 克隆项目
git clone <repository-url>
cd DecoderSDK

# 创建构建目录
mkdir build
cd build

# 配置项目
cmake ..

# 编译
make -j$(nproc)

# 安装（可选）
sudo make install
```

### 编译选项

- `BUILD_EXAMPLES`: 是否编译示例程序 (默认: ON)
- `BUILD_DECODERSDK_SHARED_LIBS`: 是否编译动态库 (默认: ON)
- `BUILD_CONSOLE_EXAMPLE`: 是否编译控制台示例 (默认: OFF)
- `BUILD_QT_GUI_EXAMPLE`: 是否编译Qt GUI示例 (默认: OFF)
- `BUILD_D3D_RENDER_EXAMPLE`: 是否编译D3D渲染示例 (默认: OFF)
- `FFMPEG_ROOT_DIR`: FFmpeg安装路径 (Windows必需)

示例：
```bash
cmake .. -DBUILD_EXAMPLES=ON -DBUILD_QT_GUI_EXAMPLE=ON -DFFMPEG_ROOT_DIR=/path/to/ffmpeg
```

## API 使用指南

### 基本使用

```cpp
#include "decodersdk/decoder_controller.h"

using namespace decoder_sdk;

int main() {
    // 创建解码器控制器
    DecoderController decoder;
    
    // 配置解码参数
    Config config;
    config.hwAccelType = HWAccelType::kAuto;  // 自动选择硬件加速
    config.swVideoOutFormat = ImageFormat::kRGB24;  // 输出格式
    config.requireFrameInSystemMemory = false;  // 是否要求系统内存
    
    // 打开媒体文件或流
    if (!decoder.open("rtsp://example.com/stream", config)) {
        std::cerr << "打开流失败" << std::endl;
        return -1;
    }
    
    // 开始解码
    decoder.startDecode();
    
    // 读取视频帧
    Frame videoFrame;
    if (decoder.videoQueue().pop(videoFrame, 1)) {  // 超时1ms
        // 处理视频帧
        double pts = videoFrame.secPts();
        std::cout << "视频帧PTS: " << pts << std::endl;
    }
    
    // 读取音频帧
    Frame audioFrame;
    if (decoder.audioQueue().pop(audioFrame, 1)) {
        // 处理音频帧
        double pts = audioFrame.secPts();
        std::cout << "音频帧PTS: " << pts << std::endl;
    }
    
    // 停止解码并关闭
    decoder.stopDecode();
    decoder.close();
    
    return 0;
}
```

### 事件系统

```cpp
// 注册全局事件监听器
decoder.addGlobalEventListener([](EventType eventType, std::shared_ptr<EventArgs> event) {
    std::cout << "事件: " << getEventTypeName(eventType) 
              << ", 描述: " << event->description << std::endl;
});

// 注册特定事件监听器
decoder.addEventListener(EventType::kStreamOpened, [](std::shared_ptr<EventArgs> event) {
    std::cout << "流已打开" << std::endl;
});

decoder.addEventListener(EventType::kDecodeError, [](std::shared_ptr<EventArgs> event) {
    std::cout << "解码错误: " << event->errorMessage << std::endl;
});
```

### 录制功能

```cpp
// 开始录制
decoder.startRecording("output.mp4");

// 停止录制
decoder.stopRecording();
```

### 播放控制

```cpp
// 设置播放速度
decoder.setSpeed(2.0f);  // 2倍速播放

// 启用帧率控制
decoder.setFrameRateControl(true);

// 跳转到指定位置
decoder.seek(30.0);  // 跳转到30秒位置
```

### 硬件加速配置

```cpp
Config config;

// 自动选择最佳硬件加速
config.hwAccelType = HWAccelType::kAuto;

// 指定特定硬件加速
config.hwAccelType = HWAccelType::kCuda;     // CUDA
config.hwAccelType = HWAccelType::kD3d11va;  // D3D11VA
config.hwAccelType = HWAccelType::kVaapi;    // VAAPI

// 禁用硬件加速
config.hwAccelType = HWAccelType::kNone;
```

## 示例程序

### 控制台示例
位于 `examples/console/` 目录，演示基本的解码功能：
```bash
# 编译控制台示例
cmake .. -DBUILD_CONSOLE_EXAMPLE=ON
make

# 运行示例
./decoder_usage_example rtsp://example.com/stream
```

### Qt GUI示例
位于 `examples/qt_gui/` 目录，提供图形界面播放器：
```bash
# 编译Qt GUI示例
cmake .. -DBUILD_QT_GUI_EXAMPLE=ON
make

# 运行示例
./qt_gui_example
```

### D3D渲染示例
位于 `examples/d3d_render_example/` 目录，演示D3D硬件渲染：
```bash
# 编译D3D渲染示例（仅Windows）
cmake .. -DBUILD_D3D_RENDER_EXAMPLE=ON
make

# 运行示例
./d3d_render_example
```

## 配置文件

SDK支持通过JSON配置文件进行配置，默认配置文件位于 `resources/decoderSDK.json`：

```json
{
    "log": {
        "...": "..."
    }
}
```

## 性能优化建议

1. **硬件加速**: 优先使用硬件加速，可显著提升解码性能s
2. **内存管理**: 合理设置队列大小，避免内存占用过高
3. **线程配置**: 根据CPU核心数配置解码线程数
4. **缓冲策略**: 根据网络条件调整预缓冲大小
5. **格式选择**: 选择合适的输出格式，减少格式转换开销

## 故障排除

### 常见问题

1. **编译错误**: 确保FFmpeg路径正确设置
2. **运行时错误**: 检查依赖库是否正确安装
3. **硬件加速失败**: 确认硬件驱动程序已正确安装
4. **内存泄漏**: 确保正确调用close()方法释放资源

### 调试方法

1. 启用详细日志输出
2. 使用性能统计功能监控解码状态
3. 检查事件回调中的错误信息

## 版本信息

当前版本: 1.0.0

版本检查：
```cpp
int major, minor, patch, build;
getVersion(&major, &minor, &patch, &build);
std::cout << "SDK版本: " << major << "." << minor << "." << patch << "." << build << std::endl;

// 检查版本兼容性
if (checkVersion(1, 0, 0)) {
    std::cout << "版本兼容" << std::endl;
}
```

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。

### 第三方库许可证

本项目使用了以下第三方库，它们各自遵循相应的许可证：

- **FFmpeg** — [LGPL-2.1 或 GPL-2.0](https://ffmpeg.org/legal.html)
- **eventpp** — MIT License
- **fmt** — MIT License
- **magic_enum** — MIT License
- **nlohmann/json** — MIT License
- **spdlog** — MIT License

## 贡献指南

欢迎提交Issue和Pull Request来改进本项目。

### 开发环境设置

1. 安装必要的开发工具和依赖库
2. 遵循项目的代码风格（使用提供的.clang-format配置）
3. 确保所有测试通过
4. 提交前请运行代码格式化

### 代码风格

项目使用clang-format进行代码格式化，配置文件为 `.clang-format`。

## 更新日志

### v1.0.0 (2025-08-08)
- 初始版本发布
- 支持多种硬件加速技术
- 完整的事件系统
- 跨平台支持
- 示例程序和文档