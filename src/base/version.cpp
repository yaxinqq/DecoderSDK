#include "version.h"

#include <iomanip>
#include <sstream>

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

const char *getVersionString()
{
    return DECODER_SDK_VERSION_STRING;
}

const char *getVersionStringFull()
{
    return DECODER_SDK_VERSION_STRING_FULL;
}

const char *getBuildInfo()
{
    static std::string buildInfo;
    if (buildInfo.empty()) {
        std::ostringstream oss;
        oss << "DecoderSDK " << DECODER_SDK_VERSION_STRING_FULL << " ("
            << DECODER_SDK_BUILD_CONFIG << ")\n"
            << "Built on " << DECODER_SDK_BUILD_DATE << " "
            << DECODER_SDK_BUILD_TIME << "\n"
            << "Platform: " << DECODER_SDK_PLATFORM << "\n"
            << "Compiler: " << DECODER_SDK_COMPILER << " ("
            << DECODER_SDK_COMPILER_VERSION << ")";
        buildInfo = oss.str();
    }
    return buildInfo.c_str();
}

void getVersion(int *major, int *minor, int *patch, int *build)
{
    if (major)
        *major = DECODER_SDK_VERSION_MAJOR;
    if (minor)
        *minor = DECODER_SDK_VERSION_MINOR;
    if (patch)
        *patch = DECODER_SDK_VERSION_PATCH;
    if (build)
        *build = DECODER_SDK_VERSION_BUILD;
}

int checkVersion(int major, int minor, int patch)
{
    return DECODER_SDK_VERSION_CHECK(major, minor, patch) ? 1 : 0;
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END