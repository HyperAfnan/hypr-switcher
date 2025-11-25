#define _POSIX_C_SOURCE 200112L

#include "render.h"
#include "config.h"
#include "wayland.h"
#include "logger/logger.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>

/* Forward declaration for mkstemp */
int mkstemp(char *template);

/* ============================================================================
 * Buffer Management
 * ============================================================================ */

/*
 * Create an anonymous shared memory file for Wayland buffer.
 * Returns file descriptor on success, calls DIE on failure.
 */
static int create_shm_file(size_t size) {
    char name[] = "/tmp/hyprswitcher-shm-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
        DIE("shm mkstemp failed\n");
    }
    unlink(name);
    
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        DIE("shm ftruncate failed\n");
    }
    
    return fd;
}

/*
 * Render context - holds all resources for a single render pass.
 * This encapsulates buffer creation and cleanup.
 */
typedef struct {
    int fd;
    void *data;
    size_t size;
    int width;
    int height;
    int stride;
    cairo_surface_t *cairo_surface;
    cairo_t *cr;
    struct wl_buffer *buffer;
    bool valid;
} RenderContext;

/*
 * Initialize a render context for the given dimensions.
 * Returns true on success.
 */
static bool render_context_init(RenderContext *ctx, int width, int height) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->width = width;
    ctx->height = height;
    ctx->stride = width * 4;
    ctx->size = (size_t)(ctx->stride * height);
    ctx->valid = false;
    
    /* Create shared memory */
    ctx->fd = create_shm_file(ctx->size);
    if (ctx->fd < 0) {
        return false;
    }
    
    /* Map memory */
    ctx->data = mmap(NULL, ctx->size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, 0);
    if (ctx->data == MAP_FAILED) {
        LOG_ERROR("[RENDER] mmap failed for %dx%d (size=%zu)", width, height, ctx->size);
        close(ctx->fd);
        return false;
    }
    
    /* Create cairo surface */
    ctx->cairo_surface = cairo_image_surface_create_for_data(
        ctx->data, CAIRO_FORMAT_ARGB32, width, height, ctx->stride);
    
    if (cairo_surface_status(ctx->cairo_surface) != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("[RENDER] cairo_image_surface_create_for_data failed");
        munmap(ctx->data, ctx->size);
        close(ctx->fd);
        return false;
    }
    
    /* Create cairo context */
    ctx->cr = cairo_create(ctx->cairo_surface);
    if (cairo_status(ctx->cr) != CAIRO_STATUS_SUCCESS) {
        LOG_ERROR("[RENDER] cairo_create failed");
        cairo_surface_destroy(ctx->cairo_surface);
        munmap(ctx->data, ctx->size);
        close(ctx->fd);
        return false;
    }
    
    ctx->valid = true;
    return true;
}

/*
 * Finish rendering and commit to Wayland surface.
 * Cleans up cairo resources but keeps buffer valid for Wayland.
 */
static void render_context_commit(RenderContext *ctx, struct wl_surface *surface) {
    if (!ctx->valid || !surface) {
        return;
    }
    
    /* Flush cairo drawing */
    cairo_destroy(ctx->cr);
    ctx->cr = NULL;
    
    cairo_surface_flush(ctx->cairo_surface);
    cairo_surface_destroy(ctx->cairo_surface);
    ctx->cairo_surface = NULL;
    
    /* Create Wayland buffer */
    struct wl_shm *shm = get_shm();
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, ctx->fd, (int32_t)ctx->size);
    ctx->buffer = wl_shm_pool_create_buffer(
        pool, 0, ctx->width, ctx->height, ctx->stride,
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    
    /* Attach and commit */
    wl_surface_attach(surface, ctx->buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, ctx->width, ctx->height);
    wl_surface_commit(surface);
    
    /* Note: buffer will be released by compositor, fd stays open until then */
    /* We can close fd now as Wayland has duplicated it internally */
    close(ctx->fd);
    ctx->fd = -1;
    
    /* Unmap memory (safe after commit) */
    munmap(ctx->data, ctx->size);
    ctx->data = NULL;
}

/* ============================================================================
 * Drawing Primitives
 * ============================================================================ */

/*
 * Draw a rounded rectangle path.
 * Does not stroke or fill - just creates the path.
 */
