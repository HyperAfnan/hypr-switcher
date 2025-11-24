#include "input.h"
#include "logger/logger.h"
#include <wayland-client.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

/*
 * Minimal keyboard input handling:
 *  - Detect Escape key (evdev code 1) -> signal close.
 *  - Detect Alt+Tab chord (Alt held when Tab (evdev 15) pressed).
 *
 * We rely on raw evdev codes delivered by wl_keyboard::key.
 * NOTE: Wayland sends the raw evdev keycode (no +8 offset needed unless using xkb).
 *
 * Alt keys considered: Left Alt (56) and Right Alt (100).
 */

static struct wl_keyboard *g_keyboard = NULL;
static bool g_alt_down = false;
static bool g_esc_flag = false;
static bool g_alt_tab_flag = false;
static uint32_t g_last_alt_press_time = 0; /* timestamp of last Alt press (wayland time msec) */
static uint32_t g_last_key_time = 0;       /* timestamp of last key event */
static uint32_t g_last_leave_time = 0;     /* synthetic counter for leave events */
static bool g_has_focus = false;           /* tracks whether keyboard focus is on our surface */
static bool g_focus_lost_flag = false;     /* set when a leave occurs; consumed by input_focus_lost() */

/* xkbcommon state */
static struct xkb_context *g_xkb_ctx = NULL;
static struct xkb_keymap  *g_xkb_keymap = NULL;
static struct xkb_state   *g_xkb_state = NULL;

/* Helper: update modifier tracking from xkb state */
static void update_mods_from_state() {
    if (!g_xkb_state) return;
    /* Alt can be named "Mod1" typically; also some layouts use "Alt".
       We check both. */
    bool mod1 = xkb_state_mod_name_is_active(
        g_xkb_state, "Mod1", XKB_STATE_MODS_EFFECTIVE) > 0;
    bool alt  = xkb_state_mod_name_is_active(
        g_xkb_state, "Alt", XKB_STATE_MODS_EFFECTIVE) > 0;
    g_alt_down = (mod1 || alt);
}

static void keyboard_keymap(void *data,
                            struct wl_keyboard *keyboard,
                            uint32_t format,
                            int fd,
                            uint32_t size)
{
    (void)data;
    (void)keyboard;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }

    if (!g_xkb_ctx) {
        g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!g_xkb_ctx) {
            munmap(map, size);
            close(fd);
            return;
        }
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        g_xkb_ctx,
        (const char *)map,
        XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(map, size);
    close(fd);

    if (!keymap) {
        return;
    }

    if (g_xkb_state) {
        xkb_state_unref(g_xkb_state);
        g_xkb_state = NULL;
    }
    if (g_xkb_keymap) {
        xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = NULL;
    }

    g_xkb_keymap = keymap;
    g_xkb_state = xkb_state_new(g_xkb_keymap);
    if (!g_xkb_state) {
        xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = NULL;
        return;
    }

    update_mods_from_state();
    LOG_INFO("[INPUT] xkb keymap loaded.");
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
    /* Focus gained: reset alt tracking, mark focus */
    bool prev_focus = g_has_focus;
    g_alt_down = false;
    g_has_focus = true;
    LOG_DEBUG("[INPUT] Keyboard enter (serial=%u prev_focus=%d new_focus=1 alt_down=%d esc=%d altTab=%d)", serial, prev_focus, g_alt_down, g_esc_flag, g_alt_tab_flag);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    g_last_leave_time++;
    bool alt_was_down = g_alt_down;
    uint32_t deltaAlt = (g_last_key_time > g_last_alt_press_time)
                        ? (g_last_key_time - g_last_alt_press_time)
                        : 999999;
    bool inferredAltTab = (g_last_alt_press_time != 0 && deltaAlt < 500);
    if (inferredAltTab) {
        g_alt_tab_flag = true;
        LOG_INFO("[INPUT] Keyboard leave (serial=%u) focus_lost alt_was_down=%d deltaAlt=%u(ms) -> inferred Alt-driven focus switch.", serial, alt_was_down, deltaAlt);
    } else {
        LOG_DEBUG("[INPUT] Keyboard leave (serial=%u) focus_lost alt_was_down=%d deltaAlt=%u(ms) esc=%d altTab_before=%d",
                  serial, alt_was_down, deltaAlt, g_esc_flag, g_alt_tab_flag);
    }
    g_focus_lost_flag = true;
    g_alt_down = false;
    g_has_focus = false;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    uint32_t xkb_code = key + 8;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    if (g_xkb_state) {
        sym = xkb_state_key_get_one_sym(g_xkb_state, xkb_code);
    }
    if (pressed) {
        bool is_escape = false;
        bool is_tab = false;
        if (sym != XKB_KEY_NoSymbol) {
            is_escape = (sym == XKB_KEY_Escape);
            is_tab    = (sym == XKB_KEY_Tab);
        } else {
            is_escape = (key == 1);
            is_tab    = (key == 15);
        }
        if (is_escape) {
            g_esc_flag = true;
            LOG_DEBUG("[INPUT] ESC pressed (sym=%u focus=%d alt_down=%d)", sym, g_has_focus, g_alt_down);
        } else if (is_tab) {
            update_mods_from_state();
            LOG_DEBUG("[INPUT] Tab pressed (sym=%u focus=%d alt_down=%d)", sym, g_has_focus, g_alt_down);
            if (g_alt_down) {
                g_alt_tab_flag = true;
                LOG_DEBUG("[INPUT] Alt+Tab chord detected (sym=%u focus=%d)", sym, g_has_focus);
            }
        } else {
            if (sym != XKB_KEY_NoSymbol) {
                char name[64] = {0};
                xkb_keysym_get_name(sym, name, sizeof(name));
                LOG_DEBUG("[INPUT] Key pressed: %s (sym=%u focus=%d alt_down=%d)", name, sym, g_has_focus, g_alt_down);
            } else {
                LOG_DEBUG("[INPUT] Key pressed (raw keycode=%u focus=%d alt_down=%d)", key, g_has_focus, g_alt_down);
            }
        }
        g_last_key_time = time;
        /* Alt press detection */
        if (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R) {
            g_alt_down = true;
            g_last_alt_press_time = time;
            LOG_DEBUG("[INPUT] Alt pressed (sym=%u time=%u focus=%d)", sym, time, g_has_focus);
        } else if (key == 56 || key == 100) {
            g_alt_down = true;
            g_last_alt_press_time = time;
            LOG_DEBUG("[INPUT] Alt pressed (fallback code=%u time=%u focus=%d)", key, time, g_has_focus);
        }
    } else {
        bool is_alt_release = (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || key == 56 || key == 100);
        if (is_alt_release) {
            g_alt_down = false;
            LOG_DEBUG("[INPUT] Alt released (sym=%u focus=%d)", sym, g_has_focus);
        }
    }
    update_mods_from_state();
}

