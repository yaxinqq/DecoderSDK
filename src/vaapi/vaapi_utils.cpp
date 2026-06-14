#include "vaapi_utils.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <mutex>

#include "logger/logger.h"
#include "sys_deps.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace {
constexpr int kMaxDrmDevices = 4;

struct VaDynFunctions {
    const char *(*vaErrorStr)(VAStatus) = nullptr;
    VADisplay (*vaGetDisplayDRM)(int fd) = nullptr;
    VAStatus (*vaInitialize)(VADisplay, int *, int *) = nullptr;
    VAStatus (*vaTerminate)(VADisplay) = nullptr;
    VAStatus (*vaDestroyConfig)(VADisplay, VAConfigID) = nullptr;
    VAStatus (*vaDestroyContext)(VADisplay, VAContextID) = nullptr;
    VAStatus (*vaDestroySurfaces)(VADisplay, VASurfaceID *, int) = nullptr;
    VAStatus (*vaDestroyBuffer)(VADisplay, VABufferID) = nullptr;
    VAStatus (*vaCreateBuffer)(VADisplay, VAContextID, VABufferType, uint32_t, uint32_t, void *,
                               VABufferID *) = nullptr;
    VAStatus (*vaMapBuffer)(VADisplay, VABufferID, void **) = nullptr;
    VAStatus (*vaUnmapBuffer)(VADisplay, VABufferID) = nullptr;
    VAStatus (*vaExportSurfaceHandle)(VADisplay, VASurfaceID, uint32_t, uint32_t, void *) = nullptr;
    VAStatus (*vaSyncSurface)(VADisplay, VASurfaceID) = nullptr;
    VAStatus (*vaCreateImage)(VADisplay, VAImageFormat *, int, int, VAImage *) = nullptr;
    VAStatus (*vaGetImage)(VADisplay, VASurfaceID, int, int, uint32_t, uint32_t,
                           VAImageID) = nullptr;
    VAStatus (*vaDestroyImage)(VADisplay, VAImageID) = nullptr;
};

void *g_libva = nullptr;
void *g_libva_drm = nullptr;
VaDynFunctions g_va_funcs;
bool g_va_loaded = false;
std::once_flag g_va_once;

void *try_dlopen(const char *name)
{
    void *handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
    if (!handle)
        LOG_WARN("dlopen failed for %s: %s", name, dlerror());
    return handle;
}

void *load_symbol(void *handle, const char *symbol)
{
    if (!handle)
        return nullptr;
    return dlsym(handle, symbol);
}

void init_va_dynamic()
{
    g_libva = try_dlopen("libva.so.2");
    if (!g_libva)
        g_libva = try_dlopen("libva.so");

    g_libva_drm = try_dlopen("libva-drm.so.2");
    if (!g_libva_drm)
        g_libva_drm = try_dlopen("libva-drm.so");

    if (!g_libva || !g_libva_drm) {
        g_va_loaded = false;
        return;
    }

#define LOAD_FROM(handle, field, symbol_name)                                               \
    do {                                                                                    \
        g_va_funcs.field =                                                                  \
            reinterpret_cast<decltype(g_va_funcs.field)>(load_symbol(handle, symbol_name)); \
        if (!g_va_funcs.field) {                                                            \
            LOG_WARN("dlsym failed for %s", symbol_name);                                   \
            g_va_loaded = false;                                                            \
            return;                                                                         \
        }                                                                                   \
    } while (0)

    g_va_loaded = true;
    LOAD_FROM(g_libva, vaErrorStr, "vaErrorStr");
    LOAD_FROM(g_libva_drm, vaGetDisplayDRM, "vaGetDisplayDRM");
    LOAD_FROM(g_libva, vaInitialize, "vaInitialize");
    LOAD_FROM(g_libva, vaTerminate, "vaTerminate");
    LOAD_FROM(g_libva, vaDestroyConfig, "vaDestroyConfig");
    LOAD_FROM(g_libva, vaDestroyContext, "vaDestroyContext");
    LOAD_FROM(g_libva, vaDestroySurfaces, "vaDestroySurfaces");
    LOAD_FROM(g_libva, vaDestroyBuffer, "vaDestroyBuffer");
    LOAD_FROM(g_libva, vaCreateBuffer, "vaCreateBuffer");
    LOAD_FROM(g_libva, vaMapBuffer, "vaMapBuffer");
    LOAD_FROM(g_libva, vaUnmapBuffer, "vaUnmapBuffer");
    LOAD_FROM(g_libva, vaExportSurfaceHandle, "vaExportSurfaceHandle");
    LOAD_FROM(g_libva, vaSyncSurface, "vaSyncSurface");
    LOAD_FROM(g_libva, vaCreateImage, "vaCreateImage");
    LOAD_FROM(g_libva, vaGetImage, "vaGetImage");
    LOAD_FROM(g_libva, vaDestroyImage, "vaDestroyImage");
#undef LOAD_FROM
}

bool ensure_va_dynamic_loaded()
{
    std::call_once(g_va_once, init_va_dynamic);
    return g_va_loaded;
}

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
            vaDisplay = va_wrapper::va_dyn_get_display_drm(drmFd);
            if (!vaDisplay) {
                close(drmFd);
                fd = -1;
                return false;
            }
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
            vaDisplay = va_wrapper::va_dyn_get_display_drm(drmFd);
            if (!vaDisplay) {
                close(drmFd);
                fd = -1;
                continue;
            }
            LOG_INFO("DRM device found: %s", device_name);
            return true;
        }
    }
    LOG_WARN("failed to find DRM device");
    return false;
}
} // namespace

