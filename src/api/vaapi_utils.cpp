#ifdef VAAPI_AVAILABLE
#include "include/decodersdk/vaapi_utils.h"
#include "logger/logger.h"
#include "vaapi/vaapi_utils.h"

namespace decoder_sdk {
VADisplay createDrmVADisplay(int &fd)
{
    return internal::va_wrapper::createDrmVADisplay(fd);
}

void destoryDrmVADisplay(VADisplay &vaDisplay, int &fd)
{
    internal::va_wrapper::destoryDrmVADisplay(vaDisplay, fd);
}

VADRMPRIMESurfaceDescriptor exportVASurfaceHandle(VADisplay vaDisplay, VASurfaceID vaSurfaceID)
{
    return internal::va_wrapper::exportVASurfaceHandle(vaDisplay, vaSurfaceID);
}

void syncVASurface(VADisplay vaDisplay, VASurfaceID vaSurfaceID)
{
    internal::va_wrapper::syncVASurface(vaDisplay, vaSurfaceID);
}   
}
#endif