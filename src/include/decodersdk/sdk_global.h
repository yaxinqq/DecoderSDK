
#ifndef DECODER_SDK_GLOBAL_H
#define DECODER_SDK_GLOBAL_H

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

#ifndef BUILD_STATIC
#if defined(DECODER_SDK_LIB)
#define DECODER_SDK_API __declspec(dllexport)
#else
#define DECODER_SDK_API __declspec(dllimport)
#endif
#else
#define DECODER_SDK_API
#endif

#endif // DECODER_SDK_GLOBAL_H
