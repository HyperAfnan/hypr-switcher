#include "logger/logger.h"
#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include "render.h"
#include "wayland.h"
#include "util.h"
int mkstemp(char *template);

static int create_shm_file(size_t size) {
    char name[] = "/tmp/hyprswitcher-shm-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) DIE("shm mkstemp failed\n");
    unlink(name);
    if (ftruncate(fd, size) < 0) DIE("shm truncate failed\n");
    return fd;
}

/* Draw a rounded rectangle path at (x,y) of size (w,h) with radius r */
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    double x0 = x, y0 = y;
    double x1 = x + w, y1 = y + h;
    double radius = r;
    if (radius < 0) radius = 0;
    if (radius > w / 2.0) radius = w / 2.0;
    if (radius > h / 2.0) radius = h / 2.0;

    const double PI = 3.14159265358979323846;
    const double HALF_PI = PI / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x1 - radius, y0 + radius, radius, -HALF_PI, 0.0);
    cairo_arc(cr, x1 - radius, y1 - radius, radius, 0.0, HALF_PI);
    cairo_arc(cr, x0 + radius, y1 - radius, radius, HALF_PI, PI);
    cairo_arc(cr, x0 + radius, y0 + radius, radius, PI, 3.0 * HALF_PI);
    cairo_close_path(cr);
}

void render_draw(struct wl_surface *surface, int width, int height) {
    int stride = width * 4;
    int size = stride * height;

    int fd = create_shm_file(size);
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        LOG_DEBUG("shm mmap failed for %dx%d (size=%d)", width, height, size);
        close(fd);
        DIE("shm mmap failed\n");
        return;
    }

    cairo_surface_t *cairosurf = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, stride);
    if (cairo_surface_status(cairosurf) != CAIRO_STATUS_SUCCESS) {
        LOG_DEBUG("cairo_image_surface_create_for_data failed");
    }
    cairo_t *cr = cairo_create(cairosurf);

    LOG_DEBUG("Rendering surface with dimensions %dx%d", width, height);

    /* Full-surface background */
    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.85);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Label */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, "Hypr Switcher Initialized", -1);
    PangoFontDescription *font = pango_font_description_from_string("Sans 24");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    /* Get text size in pixels (Pango reports in PANGO_SCALE units) */
    int pw, ph;
    pango_layout_get_size(layout, &pw, &ph);
    double text_w = pw / (double)PANGO_SCALE;
    double text_h = ph / (double)PANGO_SCALE;

    double text_x = (width  - text_w) / 2.0;
    double text_y = (height - text_h) / 2.0;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, text_x, text_y);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font);
    g_object_unref(layout);

    cairo_destroy(cr);
    cairo_surface_flush(cairosurf);
    cairo_surface_destroy(cairosurf);

    struct wl_shm *shm = get_shm();
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                  WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

void render_draw_titles(struct wl_surface *surface, int width, int height, const char **titles, size_t count) {
    int stride = width * 4;
    int size = stride * height;

    int fd = create_shm_file(size);
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        LOG_DEBUG("shm mmap failed for %dx%d (size=%d)", width, height, size);
        close(fd);
        DIE("shm mmap failed\n");
        return;
    }

    cairo_surface_t *cairosurf = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, stride);
    if (cairo_surface_status(cairosurf) != CAIRO_STATUS_SUCCESS) {
        LOG_DEBUG("cairo_image_surface_create_for_data failed");
    }
    cairo_t *cr = cairo_create(cairosurf);

    LOG_DEBUG("Rendering titles: %zu items on %dx%d", count, width, height);

    /* Background */
    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.85);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Text layout setup */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans 18");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout, 1);

    int padding = 12;

    /* Estimate line height */
    pango_layout_set_text(layout, "Mg", -1);
    int pw, ph;
    pango_layout_get_size(layout, &pw, &ph);
    int line_h = (int)(ph / (double)PANGO_SCALE);
    if (line_h < 20) line_h = 20;
    line_h += 8;

    int content_w = width - 2 * padding;
    if (content_w < 1) content_w = width;
    pango_layout_set_width(layout, content_w * PANGO_SCALE);

    /* Draw either list of titles or placeholder when empty */
    cairo_set_source_rgb(cr, 1, 1, 1);

    if (count > 0) {
        size_t max_lines = 0;
        if (line_h > 0 && height > 2 * padding) {
            max_lines = (size_t)((height - 2 * padding) / line_h);
        }
        size_t lines_to_draw = count;
        if (max_lines > 0 && lines_to_draw > max_lines) {
            lines_to_draw = max_lines;
        }

        for (size_t i = 0; i < lines_to_draw; i++) {
            const char *text = (titles && titles[i]) ? titles[i] : "(untitled)";
            pango_layout_set_text(layout, text, -1);

            double base_x = padding;
            double base_y = padding + (int)i * line_h;

            PangoRectangle ink, logical;
            pango_layout_get_pixel_extents(layout, &ink, &logical);

            double pad_x = 8.0;
            double pad_y = 4.0;

            double rx = base_x + ink.x - pad_x;
            double ry = base_y + ink.y - pad_y;
            double rw = ink.width + 2.0 * pad_x;
            double rh = ink.height + 2.0 * pad_y;

            /* Pixel-align for crisp 1px stroke */
            rx += 0.5;
            ry += 0.5;
            if (rw > 1.0) rw -= 1.0;
            if (rh > 1.0) rh -= 1.0;

            cairo_set_source_rgba(cr, 0.7, 0.7, 0.75, 0.8);
            cairo_set_line_width(cr, 1.0);
            rounded_rect(cr, rx, ry, rw, rh, 5.0);
            cairo_stroke(cr);

            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, base_x, base_y);
            pango_cairo_show_layout(cr, layout);
        }
    } else {
        /* Centered placeholder */
        const char *msg = "No windows open";
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, msg, -1);

        int tw, th;
        pango_layout_get_size(layout, &tw, &th);
        double text_w = tw / (double)PANGO_SCALE;
        double text_h = th / (double)PANGO_SCALE;

        double text_x = (width  - text_w) / 2.0;
        double text_y = (height - text_h) / 2.0;

        cairo_move_to(cr, text_x, text_y);
        pango_cairo_show_layout(cr, layout);
    }

    pango_font_description_free(font);
    g_object_unref(layout);

    cairo_destroy(cr);
    cairo_surface_flush(cairosurf);
    cairo_surface_destroy(cairosurf);

    struct wl_shm *shm = get_shm();
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                  WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

