#ifndef VAAPI_COMPAT_H
#define VAAPI_COMPAT_H


extern "C" {
#include <va/va.h>

#if VA_CHECK_VERSION(0,34,0)
# include <va/va_compat.h>
#endif
}

#include "base/base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace va_wrapper {
/* Chroma formats */
#ifndef VA_RT_FORMAT_YUV411
#define VA_RT_FORMAT_YUV411     0x00000008
#endif
#ifndef VA_RT_FORMAT_YUV400
#define VA_RT_FORMAT_YUV400     0x00000010
#endif
#ifndef VA_RT_FORMAT_RGB16
#define VA_RT_FORMAT_RGB16      0x00010000
#endif
#ifndef VA_RT_FORMAT_RGB32
#define VA_RT_FORMAT_RGB32      0x00020000
#endif

/* Profiles */
enum {
    VACompatProfileNone         = -1,
    VACompatProfileHEVCMain     = 17,
    VACompatProfileHEVCMain10   = 18,
    VACompatProfileVP9Profile0  = 19,
};
#if !VA_CHECK_VERSION(0,34,0)
#define VAProfileNone           VACompatProfileNone
#endif
#if !VA_CHECK_VERSION(0,36,1)
#define VAProfileHEVCMain       VACompatProfileHEVCMain
#define VAProfileHEVCMain10     VACompatProfileHEVCMain10
#endif
#if !VA_CHECK_VERSION(0,37,1)
#define VAProfileVP9Profile0    VACompatProfileVP9Profile0
#endif

/* Entrypoints */
enum {
    VACompatEntrypointVideoProc = 10,
};
#if !VA_CHECK_VERSION(0,34,0)
#define VAEntrypointVideoProc   VACompatEntrypointVideoProc
#endif
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // VAAPI_COMPAT_H