namespace va_wrapper {
bool vaapi_dynamic_loaded()
{
    return ensure_va_dynamic_loaded();
}

const char *va_dyn_error_str(VAStatus va_status)
{
    if (!ensure_va_dynamic_loaded())
        return "va dynamic loader not ready";
    return g_va_funcs.vaErrorStr(va_status);
}

VADisplay va_dyn_get_display_drm(int fd)
{
    if (!ensure_va_dynamic_loaded())
        return nullptr;
    return g_va_funcs.vaGetDisplayDRM(fd);
}

VAStatus va_dyn_initialize(VADisplay dpy, int *major_version, int *minor_version)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaInitialize(dpy, major_version, minor_version);
}

VAStatus va_dyn_terminate(VADisplay dpy)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaTerminate(dpy);
}

VAStatus va_dyn_destroy_config(VADisplay dpy, VAConfigID config_id)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaDestroyConfig(dpy, config_id);
}

VAStatus va_dyn_destroy_context(VADisplay dpy, VAContextID context)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaDestroyContext(dpy, context);
}

VAStatus va_dyn_destroy_surfaces(VADisplay dpy, VASurfaceID *surfaces, int num_surfaces)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaDestroySurfaces(dpy, surfaces, num_surfaces);
}

VAStatus va_dyn_destroy_buffer(VADisplay dpy, VABufferID buf_id)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaDestroyBuffer(dpy, buf_id);
}

VAStatus va_dyn_create_buffer(VADisplay dpy, VAContextID context, VABufferType type, uint32_t size,
                              uint32_t num_elements, void *data, VABufferID *buf_id)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaCreateBuffer(dpy, context, type, size, num_elements, data, buf_id);
}

VAStatus va_dyn_map_buffer(VADisplay dpy, VABufferID buf_id, void **pbuf)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaMapBuffer(dpy, buf_id, pbuf);
}

VAStatus va_dyn_unmap_buffer(VADisplay dpy, VABufferID buf_id)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaUnmapBuffer(dpy, buf_id);
}

VAStatus va_dyn_export_surface_handle(VADisplay dpy, VASurfaceID surface_id, uint32_t mem_type,
                                      uint32_t flags, void *descriptor)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaExportSurfaceHandle(dpy, surface_id, mem_type, flags, descriptor);
}

VAStatus va_dyn_sync_surface(VADisplay dpy, VASurfaceID render_target)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaSyncSurface(dpy, render_target);
}

VAStatus va_dyn_create_image(VADisplay dpy, VAImageFormat *format, int width, int height,
                             VAImage *out_image)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaCreateImage(dpy, format, width, height, out_image);
}

VAStatus va_dyn_get_image(VADisplay dpy, VASurfaceID surface, int x, int y, uint32_t width,
                          uint32_t height, VAImageID image)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaGetImage(dpy, surface, x, y, width, height, image);
}

VAStatus va_dyn_destroy_image(VADisplay dpy, VAImageID image)
{
    if (!ensure_va_dynamic_loaded())
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return g_va_funcs.vaDestroyImage(dpy, image);
}

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
            fprintf(stderr, "error: %s: %s\n", msg, va_dyn_error_str(va_status));
        return false;
    }
    return true;
}

// Destroys a VA config
void va_destroy_config(VADisplay dpy, VAConfigID *cfg_ptr)
{
    if (*cfg_ptr != VA_INVALID_ID) {
        va_dyn_destroy_config(dpy, *cfg_ptr);
        *cfg_ptr = VA_INVALID_ID;
    }
}

// Destroys a VA context
void va_destroy_context(VADisplay dpy, VAContextID *ctx_ptr)
{
    if (*ctx_ptr != VA_INVALID_ID) {
        va_dyn_destroy_context(dpy, *ctx_ptr);
        *ctx_ptr = VA_INVALID_ID;
    }
}

// Destroys a VA surface
void va_destroy_surface(VADisplay dpy, VASurfaceID *surf_ptr)
{
    if (*surf_ptr != VA_INVALID_ID) {
        va_dyn_destroy_surfaces(dpy, surf_ptr, 1);
        *surf_ptr = VA_INVALID_ID;
    }
}

// Destroys a VA buffer
void va_destroy_buffer(VADisplay dpy, VABufferID *buf_ptr)
{
    if (*buf_ptr != VA_INVALID_ID) {
        va_dyn_destroy_buffer(dpy, *buf_ptr);
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

    va_status = va_dyn_create_buffer(dpy, ctx, (VABufferType)type, static_cast<uint32_t>(size), 1,
                                     (void *)data, &buf_id);
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

    va_status = va_dyn_map_buffer(dpy, buf_id, &data);
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

    va_status = va_dyn_unmap_buffer(dpy, buf_id);
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

    if (!vaapi_dynamic_loaded()) {
        LOG_WARN("VA dynamic loader unavailable");
        return {};
    }

    if (!openDrmVADisplay(vaDisplay, fd, deviceIndex)) {
        LOG_WARN("ffva_display_drm_open failed!");
        return {};
    }

    va_status = va_dyn_initialize(vaDisplay, &major_version, &minor_version);

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
        va_dyn_terminate(vaDisplay);
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
        va_dyn_export_surface_handle(vaDisplay, vaSurfaceID, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                     VA_EXPORT_SURFACE_READ_ONLY, &desc);

    if (va_status != VA_STATUS_SUCCESS) {
        return {};
    }

    return desc;
}

void syncVASurface(VADisplay vaDisplay, VASurfaceID vaSurfaceID)
{
    va_dyn_sync_surface(vaDisplay, vaSurfaceID);
}
} // namespace va_wrapper

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END
