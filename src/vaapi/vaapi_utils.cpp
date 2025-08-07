#include "vaapi_utils.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
#include <va/va_drm.h>
}

#include "logger/logger.h"
#include "sys_deps.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
constexpr int kMaxDrmDevices = 4;
bool openDrmVADisplay(VADisplay &vaDisplay, int &fd, int deviceIndex = 0)
{
    char device_name[128] = "";

    int i, drmFd = -1;
    int ret;

    fd = drmFd;

    // 先验证传入的deviceIndex是否可用，如果可用就返回，不可用就继续遍历
    if (deviceIndex >= 0 && deviceIndex < kMaxDrmDevices) {
        snprintf(device_name, sizeof(device_name), "/dev/dri/renderD%d", deviceIndex + 0x80);
        drmFd = open(device_name, O_RDWR | O_CLOEXEC);
        if (drmFd >= 0) {
            fd = drmFd;
            vaDisplay = vaGetDisplayDRM(drmFd);
            LOG_INFO("DRM device found: %s", device_name);
            return true;
        }
    }

    /* Try render nodes first, i.e. /dev/dri/renderD<nnn> then try to
       fallback to older gfx device nodes */
    for (i = 0; i < 2 * kMaxDrmDevices; i++) {
        const int dn = i >> 1;
        const int rn = !(i & 1);

        ret = snprintf(device_name, sizeof(device_name), "/dev/dri/%s%d", rn ? "renderD" : "card",
                       dn + rn * 0x80);
        if (ret < 0 || ret >= sizeof(device_name))
            return false;

        drmFd = open(device_name, O_RDWR | O_CLOEXEC);
        if (drmFd >= 0) {
            fd = drmFd;
            vaDisplay = vaGetDisplayDRM(drmFd);
            LOG_INFO("DRM device found: %s", device_name);
            return true;
        }
    }
    LOG_WARN("failed to find DRM device");
    return false;
}
} // namespace

namespace va_wrapper {
// Checks whether the VA status error needs to be printed out
bool va_check_status_is_quiet(VAStatus va_status)
{
    /* Only "unimplemented" status are quietly ignored */
    return va_status == VA_STATUS_ERROR_UNIMPLEMENTED;
}

// Checks the VA status
bool va_check_status(VAStatus va_status, const char *msg)
{
    if (va_status != VA_STATUS_SUCCESS) {
        if (!va_check_status_is_quiet(va_status))
            fprintf(stderr, "error: %s: %s\n", msg, vaErrorStr(va_status));
        return false;
    }
    return true;
}

// Destroys a VA config
void va_destroy_config(VADisplay dpy, VAConfigID *cfg_ptr)
{
    if (*cfg_ptr != VA_INVALID_ID) {
        vaDestroyConfig(dpy, *cfg_ptr);
        *cfg_ptr = VA_INVALID_ID;
    }
}

// Destroys a VA context
void va_destroy_context(VADisplay dpy, VAContextID *ctx_ptr)
{
    if (*ctx_ptr != VA_INVALID_ID) {
        vaDestroyContext(dpy, *ctx_ptr);
        *ctx_ptr = VA_INVALID_ID;
    }
}

// Destroys a VA surface
void va_destroy_surface(VADisplay dpy, VASurfaceID *surf_ptr)
{
    if (*surf_ptr != VA_INVALID_ID) {
        vaDestroySurfaces(dpy, surf_ptr, 1);
        *surf_ptr = VA_INVALID_ID;
    }
}

// Destroys a VA buffer
void va_destroy_buffer(VADisplay dpy, VABufferID *buf_ptr)
{
    if (*buf_ptr != VA_INVALID_ID) {
        vaDestroyBuffer(dpy, *buf_ptr);
        *buf_ptr = VA_INVALID_ID;
    }
}

// Destroys an array of VA buffers
void va_destroy_buffers(VADisplay dpy, VABufferID *buf, uint32_t *len_ptr)
{
    uint32_t i, num_buffers = *len_ptr;

    if (buf) {
        for (i = 0; i < num_buffers; i++)
            va_destroy_buffer(dpy, &buf[i]);
    }
    *len_ptr = 0;
}

// Creates and maps VA buffer
bool va_create_buffer(VADisplay dpy, VAContextID ctx, int type, size_t size, const void *data,
                      VABufferID *buf_id_ptr, void **mapped_data_ptr)
{
    VABufferID buf_id;
    VAStatus va_status;

    va_status = vaCreateBuffer(dpy, ctx, (VABufferType)type, size, 1, (void *)data, &buf_id);
    if (!va_check_status(va_status, "vaCreateBuffer()"))
        return false;

    if (mapped_data_ptr) {
        data = va_map_buffer(dpy, buf_id);
        if (!data)
            goto error;
        *mapped_data_ptr = (void *)data;
    }

    *buf_id_ptr = buf_id;
    return true;

error:
    va_destroy_buffer(dpy, &buf_id);
    return false;
}

// Maps the specified VA buffer
void *va_map_buffer(VADisplay dpy, VABufferID buf_id)
{
    VAStatus va_status;
    void *data = NULL;

    va_status = vaMapBuffer(dpy, buf_id, &data);
    if (!va_check_status(va_status, "vaMapBuffer()"))
        return NULL;
    return data;
}

// Unmaps the supplied VA buffer. Sets the (optional) data pointer to NULL
void va_unmap_buffer(VADisplay dpy, VABufferID buf_id, void **buf_ptr)
{
    VAStatus va_status;

    if (buf_ptr)
        *buf_ptr = NULL;

    va_status = vaUnmapBuffer(dpy, buf_id);
    if (!va_check_status(va_status, "vaUnmapBuffer()"))
        return;
}

// Initializes image with safe default values
void va_image_init_defaults(VAImage *image)
{
    if (!image)
        return;
    image->image_id = VA_INVALID_ID;
    image->buf = VA_INVALID_ID;
}

VADisplay createDrmVADisplay(int &fd, int deviceIndex)
{
    VADisplay vaDisplay;

    int major_version, minor_version;
    VAStatus va_status;

    if (!openDrmVADisplay(vaDisplay, fd, deviceIndex)) {
        LOG_WARN("ffva_display_drm_open failed!");
        return {};
    }

    va_status = vaInitialize(vaDisplay, &major_version, &minor_version);

    if (!va_check_status(va_status, "vaInitialize()")) {
        LOG_WARN("vaInitialize failed!");
        if (vaDisplay) {
            destoryDrmVADisplay(vaDisplay, fd);
            fd = -1;
        }
        return {};
    }

    return vaDisplay;
}

void destoryDrmVADisplay(VADisplay &vaDisplay, int &fd)
{
    if (vaDisplay) {
        vaTerminate(vaDisplay);
        vaDisplay = nullptr;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

VADRMPRIMESurfaceDescriptor exportVASurfaceHandle(VADisplay vaDisplay, VASurfaceID vaSurfaceID)
{
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    VAStatus va_status =
        vaExportSurfaceHandle(vaDisplay, vaSurfaceID, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_WRITE, &desc);

    if (va_status != VA_STATUS_SUCCESS) {
        return {};
    }

    return desc;
}

void syncVASurface(VADisplay vaDisplay, VASurfaceID vaSurfaceID)
{
    vaSyncSurface(vaDisplay, vaSurfaceID);
}
} // namespace va_wrapper

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END