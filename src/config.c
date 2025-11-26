#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "logger/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* Global configuration instance */
static SwitcherConfig g_config;
static bool g_config_initialized = false;

/* ============================================================================
 * Color Parsing
 * ============================================================================ */

/*
 * Parse a single hex digit to int (0-15).
 * Returns -1 on error.
 */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/*
 * Parse two hex digits to int (0-255).
 * Returns -1 on error.
 */
static int hex_byte(const char *s) {
    int hi = hex_digit(s[0]);
    int lo = hex_digit(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

bool config_parse_color(const char *str, ConfigColor *color) {
    if (!str || !color) {
        return false;
    }
    
    /* Skip leading whitespace */
    while (*str && isspace(*str)) str++;
    
    /* Expect # prefix */
    if (*str != '#') {
        return false;
    }
    str++;
    
    /* Determine format by length */
    size_t len = strlen(str);
    
    /* Remove trailing whitespace from length calculation */
    while (len > 0 && isspace(str[len - 1])) len--;
    
    int r, g, b, a = 255;
    
    if (len == 3) {
        /* #RGB -> expand to #RRGGBB */
        int rv = hex_digit(str[0]);
        int gv = hex_digit(str[1]);
        int bv = hex_digit(str[2]);
        if (rv < 0 || gv < 0 || bv < 0) return false;
        r = rv * 17;  /* 0xF -> 0xFF */
        g = gv * 17;
        b = bv * 17;
    } else if (len == 4) {
        /* #RGBA -> expand */
        int rv = hex_digit(str[0]);
        int gv = hex_digit(str[1]);
        int bv = hex_digit(str[2]);
        int av = hex_digit(str[3]);
        if (rv < 0 || gv < 0 || bv < 0 || av < 0) return false;
        r = rv * 17;
        g = gv * 17;
        b = bv * 17;
        a = av * 17;
    } else if (len == 6) {
        /* #RRGGBB */
        r = hex_byte(str);
        g = hex_byte(str + 2);
        b = hex_byte(str + 4);
        if (r < 0 || g < 0 || b < 0) return false;
    } else if (len == 8) {
        /* #RRGGBBAA */
        r = hex_byte(str);
        g = hex_byte(str + 2);
        b = hex_byte(str + 4);
        a = hex_byte(str + 6);
        if (r < 0 || g < 0 || b < 0 || a < 0) return false;
    } else {
        return false;
    }
    
    color->r = r / 255.0;
    color->g = g / 255.0;
    color->b = b / 255.0;
    color->a = a / 255.0;
    
    return true;
}

/* ============================================================================
 * Configuration Initialization
 * ============================================================================ */

void config_init_defaults(void) {
    memset(&g_config, 0, sizeof(g_config));
    
    /* Font */
    strncpy(g_config.font, CONFIG_DEFAULT_FONT, sizeof(g_config.font) - 1);
    
    /* Background */
    g_config.background.r = CONFIG_DEFAULT_BG_R;
    g_config.background.g = CONFIG_DEFAULT_BG_G;
    g_config.background.b = CONFIG_DEFAULT_BG_B;
    g_config.background.a = CONFIG_DEFAULT_BG_A;
    
    /* Text color */
    g_config.text_color.r = CONFIG_DEFAULT_TEXT_R;
    g_config.text_color.g = CONFIG_DEFAULT_TEXT_G;
    g_config.text_color.b = CONFIG_DEFAULT_TEXT_B;
    g_config.text_color.a = CONFIG_DEFAULT_TEXT_A;
    
    /* Selected text color */
    g_config.text_selected.r = CONFIG_DEFAULT_TEXT_SEL_R;
    g_config.text_selected.g = CONFIG_DEFAULT_TEXT_SEL_G;
    g_config.text_selected.b = CONFIG_DEFAULT_TEXT_SEL_B;
    g_config.text_selected.a = CONFIG_DEFAULT_TEXT_SEL_A;
    
    /* Highlight background */
    g_config.highlight_bg.r = CONFIG_DEFAULT_HL_BG_R;
    g_config.highlight_bg.g = CONFIG_DEFAULT_HL_BG_G;
    g_config.highlight_bg.b = CONFIG_DEFAULT_HL_BG_B;
    g_config.highlight_bg.a = CONFIG_DEFAULT_HL_BG_A;
    
    /* Highlight border */
    g_config.highlight_border.r = CONFIG_DEFAULT_HL_BORDER_R;
    g_config.highlight_border.g = CONFIG_DEFAULT_HL_BORDER_G;
    g_config.highlight_border.b = CONFIG_DEFAULT_HL_BORDER_B;
    g_config.highlight_border.a = CONFIG_DEFAULT_HL_BORDER_A;
    
    /* Normal border */
    g_config.border_color.r = CONFIG_DEFAULT_BORDER_R;
    g_config.border_color.g = CONFIG_DEFAULT_BORDER_G;
    g_config.border_color.b = CONFIG_DEFAULT_BORDER_B;
    g_config.border_color.a = CONFIG_DEFAULT_BORDER_A;
    
    /* Sizing */
    g_config.padding = CONFIG_DEFAULT_PADDING;
    g_config.item_padding_x = CONFIG_DEFAULT_ITEM_PADDING_X;
    g_config.item_padding_y = CONFIG_DEFAULT_ITEM_PADDING_Y;
    g_config.item_height = CONFIG_DEFAULT_ITEM_HEIGHT;
    g_config.corner_radius = CONFIG_DEFAULT_CORNER_RADIUS;
    g_config.border_width_normal = CONFIG_DEFAULT_BORDER_WIDTH_NORMAL;
    g_config.border_width_selected = CONFIG_DEFAULT_BORDER_WIDTH_SELECTED;
    g_config.overlay_width = CONFIG_DEFAULT_OVERLAY_WIDTH;
    g_config.max_visible_items = CONFIG_DEFAULT_MAX_VISIBLE_ITEMS;
    
    /* Behavior */
    g_config.show_index = CONFIG_DEFAULT_SHOW_INDEX;
    g_config.center_text = CONFIG_DEFAULT_CENTER_TEXT;
    
    g_config.loaded = false;
    g_config_initialized = true;
    
    LOG_DEBUG("[CONFIG] Initialized with default values");
}

/* ============================================================================
 * Configuration File Path
 * ============================================================================ */

int config_get_path(char *buf, size_t bufsize) {
    if (!buf || bufsize == 0) {
        return -1;
    }
    
    /* Try XDG_CONFIG_HOME first */
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home && config_home[0] != '\0') {
        int ret = snprintf(buf, bufsize, "%s/hyprswitcher/config", config_home);
        if (ret > 0 && (size_t)ret < bufsize) {
            return 0;
        }
    }
    
    /* Fall back to ~/.config */
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int ret = snprintf(buf, bufsize, "%s/.config/hyprswitcher/config", home);
        if (ret > 0 && (size_t)ret < bufsize) {
            return 0;
        }
    }
    
    return -1;
}

