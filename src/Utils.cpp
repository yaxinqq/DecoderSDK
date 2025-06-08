#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <thread>

extern "C" {
#include <libavutil/error.h>
}

namespace utils {
bool equal(double a, double b, double epsilon)
{
    return std::fabs(a - b) < epsilon;
}
bool equal(float a, float b, double epsilon)
{
    return std::fabs(a - b) < epsilon;
}

// a > b 返回true
bool greater(double a, double b, double epsilon)
{
    return a > b && (a - b) > epsilon;
}
bool greater(float a, float b, double epsilon)
{
    return a > b && (a - b) > epsilon;
}

// a >= b 返回true
bool greaterAndEqual(double a, double b, double epsilon)
{
    return greater(a, b, epsilon) || equal(a, b, epsilon);
}
bool greaterAndEqual(float a, float b, double epsilon)
{
    return greater(a, b, epsilon) || equal(a, b, epsilon);
}

void highPrecisionSleep(double ms,
                        std::chrono::steady_clock::time_point startTime)
{
    std::this_thread::sleep_until(
        startTime + std::chrono::microseconds(static_cast<int>(ms * 1000)));
    // using namespace std;
    // using namespace std::chrono;

    // static double estimate = 5e-3;
    // static double mean = 5e-3;
    // static double m2 = 0;
    // static int64_t count = 1;

    // double seconds = ms / 1000;
    // while (seconds > estimate) {
    //     auto start = high_resolution_clock::now();
    //     this_thread::sleep_for(milliseconds(1));
    //     auto end = high_resolution_clock::now();

    //     double observed = (end - start).count() / 1e9;
    //     seconds -= observed;

    //     ++count;
    //     double delta = observed - mean;
    //     mean += delta / count;
    //     m2   += delta * (observed - mean);
    //     double stddev = sqrt(m2 / (count - 1));
    //     estimate = mean + stddev;
    // }

    // // spin lock
    // auto start = high_resolution_clock::now();
    // while ((high_resolution_clock::now() - start).count() / 1e9 < seconds);
}

bool isRealtime(const std::string &url)
{
    std::string u = url;
    std::transform(u.begin(), u.end(), u.begin(), ::tolower);

    static const char *realtimePrefixes[] = {"rtsp://", "rtmp://", "udp://",
                                             "tcp://",  "srt://",  "mms://"};

    for (const char *prefix : realtimePrefixes) {
        if (u.find(prefix) == 0)
            return true;
    }

    if ((u.find("http://") == 0 || u.find("https://") == 0) &&
        (u.find(".m3u8") != std::string::npos ||
         u.find("/live/") != std::string::npos ||
         u.find("stream") != std::string::npos)) {
        return true;
    }

    return false;
}

std::string avErr2Str(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, errnum);
    return std::string(buf);
}
}  // namespace utils
