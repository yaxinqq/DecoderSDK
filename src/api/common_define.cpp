#include "include/decodersdk/common_define.h"

#include "base/version.h"
#include "magic_enum/magic_enum.hpp"

namespace decoder_sdk {
const char *getVersionString()
{
    return internal::getVersionString();
}

const char *getVersionStringFull()
{
    return internal::getVersionStringFull();
}

const char *getBuildInfo()
{
    return internal::getBuildInfo();
}

void getVersion(int *major, int *minor, int *patch, int *build)
{
    return internal::getVersion(major, minor, patch, build);
}

int checkVersion(int major, int minor, int patch)
{
    return internal::checkVersion(major, minor, patch);
}

std::vector<EventType> allEventTypes()
{
    std::vector<EventType> types;
    for (const auto &type : magic_enum::enum_values<EventType>()) {
        types.emplace_back(type);
    }

    return types;
}

std::string getEventTypeName(EventType type)
{
    if (!magic_enum::enum_contains(type)) {
        return {};
    }

    return std::string(magic_enum::enum_name(type));
}
} // namespace decoder_sdk