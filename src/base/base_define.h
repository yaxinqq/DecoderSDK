#ifndef DECODER_SDK_INTERNAL_BASE_DEFINE_H
#define DECODER_SDK_INTERNAL_BASE_DEFINE_H

#include "include/decodersdk/sdk_global.h"

#ifndef DECODER_SDK_NAMESPACE_BEGIN
#define DECODER_SDK_NAMESPACE_BEGIN namespace decoder_sdk {
#define DECODER_SDK_NAMESPACE_END }
#endif

#ifndef INTERNAL_NAMESPACE_BEGIN
#define INTERNAL_NAMESPACE_BEGIN namespace internal {
#define INTERNAL_NAMESPACE_END }
#endif

#if defined(__cplusplus) && (__cplusplus >= 201703L)

#if defined(__clang__)
#if __clang_major__ >= 7
#define DECODER_SDK_MAGIC_ENUM_SUPPORTED
#endif
#elif defined(__GNUC__)
#if (__GNUC__ >=9)
#define DECODER_SDK_MAGIC_ENUM_SUPPORTED
#endif
#elif defined(_MSC_VER)
#if _MSC_VER >= 1910 // VS2017+
#define DECODER_SDK_MAGIC_ENUM_SUPPORTED
#endif
#endif

#endif

#endif // DECODER_SDK_INTERNAL_BASE_DEFINE_H