static void draw_rounded_rect(cairo_t *cr, double x, double y, 
                               double w, double h, double radius) {
    if (radius <= 0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    
    /* Clamp radius to half of smallest dimension */
    if (radius > w / 2.0) radius = w / 2.0;
    if (radius > h / 2.0) radius = h / 2.0;
    
    double x1 = x + w;
    double y1 = y + h;
    
    cairo_new_sub_path(cr);
    cairo_arc(cr, x1 - radius, y + radius, radius, -G_PI_2, 0);
    cairo_arc(cr, x1 - radius, y1 - radius, radius, 0, G_PI_2);
    cairo_arc(cr, x + radius, y1 - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, G_PI + G_PI_2);
    cairo_close_path(cr);
}

/*
 * Set cairo source color from ConfigColor.
 */
static void set_color(cairo_t *cr, const ConfigColor *color) {
    cairo_set_source_rgba(cr, color->r, color->g, color->b, color->a);
}

/* ============================================================================
 * Main Rendering Functions
 * ============================================================================ */

/*
 * Draw a simple placeholder overlay (used during initialization).
 */
void render_draw(struct wl_surface *surface, int width, int height) {
    if (!surface || width <= 0 || height <= 0) {
        return;
    }
    
    const SwitcherConfig *cfg = config_get();
    
    RenderContext ctx;
    if (!render_context_init(&ctx, width, height)) {
        LOG_ERROR("[RENDER] Failed to create render context");
        return;
    }
    
    cairo_t *cr = ctx.cr;
    
    /* Draw background */
    set_color(cr, &cfg->background);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    
    /* Draw centered text */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, "Hypr Switcher", -1);
    
    PangoFontDescription *font = pango_font_description_from_string(cfg->font);
    pango_font_description_set_size(font, 24 * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    
    int pw, ph;
    pango_layout_get_size(layout, &pw, &ph);
    double text_w = pw / (double)PANGO_SCALE;
    double text_h = ph / (double)PANGO_SCALE;
    
    double text_x = (width - text_w) / 2.0;
    double text_y = (height - text_h) / 2.0;
    
    set_color(cr, &cfg->text_color);
    cairo_move_to(cr, text_x, text_y);
    pango_cairo_show_layout(cr, layout);
    
    pango_font_description_free(font);
    g_object_unref(layout);
    
    render_context_commit(&ctx, surface);
    LOG_DEBUG("[RENDER] Drew placeholder %dx%d", width, height);
}

/*
 * Draw window titles without focus highlight.
 * (Wrapper for backward compatibility)
 */
void render_draw_titles(struct wl_surface *surface, int width, int height,
                        const char **titles, size_t count) {
    render_draw_titles_focus(surface, width, height, titles, count, -1);
}

/*
 * Draw window titles with focus highlight.
 * This is the main rendering function for the switcher overlay.
 */
void render_draw_titles_focus(struct wl_surface *surface, int width, int height,
                               const char **titles, size_t count, int focused_index) {
    if (!surface || width <= 0 || height <= 0) {
        return;
    }
    
    const SwitcherConfig *cfg = config_get();
    
    RenderContext ctx;
    if (!render_context_init(&ctx, width, height)) {
        LOG_ERROR("[RENDER] Failed to create render context");
        return;
    }
    
    cairo_t *cr = ctx.cr;
    
    /* Enable antialiasing */
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    
    /* ====================================================================
     * Draw Background with Rounded Corners
     * ==================================================================== */
    
    double bg_radius = cfg->corner_radius * 1.5;  /* Slightly larger for outer bg */
    
    /* Clear to transparent first */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    
    /* Draw rounded background */
    draw_rounded_rect(cr, 0, 0, width, height, bg_radius);
    set_color(cr, &cfg->background);
    cairo_fill(cr);
    
    /* ====================================================================
     * Setup Text Rendering
     * ==================================================================== */
    
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(cfg->font);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    
    if (cfg->center_text) {
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    } else {
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    }
    
    /* Calculate item dimensions */
    int padding = cfg->padding;
    int item_height = cfg->item_height;
    int item_pad_x = cfg->item_padding_x;
    int radius = cfg->corner_radius;
    
    int content_width = width - (2 * padding);
    if (content_width < 100) content_width = 100;
    
    /* Set max width for text ellipsis */
    int text_max_width = content_width - (2 * item_pad_x);
    if (cfg->show_index) {
        text_max_width -= 40;  /* Reserve space for index */
    }
    pango_layout_set_width(layout, text_max_width * PANGO_SCALE);
    
    /* ====================================================================
     * Handle Empty State
     * ==================================================================== */
    
    if (count == 0 || !titles) {
        const char *msg = "No windows open";
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_width(layout, -1);
        pango_layout_set_text(layout, msg, -1);
        
        int tw, th;
        pango_layout_get_size(layout, &tw, &th);
        double text_w = tw / (double)PANGO_SCALE;
        double text_h = th / (double)PANGO_SCALE;
        
        set_color(cr, &cfg->text_color);
        cairo_move_to(cr, (width - text_w) / 2.0, (height - text_h) / 2.0);
        pango_cairo_show_layout(cr, layout);
        
        pango_font_description_free(font);
        g_object_unref(layout);
        render_context_commit(&ctx, surface);
        LOG_DEBUG("[RENDER] Drew empty state %dx%d", width, height);
        return;
    }
    
    /* ====================================================================
     * Draw Window Items
     * ==================================================================== */
    
    /* Calculate visible items */
    size_t max_visible = cfg->max_visible_items > 0 
        ? (size_t)cfg->max_visible_items 
        : count;
    size_t visible_count = count < max_visible ? count : max_visible;
    
    /* Calculate scroll offset if focused item would be out of view */
    size_t scroll_offset = 0;
    if (focused_index >= 0 && (size_t)focused_index >= visible_count) {
        scroll_offset = (size_t)focused_index - visible_count + 1;
        if (scroll_offset + visible_count > count) {
            scroll_offset = count - visible_count;
        }
    }
    
    /* Draw each visible item */
    for (size_t vi = 0; vi < visible_count; vi++) {
        size_t i = vi + scroll_offset;
        if (i >= count) break;
        
        const char *text = titles[i] ? titles[i] : "(untitled)";
        bool is_focused = ((int)i == focused_index);
        
        /* Calculate item position */
        double item_x = padding;
        double item_y = padding + (vi * item_height);
        double item_w = content_width;
        double item_h = item_height - 4;  /* Small gap between items */
        
        /* ================================================================
         * Draw Item Background and Border
         * ================================================================ */
        
        if (is_focused) {
            /* Focused item: filled background */
            draw_rounded_rect(cr, item_x, item_y, item_w, item_h, radius);
            set_color(cr, &cfg->highlight_bg);
            cairo_fill_preserve(cr);
            
            /* Focused border */
            set_color(cr, &cfg->highlight_border);
            cairo_set_line_width(cr, cfg->border_width_selected);
            cairo_stroke(cr);
        } else {
            /* Normal item: subtle border only */
            draw_rounded_rect(cr, item_x + 0.5, item_y + 0.5, 
                              item_w - 1, item_h - 1, radius);
            set_color(cr, &cfg->border_color);
            cairo_set_line_width(cr, cfg->border_width_normal);
            cairo_stroke(cr);
        }
        
        /* ================================================================
         * Draw Item Text
         * ================================================================ */
        
        /* Build display text */
        char display_text[512];
        if (cfg->show_index) {
            snprintf(display_text, sizeof(display_text), "%zu. %s", i + 1, text);
        } else {
            strncpy(display_text, text, sizeof(display_text) - 1);
            display_text[sizeof(display_text) - 1] = '\0';
        }
        
        pango_layout_set_text(layout, display_text, -1);
        
        /* Get text dimensions */
        int tw, th;
        pango_layout_get_size(layout, &tw, &th);
        double text_h = th / (double)PANGO_SCALE;
        
        /* Calculate text position (vertically centered in item) */
        double text_x = item_x + item_pad_x;
        double text_y = item_y + (item_h - text_h) / 2.0;
        
        if (cfg->center_text) {
            double text_w = tw / (double)PANGO_SCALE;
            text_x = item_x + (item_w - text_w) / 2.0;
        }
        
        /* Set text color */
        if (is_focused) {
            set_color(cr, &cfg->text_selected);
        } else {
            set_color(cr, &cfg->text_color);
        }
        
        /* Draw text */
        cairo_move_to(cr, text_x, text_y);
        pango_cairo_show_layout(cr, layout);
    }
    
    /* ====================================================================
     * Draw Scroll Indicators (if needed)
     * ==================================================================== */
    
    if (scroll_offset > 0) {
        /* Draw "more above" indicator */
        set_color(cr, &cfg->text_color);
        cairo_set_line_width(cr, 2);
        double cx = width / 2.0;
        double cy = padding / 2.0;
        cairo_move_to(cr, cx - 10, cy + 3);
        cairo_line_to(cr, cx, cy - 3);
        cairo_line_to(cr, cx + 10, cy + 3);
        cairo_stroke(cr);
    }
    
    if (scroll_offset + visible_count < count) {
        /* Draw "more below" indicator */
        set_color(cr, &cfg->text_color);
        cairo_set_line_width(cr, 2);
        double cx = width / 2.0;
        double cy = height - padding / 2.0;
        cairo_move_to(cr, cx - 10, cy - 3);
        cairo_line_to(cr, cx, cy + 3);
        cairo_line_to(cr, cx + 10, cy - 3);
        cairo_stroke(cr);
    }
    
    /* ====================================================================
     * Cleanup and Commit
     * ==================================================================== */
    
    pango_font_description_free(font);
    g_object_unref(layout);
    
    render_context_commit(&ctx, surface);
    LOG_DEBUG("[RENDER] Drew %zu items (focus=%d) on %dx%d", 
              visible_count, focused_index, width, height);
}