/* ============================================================================
 * Configuration File Parsing
 * ============================================================================ */

/*
 * Trim leading and trailing whitespace in place.
 * Returns pointer to trimmed string (may be different from input).
 */
static char *trim(char *str) {
    if (!str) return str;
    
    /* Trim leading */
    while (*str && isspace(*str)) str++;
    
    if (*str == '\0') return str;
    
    /* Trim trailing */
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) {
        *end = '\0';
        end--;
    }
    
    return str;
}

/*
 * Parse a single configuration line.
 * Format: KEY=VALUE (whitespace around = is trimmed)
 */
static void parse_config_line(char *line) {
    /* Skip empty lines and comments */
    line = trim(line);
    if (!line || line[0] == '\0' || line[0] == '#') {
        return;
    }
    
    /* Find = separator */
    char *eq = strchr(line, '=');
    if (!eq) {
        LOG_DEBUG("[CONFIG] Invalid line (no '='): %s", line);
        return;
    }
    
    /* Split into key and value */
    *eq = '\0';
    char *key = trim(line);
    char *value = trim(eq + 1);
    
    if (!key || key[0] == '\0') {
        return;
    }
    
    LOG_DEBUG("[CONFIG] Parsing: %s = %s", key, value ? value : "(null)");
    
    /* Match key and set value */
    if (strcmp(key, "font") == 0) {
        if (value && value[0] != '\0') {
            strncpy(g_config.font, value, sizeof(g_config.font) - 1);
            g_config.font[sizeof(g_config.font) - 1] = '\0';
        }
    }
    else if (strcmp(key, "background_color") == 0 || strcmp(key, "bg_color") == 0) {
        config_parse_color(value, &g_config.background);
    }
    else if (strcmp(key, "text_color") == 0) {
        config_parse_color(value, &g_config.text_color);
    }
    else if (strcmp(key, "text_selected_color") == 0) {
        config_parse_color(value, &g_config.text_selected);
    }
    else if (strcmp(key, "highlight_color") == 0 || strcmp(key, "highlight_bg") == 0) {
        config_parse_color(value, &g_config.highlight_bg);
    }
    else if (strcmp(key, "highlight_border") == 0 || strcmp(key, "highlight_border_color") == 0) {
        config_parse_color(value, &g_config.highlight_border);
    }
    else if (strcmp(key, "border_color") == 0) {
        config_parse_color(value, &g_config.border_color);
    }
    else if (strcmp(key, "padding") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 100) g_config.padding = v;
    }
    else if (strcmp(key, "item_padding_x") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 100) g_config.item_padding_x = v;
    }
    else if (strcmp(key, "item_padding_y") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 100) g_config.item_padding_y = v;
    }
    else if (strcmp(key, "item_height") == 0) {
        int v = atoi(value);
        if (v >= 20 && v <= 200) g_config.item_height = v;
    }
    else if (strcmp(key, "corner_radius") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 50) g_config.corner_radius = v;
    }
    else if (strcmp(key, "border_width") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 10) g_config.border_width_normal = v;
    }
    else if (strcmp(key, "border_width_selected") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 10) g_config.border_width_selected = v;
    }
    else if (strcmp(key, "overlay_width") == 0 || strcmp(key, "width") == 0) {
        int v = atoi(value);
        if (v >= 200 && v <= 2000) g_config.overlay_width = v;
    }
    else if (strcmp(key, "max_visible_items") == 0 || strcmp(key, "max_items") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 50) g_config.max_visible_items = v;
    }
    else if (strcmp(key, "show_index") == 0) {
        g_config.show_index = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    }
    else if (strcmp(key, "center_text") == 0) {
        g_config.center_text = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    }
    else {
        LOG_DEBUG("[CONFIG] Unknown key: %s", key);
    }
}

