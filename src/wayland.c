#define _XOPEN_SOURCE 700  /* Ensure POSIX prototype for usleep is visible */

#include "wayland.h"
#include "logger/logger.h"
#include "render.h"
#include "util.h"
#include "ipc.h"
#include "input.h"
#include "switcher_ipc.h"
#include "hypr_events.h"
#include "config.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <errno.h>

/* ============================================================================
 * Static State
 * ============================================================================ */

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_seat *seat;

static uint32_t current_width  = 600;
static uint32_t current_height = 120;

static int configured = 0;

/* Client list state */
static HyprClientInfo *g_clients = NULL;
static size_t g_client_count = 0;

/* Deep-copied titles for safe rendering (won't dangle if clients refreshed) */
static char **g_titles = NULL;
static size_t g_titles_count = 0;

/* Selection state */
static int g_selection_index = -1;
static int g_initial_focus_index = -1;  /* For Escape restore */
static char *g_initial_focus_address = NULL;  /* Address of initially focused window */
static char *g_selected_address = NULL;  /* Address of currently selected window (for preservation) */

/* IPC socket FD passed from main, stored for event loop */
static int g_ipc_listen_fd = -1;

/* Hyprland event socket FD */
static int g_hypr_events_fd = -1;

/* Flag to track if we need a redraw */
static bool g_needs_redraw = false;

/* Flag to track if client list changed and needs refresh */
static bool g_clients_dirty = false;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

void wayland_shutdown(void);
static void refresh_client_list(void);
static void rebuild_titles(void);
static void redraw_overlay(void);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Calculate overlay height with overflow protection.
 * Returns a safe height value clamped to reasonable bounds.
 */
static uint32_t calculate_overlay_height(size_t visible_count, int item_height, int padding) {
    #define MAX_OVERLAY_HEIGHT 4096
    size_t raw_height = (visible_count * (size_t)item_height) + (2 * (size_t)padding);
    if (raw_height > MAX_OVERLAY_HEIGHT) {
        raw_height = MAX_OVERLAY_HEIGHT;
    }
    uint32_t desired_height = (uint32_t)raw_height;
    uint32_t min_height = (uint32_t)item_height + (2 * (uint32_t)padding);
    if (desired_height < min_height) {
        desired_height = min_height;
    }
    return desired_height;
}

/* ============================================================================
 * Title Management (Deep Copy for Safety)
 * ============================================================================ */

static void free_titles(void) {
    if (g_titles) {
        for (size_t i = 0; i < g_titles_count; i++) {
            free(g_titles[i]);
        }
        free(g_titles);
        g_titles = NULL;
    }
    g_titles_count = 0;
}

static void rebuild_titles(void) {
    free_titles();
    
    if (g_client_count == 0 || !g_clients) {
        return;
    }
    
    g_titles = calloc(g_client_count, sizeof(char *));
    if (!g_titles) {
        LOG_ERROR("[WAYLAND] Failed to allocate titles array");
        return;
    }
    g_titles_count = g_client_count;
    
    for (size_t i = 0; i < g_client_count; i++) {
        const char *display_name = NULL;
        
        /* Prefer app_class, fall back to title */
        if (g_clients[i].app_class && g_clients[i].app_class[0] != '\0') {
            display_name = g_clients[i].app_class;
        } else if (g_clients[i].title && g_clients[i].title[0] != '\0') {
            display_name = g_clients[i].title;
        } else {
            display_name = "(untitled)";
        }
        
        g_titles[i] = strdup(display_name);
        if (!g_titles[i]) {
            g_titles[i] = "(error)";
        }
    }
}

/* ============================================================================
 * Centralized Selection Management (Phase 2: Bounds & Safety)
 * ============================================================================ */

/*
 * Set selection index with bounds checking and wrap-around.
 * This is the single point of truth for selection changes.
 *
 * @param new_index  Desired new index (-1 for none, or any integer)
 * @param wrap       If true, wrap around at boundaries; if false, clamp
 */
