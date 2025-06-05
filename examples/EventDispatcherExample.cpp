// #include "../src/EventDispatcher.h"
// #include <chrono>
// #include <iostream>
// #include <memory>
// #include <string>
// #include <thread>

// // 自定义事件参数类
// class StreamEventArgs : public EventArgs {
// public:
//     StreamEventArgs(const std::string &filePath) : filePath_(filePath)
//     {
//     }

//     std::string description() const override
//     {
//         return "StreamEventArgs: " + filePath_;
//     }

//     const std::string &getFilePath() const
//     {
//         return filePath_;
//     }

// private:
//     std::string filePath_;
// };

// class DecodeEventArgs : public EventArgs {
// public:
//     DecodeEventArgs(int frameNumber) : frameNumber_(frameNumber)
//     {
//     }

//     std::string description() const override
//     {
//         return "DecodeEventArgs: frame " + std::to_string(frameNumber_);
//     }

//     int getFrameNumber() const
//     {
//         return frameNumber_;
//     }

// private:
//     int frameNumber_;
// };

// int main()
// {
//     std::cout << "=== EventDispatcher 简化版示例 ===\n\n";

//     // 创建事件分发器
//     EventDispatcher dispatcher;

//     // 1. 注册同步监听器
//     std::cout << "1. 注册同步监听器\n";
//     auto syncHandle = dispatcher.addEventListener(
//         EventType::kStreamOpened,
//         [](EventType type, std::shared_ptr<EventArgs> args) {
//             std::cout << "[同步] 流已打开: " << args->description()
//                       << std::endl;
//         },
//         ConnectionType::Direct);

//     // 2. 注册异步监听器
//     std::cout << "2. 注册异步监听器\n";
//     auto asyncHandle = dispatcher.addEventListener(
//         EventType::kFrameDecoded,
//         [](EventType type, std::shared_ptr<EventArgs> args) {
//             std::cout << "[异步] 帧已解码: " << args->description()
//                       << std::endl;
//         },
//         ConnectionType::Queued);

//     // 3. 注册自动选择类型的监听器
//     auto autoHandle = dispatcher.addEventListener(
//         EventType::kStreamClosed,
//         [](EventType type, std::shared_ptr<EventArgs> args) {
//             std::cout << "[自动] 流已关闭: " << args->description()
//                       << std::endl;
//         },
//         ConnectionType::Auto);

//     // 4. 设置默认连接类型
//     std::cout << "\n3. 设置默认连接类型为异步\n";
//     dispatcher.setDefaultConnectionType(ConnectionType::Queued);

//     // 5. 启动异步处理
//     std::cout << "4. 启动后台异步处理\n";
//     dispatcher.startAsyncProcessing();

//     // 6. 触发事件
//     std::cout << "\n5. 触发事件\n";

//     // 同步事件
//     auto streamArgs = std::make_shared<StreamEventArgs>("test_video.mp4");
//     dispatcher.triggerEventSync(EventType::kStreamOpened, streamArgs);

//     // 异步事件
//     for (int i = 1; i <= 3; ++i) {
//         auto frameArgs = std::make_shared<DecodeEventArgs>(i);
//         dispatcher.triggerEventAsync(EventType::kFrameDecoded, frameArgs);
//     }

//     // 使用默认类型触发事件
//     auto closeArgs = std::make_shared<StreamEventArgs>("test_video.mp4");
//     dispatcher.triggerEvent(EventType::kStreamClosed, closeArgs);

//     // 7. 等待异步事件处理完成
//     std::cout << "\n6. 等待异步事件处理...\n";
//     std::this_thread::sleep_for(std::chrono::milliseconds(100));

//     // 8. 主线程手动处理异步事件
//     std::cout << "\n7. 停止后台处理，改为主线程处理\n";
//     dispatcher.stopAsyncProcessing();

//     // 再次触发异步事件
//     auto frameArgs = std::make_shared<DecodeEventArgs>(4);
//     dispatcher.triggerEventAsync(EventType::kFrameDecoded, frameArgs);

//     // 主线程处理
//     std::cout << "主线程处理异步事件:\n";
//     dispatcher.processAsyncEvents();

//     // 9. 查询监听器状态
//     std::cout << "\n8. 监听器状态查询\n";
//     std::cout << "kStreamOpened 是否有监听器: "
//               << (dispatcher.hasListeners(EventType::kStreamOpened) ? "是"
//                                                                     : "否")
//               << std::endl;
//     std::cout << "kFrameDecoded 监听器数量: "
//               << dispatcher.getListenerCount(EventType::kFrameDecoded)
//               << std::endl;
//     std::cout << "异步处理是否活跃: "
//               << (dispatcher.isAsyncProcessingActive() ? "是" : "否")
//               << std::endl;

//     // 10. 移除监听器
//     std::cout << "\n9. 移除监听器\n";
//     dispatcher.removeEventListener(EventType::kStreamOpened, syncHandle);
//     std::cout << "移除同步监听器后，kStreamOpened 是否有监听器: "
//               << (dispatcher.hasListeners(EventType::kStreamOpened) ? "是"
//                                                                     : "否")
//               << std::endl;

//     // 11. 扩展性演示 - 获取底层分发器
//     std::cout << "\n10. 扩展性演示\n";
//     auto *syncDispatcher = dispatcher.getSyncDispatcher();
//     auto *asyncQueue = dispatcher.getAsyncQueue();

//     if (syncDispatcher && asyncQueue) {
//         std::cout << "成功获取底层eventpp分发器，可进行高级扩展\n";

//         // 可以直接使用eventpp的高级功能
//         syncDispatcher->appendListener(
//             EventType::kError,
//             [](EventType type, std::shared_ptr<EventArgs> args) {
//                 std::cout << "[直接使用eventpp] 错误事件: "
//                           << args->description() << std::endl;
//             });

//         // 触发错误事件
//         auto errorArgs = std::make_shared<EventArgs>();
//         syncDispatcher->dispatch(EventType::kError, errorArgs);
//     }

//     std::cout << "\n=== 示例完成 ===\n";
//     return 0;
// }