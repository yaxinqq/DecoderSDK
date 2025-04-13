#ifndef DECODER_SDK_GLOBAL_H
#define DECODER_SDK_GLOBAL_H

#ifndef BUILD_STATIC
# if defined(DECODER_SDK_LIB)
#  define DECODER_SDK_API __declspec(dllexport)
# else
#  define DECODER_SDK_API __declspec(dllimport)
# endif
#else
# define DECODER_SDK_API
#endif

#endif // DECODER_SDK_GLOBAL_H