static void selection_set(int new_index, bool wrap) {
    int old_index = g_selection_index;
    
    if (g_client_count == 0) {
        g_selection_index = -1;
        if (old_index != g_selection_index) {
            g_needs_redraw = true;
        }
        return;
    }
    
    if (new_index < 0) {
        if (wrap) {
            new_index = (int)g_client_count - 1;
        } else {
            new_index = 0;
        }
    } else if (new_index >= (int)g_client_count) {
        if (wrap) {
            new_index = 0;
        } else {
            new_index = (int)g_client_count - 1;
        }
    }
    
    g_selection_index = new_index;
    
    /* Update selected address for preservation across refreshes */
    free(g_selected_address);
    g_selected_address = NULL;
    if (g_selection_index >= 0 && g_selection_index < (int)g_client_count) {
        if (g_clients[g_selection_index].address) {
            g_selected_address = strdup(g_clients[g_selection_index].address);
        }
    }
    
    if (old_index != g_selection_index) {
        LOG_DEBUG("[SELECTION] Changed from %d to %d (count=%zu)", 
                  old_index, g_selection_index, g_client_count);
        g_needs_redraw = true;
    }
}

/*
 * Find a client by address in the current list.
 * Returns index if found, -1 if not found.
 */
static int find_client_by_address(const char *address) {
    if (!address || !g_clients) {
        return -1;
    }
    
    for (size_t i = 0; i < g_client_count; i++) {
        if (g_clients[i].address && strcmp(g_clients[i].address, address) == 0) {
            return (int)i;
        }
    }
    
    return -1;
}

/*
 * Preserve selection after client list refresh.
 * Tries to find the previously selected window by address.
 * If not found, adjusts selection to remain valid.
 */
static void preserve_selection(void) {
    if (g_client_count == 0) {
        selection_set(-1, false);
        return;
    }
    
    /* Try to find previously selected window */
    if (g_selected_address) {
        int found = find_client_by_address(g_selected_address);
        if (found >= 0) {
            selection_set(found, false);
            LOG_DEBUG("[SELECTION] Preserved selection at index %d (address=%s)", 
                      found, g_selected_address);
            return;
        }
        LOG_DEBUG("[SELECTION] Previously selected window not found (address=%s)", 
                  g_selected_address);
    }
    
    /* Selected window is gone; clamp to valid range */
    selection_set(g_selection_index, false);
}

/* Helper: cycle selection forward */
static void cycle_forward(void) {
    if (g_client_count > 0) {
        selection_set(g_selection_index + 1, true);
        LOG_DEBUG("[WAYLAND] Cycle forward: new selection index: %d", g_selection_index);
    }
}

/* Helper: cycle selection backward */
static void cycle_backward(void) {
    if (g_client_count > 0) {
        selection_set(g_selection_index - 1, true);
        LOG_DEBUG("[WAYLAND] Cycle backward: new selection index: %d", g_selection_index);
    }
}

/* ============================================================================
 * Client List Management (Phase 2: Dynamic Updates)
 * ============================================================================ */

static void free_client_list(void) {
    if (g_clients) {
        hypr_ipc_free_client_infos(g_clients, g_client_count);
        g_clients = NULL;
    }
    g_client_count = 0;
}

/*
 * Refresh client list from Hyprland IPC.
 * Preserves selection if possible.
 */
