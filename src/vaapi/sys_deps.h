#ifndef SYSDEPS_H
#define SYSDEPS_H
extern "C" {
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include "base/base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace va_wrapper {
/* Visibility attributes */
#if defined __GNUC__ && __GNUC__ >= 4
# define DLL_PUBLIC __attribute__((visibility("default")))
# define DLL_HIDDEN __attribute__((visibility("hidden")))
#else
# define DLL_PUBLIC
# define DLL_HIDDEN
#endif

/* Helper macros */
#define U_GEN_STRING(x)                 U_GEN_STRING_I(x)
#define U_GEN_STRING_I(x)               #x
#define U_GEN_CONCAT(a1, a2)            U_GEN_CONCAT2_I(a1, a2)
#define U_GEN_CONCAT2(a1, a2)           U_GEN_CONCAT2_I(a1, a2)
#define U_GEN_CONCAT2_I(a1, a2)         a1 ## a2
#define U_GEN_CONCAT3(a1, a2, a3)       U_GEN_CONCAT3_I(a1, a2, a3)
#define U_GEN_CONCAT3_I(a1, a2, a3)     a1 ## a2 ## a3
#define U_GEN_CONCAT4(a1, a2, a3, a4)   U_GEN_CONCAT4_I(a1, a2, a3, a4)
#define U_GEN_CONCAT4_I(a1, a2, a3, a4) a1 ## a2 ## a3 ## a4
#endif // SYSDEPS_H
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END