/* Focus-aware variant: highlights the line at focused_index (>=0) */
void render_draw_titles_focus(struct wl_surface *surface, int width, int height,
                              const char **titles, size_t count, int focused_index) {
    int stride = width * 4;
    int size = stride * height;

    int fd = create_shm_file(size);
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        LOG_DEBUG("shm mmap failed for %dx%d (size=%d)", width, height, size);
        close(fd);
        DIE("shm mmap failed\n");
        return;
    }

    cairo_surface_t *cairosurf = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, width, height, stride);
    if (cairo_surface_status(cairosurf) != CAIRO_STATUS_SUCCESS) {
        LOG_DEBUG("cairo_image_surface_create_for_data failed");
    }
    cairo_t *cr = cairo_create(cairosurf);

    LOG_DEBUG("Rendering titles (focus variant): %zu items, focus=%d on %dx%d",
              count, focused_index, width, height);

    /* Background */
    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.90);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    /* Text layout setup */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans 18");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout, 1);

    int padding = 12;

    /* Estimate line height */
    pango_layout_set_text(layout, "Mg", -1);
    int pw, ph;
    pango_layout_get_size(layout, &pw, &ph);
    int line_h = (int)(ph / (double)PANGO_SCALE);
    if (line_h < 20) line_h = 20;
    line_h += 8;

    int content_w = width - 2 * padding;
    if (content_w < 1) content_w = width;
    pango_layout_set_width(layout, content_w * PANGO_SCALE);

    if (count > 0) {
        size_t max_lines = 0;
        if (line_h > 0 && height > 2 * padding) {
            max_lines = (size_t)((height - 2 * padding) / line_h);
        }
        size_t lines_to_draw = count;
        if (max_lines > 0 && lines_to_draw > max_lines) {
            lines_to_draw = max_lines;
        }

        for (size_t i = 0; i < lines_to_draw; i++) {
            const char *text = (titles && titles[i]) ? titles[i] : "(untitled)";
            pango_layout_set_text(layout, text, -1);
            double base_x = padding;
            double base_y = padding + (int)i * line_h;

            PangoRectangle ink, logical;
            pango_layout_get_pixel_extents(layout, &ink, &logical);

            double pad_x = 8.0;
            double pad_y = 4.0;

            double rx = base_x + ink.x - pad_x;
            double ry = base_y + ink.y - pad_y;
            double rw = ink.width + 2.0 * pad_x;
            double rh = ink.height + 2.0 * pad_y;

            /* Pixel-align */
            rx += 0.5;
            ry += 0.5;
            if (rw > 1.0) rw -= 1.0;
            if (rh > 1.0) rh -= 1.0;

            int is_focused = ((int)i == focused_index);

            if (is_focused) {
                /* Optional subtle fill */
                cairo_set_source_rgba(cr, 0.28, 0.60, 0.90, 0.20);
                rounded_rect(cr, rx, ry, rw, rh, 5.0);
                cairo_fill(cr);

                cairo_set_source_rgba(cr, 0.30, 0.65, 0.95, 0.95);
                cairo_set_line_width(cr, 2.0);
            } else {
                cairo_set_source_rgba(cr, 0.7, 0.7, 0.75, 0.8);
                cairo_set_line_width(cr, 1.0);
            }
            rounded_rect(cr, rx, ry, rw, rh, 5.0);
            cairo_stroke(cr);

            /* Text */
            if (is_focused) {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
            }
            cairo_move_to(cr, base_x, base_y);
            pango_cairo_show_layout(cr, layout);
        }
    } else {
        /* Placeholder */
        const char *msg = "No windows open";
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, msg, -1);

        int tw, th;
        pango_layout_get_size(layout, &tw, &th);
        double text_w = tw / (double)PANGO_SCALE;
        double text_h = th / (double)PANGO_SCALE;

        double text_x = (width  - text_w) / 2.0;
        double text_y = (height - text_h) / 2.0;

        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, text_x, text_y);
        pango_cairo_show_layout(cr, layout);
    }

    pango_font_description_free(font);
    g_object_unref(layout);

    cairo_destroy(cr);
    cairo_surface_flush(cairosurf);
    cairo_surface_destroy(cairosurf);

    struct wl_shm *shm = get_shm();
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                  WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}
