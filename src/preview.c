#define _POSIX_C_SOURCE 200809L
#include "preview.h"
#include "wayland.h"
#include "logger/logger.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

struct preview_cache_entry {
    char *address;
    cairo_surface_t *surface;
    int shm_fd;
    void *shm_data;
    size_t shm_size;
    int width;
    int height;
    int stride;
    uint32_t format;
    struct hyprland_toplevel_export_frame_v1 *frame;
    struct wl_buffer *buffer;
    bool done;
    bool failed;
    struct preview_cache_entry *next;
};

static struct preview_cache_entry *cache_head = NULL;

/* Stub to satisfy linker, as we don't use capture_toplevel_with_wlr_toplevel_handle */
const struct wl_interface zwlr_foreign_toplevel_handle_v1_interface = {
    "zwlr_foreign_toplevel_handle_v1", 1, 0, NULL, 0, NULL
};

extern int mkstemp(char *template);

static int create_shm_file(size_t size) {
    char name[] = "/tmp/hyprswitcher-preview-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
        LOG_ERROR("[PREVIEW] shm mkstemp failed");
        return -1;
    }
    unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        LOG_ERROR("[PREVIEW] shm ftruncate failed");
        return -1;
    }
    return fd;
}

static void frame_handle_buffer(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame,
    uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
    (void)frame;
    struct preview_cache_entry *entry = data;
    entry->width = width;
    entry->height = height;
    entry->stride = stride;
    entry->format = format;
}

static void frame_handle_damage(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    (void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void frame_handle_flags(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame,
    uint32_t flags)
{
    (void)data; (void)frame; (void)flags;
}

static void frame_handle_ready(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    struct preview_cache_entry *entry = data;
    
    /* Determine cairo format based on shm format */
    cairo_format_t cairo_fmt = CAIRO_FORMAT_ARGB32;
    if (entry->format == WL_SHM_FORMAT_XRGB8888) {
        cairo_fmt = CAIRO_FORMAT_RGB24;
    } else if (entry->format != WL_SHM_FORMAT_ARGB8888) {
        LOG_WARN("[PREVIEW] Unhandled format %u for %s, falling back to ARGB32", entry->format, entry->address);
    }

    entry->surface = cairo_image_surface_create_for_data(
        entry->shm_data, cairo_fmt, entry->width, entry->height, entry->stride);
    
    if (cairo_surface_status(entry->surface) != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("[PREVIEW] Failed to create cairo surface for %s", entry->address);
        cairo_surface_destroy(entry->surface);
        entry->surface = NULL;
        entry->failed = true;
    } else {
        LOG_DEBUG("[PREVIEW] Capture ready for %s", entry->address);
    }
    
    entry->done = true;
    hyprland_toplevel_export_frame_v1_destroy(entry->frame);
    entry->frame = NULL;
    wayland_request_redraw();
}

static void frame_handle_failed(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame)
{
    (void)frame;
    struct preview_cache_entry *entry = data;
    LOG_WARN("[PREVIEW] Capture failed for %s", entry->address);
    entry->failed = true;
    entry->done = true;
    hyprland_toplevel_export_frame_v1_destroy(entry->frame);
    entry->frame = NULL;
    wayland_request_redraw();
}

static void frame_handle_linux_dmabuf(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame,
    uint32_t format, uint32_t width, uint32_t height)
{
    (void)data; (void)frame; (void)format; (void)width; (void)height;
}

static void frame_handle_buffer_done(void *data,
    struct hyprland_toplevel_export_frame_v1 *frame)
{
    struct preview_cache_entry *entry = data;
    
    if (entry->width == 0 || entry->height == 0) {
        LOG_ERROR("[PREVIEW] Invalid dimensions for %s", entry->address);
        entry->failed = true;
        return;
    }

    entry->shm_size = (size_t)entry->stride * (size_t)entry->height;
    entry->shm_fd = create_shm_file(entry->shm_size);
    if (entry->shm_fd < 0) {
        entry->failed = true;
        return;
    }

    entry->shm_data = mmap(NULL, entry->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, entry->shm_fd, 0);
    if (entry->shm_data == MAP_FAILED) {
        LOG_ERROR("[PREVIEW] mmap failed for %s", entry->address);
        close(entry->shm_fd);
        entry->shm_fd = -1;
        entry->failed = true;
        return;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(get_shm(), entry->shm_fd, entry->shm_size);
    entry->buffer = wl_shm_pool_create_buffer(pool, 0, entry->width, entry->height, entry->stride, entry->format);
    wl_shm_pool_destroy(pool);

    hyprland_toplevel_export_frame_v1_copy(frame, entry->buffer, 1);
}

static const struct hyprland_toplevel_export_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .damage = frame_handle_damage,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .linux_dmabuf = frame_handle_linux_dmabuf,
    .buffer_done = frame_handle_buffer_done,
};

void preview_capture_window(const char *address) {
    if (!address) return;
    
    struct hyprland_toplevel_export_manager_v1 *manager = get_toplevel_export_manager();
    if (!manager) return;

    /* Check if already capturing */
    struct preview_cache_entry *curr = cache_head;
    while (curr) {
        if (strcmp(curr->address, address) == 0) {
            return; /* already tracking this window */
        }
        curr = curr->next;
    }

    struct preview_cache_entry *entry = calloc(1, sizeof(*entry));
    entry->address = strdup(address);
    entry->shm_fd = -1;
    
    /* Address like "0x559e21...". Convert to integer handle.
     * protocol expects uint32_t handle */
    uint32_t handle = (uint32_t)strtoull(address, NULL, 16);

    entry->frame = hyprland_toplevel_export_manager_v1_capture_toplevel(manager, 0, handle);
    hyprland_toplevel_export_frame_v1_add_listener(entry->frame, &frame_listener, entry);

    entry->next = cache_head;
    cache_head = entry;
    
    LOG_DEBUG("[PREVIEW] Requested capture for %s (handle: %u)", address, handle);
}

cairo_surface_t *preview_get_surface(const char *address) {
    if (!address) return NULL;
    struct preview_cache_entry *curr = cache_head;
    while (curr) {
        if (strcmp(curr->address, address) == 0) {
            return curr->surface;
        }
        curr = curr->next;
    }
    return NULL;
}

void preview_cleanup_all(void) {
    struct preview_cache_entry *curr = cache_head;
    while (curr) {
        struct preview_cache_entry *next = curr->next;
        
        if (curr->surface) {
            cairo_surface_destroy(curr->surface);
        }
        if (curr->buffer) {
            wl_buffer_destroy(curr->buffer);
        }
        if (curr->shm_data && curr->shm_data != MAP_FAILED) {
            munmap(curr->shm_data, curr->shm_size);
        }
        if (curr->shm_fd >= 0) {
            close(curr->shm_fd);
        }
        if (curr->frame) {
            hyprland_toplevel_export_frame_v1_destroy(curr->frame);
        }
        free(curr->address);
        free(curr);
        
        curr = next;
    }
    cache_head = NULL;
}
