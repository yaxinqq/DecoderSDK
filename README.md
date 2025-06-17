# DecoderSDK

一个高性能的音视频解码SDK，支持多种硬件加速技术和实时流处理。

## 项目简介

DecoderSDK是一个基于FFmpeg的现代化音视频解码库，提供了简洁易用的C++ API接口。该SDK专为高性能音视频应用设计，支持多种硬件加速技术、实时流处理、录制功能以及完善的事件系统。

## 主要特性

### 🚀 核心功能
- **多格式支持**: 支持主流音视频格式解码
- **硬件加速**: 支持CUDA、DXVA2、D3D11VA、QSV、VAAPI等多种硬件加速
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
- **跨平台**: 支持Windows、Linux等多平台

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

## 系统要求

### 编译环境
- **编译器**: 
  - Windows: MSVC 2019及以上
  - Linux: GCC 7.0及以上 或 Clang 6.0及以上
- **CMake**: 3.16及以上版本
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

```bash
# 克隆项目
git clone <repository-url>
cd DecoderSDK

# 创建构建目录
mkdir build
cd build

# 配置项目
cmake ..

# 编译
cmake --build . --config Release

# 安装（可选）
cmake --install .