static void refresh_client_list(void) {
    LOG_DEBUG("[WAYLAND] Refreshing client list...");
    
    /* Store old selection address for preservation */
    char *old_selected = g_selected_address ? strdup(g_selected_address) : NULL;
    
    /* Free old list */
    free_client_list();
    
    /* Fetch new list */
    if (hypr_ipc_get_clients_basic(&g_clients, &g_client_count) != 0) {
        LOG_WARN("[WAYLAND] Failed to refresh client list");
        g_clients = NULL;
        g_client_count = 0;
    } else {
        /* Sort by focus history: most recently focused first */
        hypr_ipc_sort_clients_by_focus(g_clients, g_client_count);
    }
    
    LOG_DEBUG("[WAYLAND] Client list refreshed: %zu clients (sorted by focus history)", g_client_count);
    
    /* Rebuild titles with deep copies */
    rebuild_titles();
    
    /* Restore selection address and preserve */
    if (old_selected) {
        free(g_selected_address);
        g_selected_address = old_selected;
    }
    preserve_selection();
    
    /* Update layer surface size based on config */
    if (layer_surface && g_client_count > 0) {
        const SwitcherConfig *cfg = config_get();
        int item_height = cfg->item_height;
        int padding = cfg->padding;
        size_t visible_count = g_client_count;
        
        /* Limit visible items if configured */
        if (cfg->max_visible_items > 0 && visible_count > (size_t)cfg->max_visible_items) {
            visible_count = (size_t)cfg->max_visible_items;
        }
        
        uint32_t desired_height = calculate_overlay_height(visible_count, item_height, padding);
        
        if (desired_height != current_height) {
            current_height = desired_height;
            zwlr_layer_surface_v1_set_size(layer_surface, current_width, current_height);
            wl_surface_commit(surface);
            LOG_DEBUG("[WAYLAND] Resized overlay to %ux%u", current_width, current_height);
        }
    }
    
    g_needs_redraw = true;
    g_clients_dirty = false;
}

/* ============================================================================
 * Hyprland Event Handling (Phase 2: Dynamic Window Updates)
 * ============================================================================ */

static void process_hypr_events(void) {
    if (g_hypr_events_fd < 0) {
        return;
    }
    
    HyprEvent event;
    bool list_changed = false;
    
    /* Process all pending events */
    while (hypr_events_read(g_hypr_events_fd, &event)) {
        switch (event.type) {
            case HYPR_EVENT_OPEN_WINDOW:
                LOG_INFO("[HYPR_EVENT] Window opened: %s (%s)", 
                         event.address, event.window_class);
                list_changed = true;
                break;
                
            case HYPR_EVENT_CLOSE_WINDOW:
                LOG_INFO("[HYPR_EVENT] Window closed: %s", event.address);
                list_changed = true;
                
                /* Check if closed window was our initial focus */
                if (g_initial_focus_address && 
                    strcmp(g_initial_focus_address, event.address) == 0) {
                    LOG_DEBUG("[HYPR_EVENT] Initial focus window was closed");
                    free(g_initial_focus_address);
                    g_initial_focus_address = NULL;
                    g_initial_focus_index = -1;
                }
                break;
                
            case HYPR_EVENT_ACTIVE_WINDOW:
                /* Active window changed - we might want to track this */
                LOG_DEBUG("[HYPR_EVENT] Active window: %s (%s)", 
                          event.window_class, event.title);
                break;
                
            case HYPR_EVENT_MOVE_WINDOW:
                LOG_DEBUG("[HYPR_EVENT] Window moved: %s to workspace %d", 
                          event.address, event.workspace_id);
                /* Could refresh if we filter by workspace */
                break;
                
            default:
                break;
        }
    }
    
    if (list_changed) {
        g_clients_dirty = true;
    }
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

static void redraw_overlay(void) {
    if (!surface) {
        return;
    }
    
    if (g_titles && g_titles_count > 0) {
        render_draw_titles_focus(surface, current_width, current_height,
                                 (const char **)g_titles, g_titles_count, 
                                 g_selection_index);
    } else {
        /* Show "No windows" placeholder */
        render_draw_titles_focus(surface, current_width, current_height,
                                 NULL, 0, -1);
    }
    
    g_needs_redraw = false;
}

/* ============================================================================
 * Wayland Registry Handlers
 * ============================================================================ */

static void registry_add(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    (void)version;

    if (strcmp(interface, "wl_compositor") == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "wl_shm") == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        layer_shell = wl_registry_bind(registry, name,
            &zwlr_layer_shell_v1_interface, 3);
    }
    else if (strcmp(interface, "wl_seat") == 0) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
        input_handle_seat(seat);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_add,
    .global_remove = registry_remove,
};

