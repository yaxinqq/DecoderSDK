#include "Utils.h"

#include <chrono>
#include <cmath>
#include <thread>

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

    void highPrecisionSleep(double ms) {
        using namespace std;
        using namespace std::chrono;

        static double estimate = 5e-3;
        static double mean = 5e-3;
        static double m2 = 0;
        static int64_t count = 1;

        double seconds = ms / 1000;
        while (seconds > estimate) {
            auto start = high_resolution_clock::now();
            this_thread::sleep_for(milliseconds(1));
            auto end = high_resolution_clock::now();

            double observed = (end - start).count() / 1e9;
            seconds -= observed;

            ++count;
            double delta = observed - mean;
            mean += delta / count;
            m2   += delta * (observed - mean);
            double stddev = sqrt(m2 / (count - 1));
            estimate = mean + stddev;
        }

        // spin lock
        auto start = high_resolution_clock::now();
        while ((high_resolution_clock::now() - start).count() / 1e9 < seconds);
    }
}
