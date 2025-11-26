#pragma once
/*
 * config.h - Configuration system for hyprswitcher
 *
 * Provides customizable appearance settings including:
 *   - Colors (background, text, selection highlight)
 *   - Font settings
 *   - Sizing (padding, corner radius, item height)
 *   - Behavior options
 *
 * Configuration is loaded from:
 *   $XDG_CONFIG_HOME/hyprswitcher/config
 *   or ~/.config/hyprswitcher/config
 *
 * Format: KEY=VALUE (one per line, # for comments)
 *
 * Example config file:
 *   # hyprswitcher configuration
 *   font=Sans 14
 *   background_color=#1a1a1aE6
 *   text_color=#FFFFFF
 *   highlight_color=#4A9DFFCC
 *   border_color=#666666CC
 *   corner_radius=8
 *   item_height=48
 *   padding=16
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* RGBA color structure */
typedef struct {
    double r;  /* 0.0 - 1.0 */
    double g;  /* 0.0 - 1.0 */
    double b;  /* 0.0 - 1.0 */
    double a;  /* 0.0 - 1.0 */
} ConfigColor;

/* Configuration structure */
typedef struct {
    /* Font settings */
    char font[128];              /* Pango font description (e.g., "Sans 14") */
    
    /* Colors */
    ConfigColor background;      /* Overlay background */
    ConfigColor text_color;      /* Normal text color */
    ConfigColor text_selected;   /* Selected item text color */
    ConfigColor highlight_bg;    /* Selected item background fill */
    ConfigColor highlight_border;/* Selected item border */
    ConfigColor border_color;    /* Normal item border */
    
    /* Sizing */
    int padding;                 /* Outer padding in pixels */
    int item_padding_x;          /* Horizontal padding inside items */
    int item_padding_y;          /* Vertical padding inside items */
    int item_height;             /* Height of each item row */
    int corner_radius;           /* Rounded corner radius */
    int border_width_normal;     /* Border width for normal items */
    int border_width_selected;   /* Border width for selected item */
    int overlay_width;           /* Overlay width (0 = auto) */
    int max_visible_items;       /* Maximum items to show (0 = no limit) */
    
    /* Behavior */
    bool show_index;             /* Show item index numbers */
    bool center_text;            /* Center text in items */
    
    /* Internal */
    bool loaded;                 /* Whether config was loaded from file */
} SwitcherConfig;

/*
 * Get the global configuration instance.
 * This is a singleton - first call initializes with defaults,
 * subsequent calls return the same instance.
 *
 * Returns:
 *   Pointer to the global config (never NULL)
 */
const SwitcherConfig *config_get(void);

/*
 * Get a mutable pointer to the global configuration.
 * Use for initialization or runtime changes.
 *
 * Returns:
 *   Mutable pointer to the global config
 */
SwitcherConfig *config_get_mut(void);

/*
 * Initialize configuration with default values.
 * Called automatically by config_get() if not already initialized.
 */
void config_init_defaults(void);

/*
 * Load configuration from file.
 * Searches for config file in standard locations:
 *   1. $XDG_CONFIG_HOME/hyprswitcher/config
 *   2. ~/.config/hyprswitcher/config
 *
 * Missing file or parse errors use defaults for those values.
 *
 * Returns:
 *   0:  Config loaded successfully (or defaults used)
 *   -1: Error (logged, defaults still applied)
 */
int config_load(void);

/*
 * Parse a color string into a ConfigColor.
 * Supports formats:
 *   #RGB        (e.g., #FFF)
 *   #RGBA       (e.g., #FFFF)
 *   #RRGGBB     (e.g., #FFFFFF)
 *   #RRGGBBAA   (e.g., #FFFFFFCC)
 *
 * @param str    Color string to parse
 * @param color  Output color structure
 *
 * Returns:
 *   true:  Parsed successfully
 *   false: Parse error (color unchanged)
 */
bool config_parse_color(const char *str, ConfigColor *color);

/*
 * Get the config file path (for logging/debugging).
 *
 * @param buf     Buffer to write path into
 * @param bufsize Size of buffer
 *
 * Returns:
 *   0:  Success
 *   -1: Error (buffer too small or env vars not set)
 */
int config_get_path(char *buf, size_t bufsize);

/* ============================================================================
 * Default Values (can be used for reset)
 * ============================================================================ */

/* Default font */
#define CONFIG_DEFAULT_FONT "Sans 14"

/* Default colors (RGBA as hex) */
#define CONFIG_DEFAULT_BG_R          0.10
#define CONFIG_DEFAULT_BG_G          0.10
#define CONFIG_DEFAULT_BG_B          0.12
#define CONFIG_DEFAULT_BG_A          0.92

#define CONFIG_DEFAULT_TEXT_R        0.95
#define CONFIG_DEFAULT_TEXT_G        0.95
#define CONFIG_DEFAULT_TEXT_B        0.95
#define CONFIG_DEFAULT_TEXT_A        1.0

#define CONFIG_DEFAULT_TEXT_SEL_R    1.0
#define CONFIG_DEFAULT_TEXT_SEL_G    1.0
#define CONFIG_DEFAULT_TEXT_SEL_B    1.0
#define CONFIG_DEFAULT_TEXT_SEL_A    1.0

#define CONFIG_DEFAULT_HL_BG_R       0.29
#define CONFIG_DEFAULT_HL_BG_G       0.56
#define CONFIG_DEFAULT_HL_BG_B       0.89
#define CONFIG_DEFAULT_HL_BG_A       0.25

#define CONFIG_DEFAULT_HL_BORDER_R   0.35
#define CONFIG_DEFAULT_HL_BORDER_G   0.62
#define CONFIG_DEFAULT_HL_BORDER_B   0.95
#define CONFIG_DEFAULT_HL_BORDER_A   0.95

#define CONFIG_DEFAULT_BORDER_R      0.40
#define CONFIG_DEFAULT_BORDER_G      0.40
#define CONFIG_DEFAULT_BORDER_B      0.45
#define CONFIG_DEFAULT_BORDER_A      0.60

/* Default sizing */
#define CONFIG_DEFAULT_PADDING              16
#define CONFIG_DEFAULT_ITEM_PADDING_X       12
#define CONFIG_DEFAULT_ITEM_PADDING_Y       8
#define CONFIG_DEFAULT_ITEM_HEIGHT          48
#define CONFIG_DEFAULT_CORNER_RADIUS        8
#define CONFIG_DEFAULT_BORDER_WIDTH_NORMAL  1
#define CONFIG_DEFAULT_BORDER_WIDTH_SELECTED 2
#define CONFIG_DEFAULT_OVERLAY_WIDTH        600
#define CONFIG_DEFAULT_MAX_VISIBLE_ITEMS    12

/* Default behavior */
#define CONFIG_DEFAULT_SHOW_INDEX    false
#define CONFIG_DEFAULT_CENTER_TEXT   false

#endif /* CONFIG_H */