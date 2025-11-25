#pragma once
/*
 * hypr_events.h - Hyprland event socket subscription for dynamic window updates
 *
 * This module connects to Hyprland's event socket (socket2) to receive
 * real-time notifications about window events:
 *   - openwindow    : A new window was opened
 *   - closewindow   : A window was closed
 *   - activewindow  : The active window changed
 *   - movewindow    : A window was moved to another workspace
 *
 * The event socket is located at:
 *   $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock
 *
 * Events are delivered as newline-terminated strings in the format:
 *   EVENT>>DATA
 *
 * Example:
 *   openwindow>>5c4fe19a0,1,kitty,Kitty Terminal
 *   closewindow>>5c4fe19a0
 *   activewindow>>kitty,Kitty Terminal
 */

#ifndef HYPR_EVENTS_H
#define HYPR_EVENTS_H

#include <stdbool.h>

/* Event types */
typedef enum {
    HYPR_EVENT_NONE = 0,
    HYPR_EVENT_OPEN_WINDOW,
    HYPR_EVENT_CLOSE_WINDOW,
    HYPR_EVENT_ACTIVE_WINDOW,
    HYPR_EVENT_MOVE_WINDOW,
    HYPR_EVENT_UNKNOWN
} HyprEventType;

/* Parsed event data */
typedef struct {
    HyprEventType type;
    char address[32];      /* Window address (hex, e.g., "5c4fe19a0") */
    char window_class[128]; /* Window class/app_id */
    char title[256];       /* Window title */
    int workspace_id;      /* Workspace ID (for movewindow) */
} HyprEvent;

/*
 * Connect to Hyprland's event socket (socket2).
 *
 * Returns:
 *   >= 0: Connected socket FD (non-blocking)
 *   -1:   Error (logged)
 */
int hypr_events_connect(void);

/*
 * Read and parse pending events from the event socket.
 * This function is non-blocking and may return HYPR_EVENT_NONE
 * if no complete event is available yet.
 *
 * @param fd    Event socket FD from hypr_events_connect()
 * @param event Output structure to fill with parsed event data
 *
 * Returns:
 *   true:  An event was parsed and stored in `event`
 *   false: No complete event available (try again later)
 */
bool hypr_events_read(int fd, HyprEvent *event);

/*
 * Check if there are more buffered events to read.
 * Call hypr_events_read() in a loop until this returns false.
 *
 * Returns:
 *   true:  More events may be available in the buffer
 *   false: Buffer is empty, wait for more data
 */
bool hypr_events_pending(void);

/*
 * Close the event socket and clean up resources.
 *
 * @param fd Event socket FD to close
 */
void hypr_events_disconnect(int fd);

/*
 * Get a human-readable name for an event type.
 *
 * @param type Event type enum value
 *
 * Returns:
 *   Static string describing the event type
 */
const char *hypr_event_type_name(HyprEventType type);

#endif /* HYPR_EVENTS_H */