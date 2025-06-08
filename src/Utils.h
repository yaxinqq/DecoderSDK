#pragma once
#include <chrono>
#include <string>

#define EPSILON 1e-6
#define DOUBLEEPSILON 1e-12

namespace utils {
// 浮点数的大小比较函数
bool equal(double a, double b, double epsilon = DOUBLEEPSILON);
bool equal(float a, float b, double epsilon = EPSILON);

// a > b 返回true
bool greater(double a, double b, double epsilon = DOUBLEEPSILON);
bool greater(float a, float b, double epsilon = EPSILON);

// a >= b 返回true
bool greaterAndEqual(double a, double b, double epsilon = DOUBLEEPSILON);
bool greaterAndEqual(float a, float b, double epsilon = EPSILON);

// 高精度休眠函数
void highPrecisionSleep(double ms,
                        std::chrono::steady_clock::time_point startTime =
                            std::chrono::steady_clock::now());

// 判断一个url是否为实时流
bool isRealtime(const std::string &url);

// av error改为string
std::string avErr2Str(int errnum);
}  // namespace utils