int config_load(void) {
    /* Ensure defaults are set first */
    if (!g_config_initialized) {
        config_init_defaults();
    }
    
    /* Get config file path */
    char path[512];
    if (config_get_path(path, sizeof(path)) != 0) {
        LOG_DEBUG("[CONFIG] Could not determine config file path");
        return 0;  /* Not an error - use defaults */
    }
    
    /* Try to open config file */
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            LOG_DEBUG("[CONFIG] Config file not found: %s (using defaults)", path);
        } else {
            LOG_DEBUG("[CONFIG] Could not open config file: %s (%s)", path, strerror(errno));
        }
        return 0;  /* Not an error - use defaults */
    }
    
    LOG_INFO("[CONFIG] Loading config from: %s", path);
    
    /* Parse line by line */
    char line[512];
    int line_num = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        
        /* Remove newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        parse_config_line(line);
    }
    
    fclose(f);
    
    g_config.loaded = true;
    LOG_INFO("[CONFIG] Loaded %d lines from config file", line_num);
    
    return 0;
}

/* ============================================================================
 * Configuration Access
 * ============================================================================ */

const SwitcherConfig *config_get(void) {
    if (!g_config_initialized) {
        config_init_defaults();
    }
    return &g_config;
}

SwitcherConfig *config_get_mut(void) {
    if (!g_config_initialized) {
        config_init_defaults();
    }
    return &g_config;
}