/* ============================================================================
 * Layer Surface Handlers
 * ============================================================================ */

static void layer_surface_configure(void *data,
    struct zwlr_layer_surface_v1 *lsurf,
    uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;
    
    const SwitcherConfig *cfg = config_get();

    /* Compositor may send 0; fall back to config size */
    if (width == 0)  width  = (uint32_t)cfg->overlay_width;
    if (height == 0) height = (uint32_t)cfg->item_height;

    /* Save current compositor-approved size */
    current_width = width;
    current_height = height;

    zwlr_layer_surface_v1_ack_configure(lsurf, serial);
    configured = 1;

    /* Initial client list fetch */
    free_client_list();
    free_titles();
    
    g_selection_index = -1;
    g_initial_focus_index = -1;
    free(g_initial_focus_address);
    g_initial_focus_address = NULL;

    if (hypr_ipc_get_clients_basic(&g_clients, &g_client_count) == 0) {

        /* Sort by focus history: most recently focused first */
        hypr_ipc_sort_clients_by_focus(g_clients, g_client_count);

        /* After sorting:
         *   Index 0 = currently focused window (focusHistoryID == 0)
         *   Index 1 = previously focused window (focusHistoryID == 1)
         * 
         * Initial focus is always index 0 (for Escape restore).
         * Selection starts at index 1 (previous window) so one Tab press
         * switches to the last used window. */
        
        g_initial_focus_index = 0;
        if (g_client_count > 0 && g_clients[0].address) {
            g_initial_focus_address = strdup(g_clients[0].address);
            if (!g_initial_focus_address) {
                LOG_WARN("[WAYLAND] Failed to allocate initial focus address");
            }
        }
        
        /* Start selection at index 1 (previous window) if available */
        if (g_client_count > 1) {
            g_selection_index = 1;
        } else if (g_client_count == 1) {
            g_selection_index = 0;
        }

        /* Calculate dynamic height based on config */
        int item_height = cfg->item_height;
        int padding = cfg->padding;
        size_t visible_count = g_client_count;
        
        /* Limit visible items if configured */
        if (cfg->max_visible_items > 0 && visible_count > (size_t)cfg->max_visible_items) {
            visible_count = (size_t)cfg->max_visible_items;
        }
        
        uint32_t desired_height = calculate_overlay_height(visible_count, item_height, padding);

        current_height = desired_height;
        zwlr_layer_surface_v1_set_size(lsurf, current_width, current_height);
        
        LOG_DEBUG("Initial configure: %ux%u (clients: %zu, focus=%d, initial=%d)",
                  current_width, current_height, g_client_count, 
                  g_selection_index, g_initial_focus_index);

        /* Build titles and update selected address */
        rebuild_titles();
        
        free(g_selected_address);
        g_selected_address = NULL;
        if (g_selection_index >= 0 && g_clients[g_selection_index].address) {
            g_selected_address = strdup(g_clients[g_selection_index].address);
            if (!g_selected_address) {
                LOG_WARN("[WAYLAND] Failed to allocate selected address");
            }
        }

        redraw_overlay();
    } else {
        LOG_WARN("[WAYLAND] Failed to get initial client list");
        render_draw(surface, current_width, current_height);
    }
}

static void layer_surface_closed(void *data,
    struct zwlr_layer_surface_v1 *lsurf)
{
    (void)data; (void)lsurf;
    LOG_INFO("[WAYLAND] Layer surface closed by compositor");
    wayland_shutdown();
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ============================================================================
 * Public API: Initialization
 * ============================================================================ */

struct wl_shm *get_shm() { return shm; }

struct wl_display *init_wayland() {
    display = wl_display_connect(NULL);
    if (!display) {
        DIE("Failed to connect to Wayland.\n");
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !layer_shell || !shm) {
        DIE("Missing Wayland globals.\n");
    }