static void keyboard_modifiers(void *data,
                               struct wl_keyboard *keyboard,
                               uint32_t serial,
                               uint32_t depressed,
                               uint32_t latched,
                               uint32_t locked,
                               uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    if (g_xkb_state) {
        xkb_state_update_mask(
            g_xkb_state,
            depressed, latched, locked,
            0, 0, group);
        update_mods_from_state();
    }
}

static void keyboard_repeat_info(void *data,
                                 struct wl_keyboard *keyboard,
                                 int32_t rate,
                                 int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
    /* Ignored; Alt+Tab and Escape handling do not depend on key repeat. */
}

static const struct wl_keyboard_listener g_keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info
};

void input_handle_seat(struct wl_seat *seat) {
    if (!seat) return;

    /* If re-binding, destroy previous keyboard */
    if (g_keyboard) {
        wl_keyboard_destroy(g_keyboard);
        g_keyboard = NULL;
    }

    g_keyboard = wl_seat_get_keyboard(seat);
    if (!g_keyboard) {
        LOG_WARN("[INPUT] Failed to get wl_keyboard from seat");
        return;
    }
    wl_keyboard_add_listener(g_keyboard, &g_keyboard_listener, NULL);
    LOG_INFO("[INPUT] Keyboard listener attached.");
}

void input_enable_layer_keyboard(struct zwlr_layer_surface_v1 *layer_surface) {
    if (!layer_surface) return;
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);
    LOG_INFO("[INPUT] Layer surface keyboard interactivity enabled (requesting focus).");
}
bool input_has_focus(void) {
    return g_has_focus;
}
bool input_focus_lost(void) {
    if (g_focus_lost_flag) {
        g_focus_lost_flag = false;
        LOG_DEBUG("[INPUT] focus lost flag consumed");
        return true;
    }
    return false;
}

bool input_escape_pressed(void) {
    if (g_esc_flag) {
        g_esc_flag = false;
        LOG_DEBUG("[INPUT] escape flag consumed");
        return true;
    }
    return false;
}

bool input_alt_tab_triggered(void) {
    if (g_alt_tab_flag) {
        g_alt_tab_flag = false;
        LOG_DEBUG("[INPUT] alt+tab flag consumed");
        return true;
    }
    return false;
}

void input_clear_flags(void) {
    g_esc_flag = false;
    g_alt_tab_flag = false;
    g_focus_lost_flag = false;
}

void input_shutdown(void) {
    if (g_keyboard) {
        wl_keyboard_destroy(g_keyboard);
        g_keyboard = NULL;
    }
    if (g_xkb_state) {
        xkb_state_unref(g_xkb_state);
        g_xkb_state = NULL;
    }
    if (g_xkb_keymap) {
        xkb_keymap_unref(g_xkb_keymap);
        g_xkb_keymap = NULL;
    }
    if (g_xkb_ctx) {
        xkb_context_unref(g_xkb_ctx);
        g_xkb_ctx = NULL;
    }
    g_alt_down = false;
    input_clear_flags();
    LOG_INFO("[INPUT] Input subsystem shut down (xkb cleaned).");
}