    return display;
}

void create_layer_surface() {
    surface = wl_compositor_create_surface(compositor);

    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "hyprswitcher");

    zwlr_layer_surface_v1_set_size(layer_surface, current_width, current_height);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    /* Enable keyboard interactivity so we receive key events */
    input_enable_layer_keyboard(layer_surface);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);

    wl_surface_commit(surface);
    
    /* Connect to Hyprland event socket for dynamic updates */
    g_hypr_events_fd = hypr_events_connect();
    if (g_hypr_events_fd < 0) {
        LOG_WARN("[WAYLAND] Could not connect to Hyprland events; dynamic updates disabled");
    }
}

/* ============================================================================
 * Focus Helpers
 * ============================================================================ */

/* Helper: attempt focusing the currently selected client */
static void wayland_focus_selected(const char *tag) {
    if (g_clients && g_client_count > 0 &&
        g_selection_index >= 0 &&
        g_selection_index < (int)g_client_count) {
        HyprClientInfo *sel = &g_clients[g_selection_index];
        LOG_INFO("[FOCUS] %s Selected index=%d address=%s class=%s title=%s",
                 tag ? tag : "",
                 g_selection_index,
                 sel->address ? sel->address : "(null)",
                 sel->app_class ? sel->app_class : "(null)",
                 sel->title ? sel->title : "(null)");
        int frc = hypr_ipc_focus_client(sel);
        if (frc == 0) {
            LOG_INFO("[FOCUS] %s Focus attempt succeeded.", tag ? tag : "");
        } else {
            LOG_WARN("[FOCUS] %s Focus attempt failed.", tag ? tag : "");
        }
    } else {
        LOG_WARN("[FOCUS] %s No valid selection (index=%d count=%zu).",
                 tag ? tag : "",
                 g_selection_index, g_client_count);
    }
}

/* Helper: restore initial focus (for Escape/Cancel) */
static void wayland_restore_initial_focus(void) {
    /* First try to find by stored address (more reliable) */
    if (g_initial_focus_address && g_clients) {
        int found = find_client_by_address(g_initial_focus_address);
        if (found >= 0) {
            HyprClientInfo *initial = &g_clients[found];
            LOG_INFO("[FOCUS] Restoring initial focus by address: %s class=%s title=%s",
                     initial->address ? initial->address : "(null)",
                     initial->app_class ? initial->app_class : "(null)",
                     initial->title ? initial->title : "(null)");
            int frc = hypr_ipc_focus_client(initial);
            if (frc == 0) {
                LOG_INFO("[FOCUS] Initial focus restored successfully.");
            } else {
                LOG_WARN("[FOCUS] Failed to restore initial focus.");
            }
            return;
        }
        LOG_DEBUG("[FOCUS] Initial focus window no longer exists (address=%s)",
                  g_initial_focus_address);
    }
    
    /* Fall back to index-based restore */
    if (g_clients && g_client_count > 0 &&
        g_initial_focus_index >= 0 &&
        g_initial_focus_index < (int)g_client_count) {
        HyprClientInfo *initial = &g_clients[g_initial_focus_index];
        LOG_INFO("[FOCUS] Restoring initial focus by index: %d address=%s",
                 g_initial_focus_index,
                 initial->address ? initial->address : "(null)");
        int frc = hypr_ipc_focus_client(initial);
        if (frc == 0) {
            LOG_INFO("[FOCUS] Initial focus restored successfully.");
        } else {
            LOG_WARN("[FOCUS] Failed to restore initial focus.");
        }
    } else {
        LOG_DEBUG("[FOCUS] No initial focus to restore (index=%d count=%zu address=%s).",
                  g_initial_focus_index, g_client_count,
                  g_initial_focus_address ? g_initial_focus_address : "(null)");
    }
}

/* ============================================================================
 * IPC Command Processing
 * ============================================================================ */

/* Process incoming IPC commands from helper instances */
static void process_ipc_commands(int listen_fd) {
    if (listen_fd < 0) return;

    /* Accept any pending connections */
    int client_fd;
    while ((client_fd = switcher_ipc_accept(listen_fd)) >= 0) {
        /* Read command from this client */
        SwitcherCmdType cmd = switcher_ipc_read_command(client_fd);
        close(client_fd);

        switch (cmd) {
            case SWITCHER_CMD_TYPE_CYCLE:
                LOG_INFO("[IPC] Received CYCLE command");
                cycle_forward();
                break;

            case SWITCHER_CMD_TYPE_CYCLE_BACKWARD:
                LOG_INFO("[IPC] Received CYCLE_BACKWARD command");
                cycle_backward();
                break;

            case SWITCHER_CMD_TYPE_COMMIT:
                LOG_INFO("[IPC] Received COMMIT command");
                wayland_focus_selected("(IPC COMMIT)");
                wayland_shutdown();
                return;

            case SWITCHER_CMD_TYPE_CANCEL:
                LOG_INFO("[IPC] Received CANCEL command");
                wayland_restore_initial_focus();
                wayland_shutdown();
                return;

            case SWITCHER_CMD_TYPE_NONE:
                /* No data yet or client disconnected - not an error */
                break;
                
            case SWITCHER_CMD_TYPE_UNKNOWN:
                LOG_WARN("[IPC] Received unknown command, ignoring");
                break;
                
            default:
                break;
        }
    }
}

/* ============================================================================
 * Main Event Loop
 * ============================================================================ */

/* Original wayland_loop for backward compatibility */
void wayland_loop() {
    wayland_loop_with_ipc(-1);
}

/* Main event loop with IPC socket integration */
void wayland_loop_with_ipc(int ipc_listen_fd) {
    if (!display) return;

    g_ipc_listen_fd = ipc_listen_fd;

    int wl_fd = wl_display_get_fd(display);

    /* Set up poll for Wayland, IPC, and Hyprland events */
    struct pollfd pfds[3];
    int nfds = 1;

    pfds[0].fd = wl_fd;
    pfds[0].events = POLLIN;

    if (ipc_listen_fd >= 0) {
        pfds[nfds].fd = ipc_listen_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
    }
    
    int hypr_events_poll_idx = -1;
    if (g_hypr_events_fd >= 0) {
        hypr_events_poll_idx = nfds;
        pfds[nfds].fd = g_hypr_events_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
    }

    while (display) {
        /* Process any already queued (non-blocking) Wayland events */
        wl_display_dispatch_pending(display);
        
        /* Process Hyprland window events */
        if (g_hypr_events_fd >= 0) {
            process_hypr_events();
        }
        
        /* Refresh client list if dirty */
        if (g_clients_dirty) {
            refresh_client_list();
        }

        /* Process any pending IPC commands */
        if (ipc_listen_fd >= 0) {
            process_ipc_commands(ipc_listen_fd);
            if (!display) break;  /* process_ipc_commands may have called shutdown */
        }

        /* Input / lifecycle checks */
        if (input_focus_lost()) {
            LOG_INFO("[INPUT] Focus lost; attempting focus then closing overlay.");
            wayland_focus_selected("(focus-lost)");
            wayland_shutdown();
            break;
        }
        if (input_escape_pressed()) {
            LOG_INFO("[INPUT] Escape pressed, restoring initial focus and shutting down.");
            wayland_restore_initial_focus();
            wayland_shutdown();
            break;
        }
        if (input_alt_tab_triggered()) {
            if (g_client_count > 0) {
                if (input_shift_is_down()) {
                    cycle_backward();
                } else {
                    cycle_forward();
                }
            }
        }
        if (input_alt_released()) {
            LOG_INFO("[INPUT] Alt released; attempting to focus selected client.");
            wayland_focus_selected("");
            wayland_shutdown();
            break;
        }
        
        /* Redraw if needed */
        if (g_needs_redraw) {
            redraw_overlay();
        }

        /* Prepare to block for new events with timeout */
        if (wl_display_prepare_read(display) != 0) {
            /* Events queued, dispatch them next iteration */
            continue;
        }

        /* Check for Wayland errors */
        if (wl_display_flush(display) < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                LOG_ERROR("[WAYLAND] Connection to compositor lost");
                wl_display_cancel_read(display);
                wayland_shutdown();
                break;
            }
        }

        int timeout_ms = 50; /* wake 20 times per second to remain responsive */
        int pr = poll(pfds, nfds, timeout_ms);

        if (pr < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(display);
                continue;
            }
            LOG_ERROR("[WAYLAND] poll() failed: %s", strerror(errno));
            wl_display_cancel_read(display);
            wayland_shutdown();
            break;
        }

        if (pr > 0) {
            /* Check Wayland FD */
            if (pfds[0].revents & POLLIN) {
                if (wl_display_read_events(display) == 0) {
                    wl_display_dispatch_pending(display);
                } else {
                    LOG_WARN("[WAYLAND] read_events failed; shutting down.");
                    wl_display_cancel_read(display);
                    wayland_shutdown();
                    break;
                }
            } else {
                wl_display_cancel_read(display);
            }
            
            /* Check for Wayland errors */
            if (pfds[0].revents & (POLLERR | POLLHUP)) {
                LOG_ERROR("[WAYLAND] Compositor connection error");
                wayland_shutdown();
                break;
            }
            
            /* Check Hyprland events FD */
            if (hypr_events_poll_idx >= 0 && 
                (pfds[hypr_events_poll_idx].revents & POLLIN)) {
                /* Events will be processed at start of next loop */
            }
            
            /* Check for Hyprland event socket errors */
            if (hypr_events_poll_idx >= 0 &&
                (pfds[hypr_events_poll_idx].revents & (POLLERR | POLLHUP))) {
                LOG_WARN("[WAYLAND] Hyprland event socket disconnected");
                hypr_events_disconnect(g_hypr_events_fd);
                g_hypr_events_fd = -1;
                
                /* Compact the poll array by moving last element to this position */
                if (hypr_events_poll_idx < nfds - 1) {
                    pfds[hypr_events_poll_idx] = pfds[nfds - 1];
                }
                hypr_events_poll_idx = -1;
                nfds--;
            }

            /* IPC FD activity will be processed at start of next iteration */
        } else {
            /* Timeout; cancel read so we can check state again */
            wl_display_cancel_read(display);
        }
    }
}

/* ============================================================================
 * Shutdown and Cleanup
 * ============================================================================ */

void wayland_shutdown() {
    LOG_DEBUG("[WAYLAND] Shutting down...");
    
    /* Close Hyprland event socket */
    if (g_hypr_events_fd >= 0) {
        hypr_events_disconnect(g_hypr_events_fd);
        g_hypr_events_fd = -1;
    }
    
    /* Destroy layer surface early to let compositor reclaim resources */
    if (layer_surface) { 
        zwlr_layer_surface_v1_destroy(layer_surface); 
        layer_surface = NULL; 
    }

    /* Free client list and titles */
    free_client_list();
    free_titles();
    
    /* Free address tracking strings */
    free(g_initial_focus_address);
    g_initial_focus_address = NULL;
    free(g_selected_address);
    g_selected_address = NULL;

    /* Reset indices */
    g_selection_index = -1;
    g_initial_focus_index = -1;

    /* Input after layer surface so no more events target destroyed surface */
    input_shutdown();

    if (surface) { wl_surface_destroy(surface); surface = NULL; }
    if (layer_shell) { zwlr_layer_shell_v1_destroy(layer_shell); layer_shell = NULL; }
    if (compositor) { wl_compositor_destroy(compositor); compositor = NULL; }
    if (shm) { wl_shm_destroy(shm); shm = NULL; }
    if (seat) { wl_seat_destroy(seat); seat = NULL; }
    if (display) { wl_display_disconnect(display); display = NULL; }
    
    LOG_INFO("[WAYLAND] Shutdown complete");
}
