#define _POSIX_C_SOURCE 200809L

#include "hypr_events.h"
#include "logger/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Buffer for accumulating partial event data */
#define EVENT_BUFFER_SIZE 4096
static char s_event_buffer[EVENT_BUFFER_SIZE];
static size_t s_buffer_len = 0;

/*
 * Connect to Hyprland's event socket (socket2).
 */
int hypr_events_connect(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");

    if (!xdg || xdg[0] == '\0') {
        LOG_ERROR("[HYPR_EVENTS] XDG_RUNTIME_DIR not set");
        return -1;
    }

    if (!sig || sig[0] == '\0') {
        LOG_ERROR("[HYPR_EVENTS] HYPRLAND_INSTANCE_SIGNATURE not set");
        return -1;
    }

    char path[256];
    int ret = snprintf(path, sizeof(path), "%s/hypr/%s/.socket2.sock", xdg, sig);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        LOG_ERROR("[HYPR_EVENTS] Event socket path too long");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("[HYPR_EVENTS] socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
   strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
   addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("[HYPR_EVENTS] connect(%s) failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Set non-blocking for integration with event loop */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Clear the buffer */
    s_buffer_len = 0;
    s_event_buffer[0] = '\0';

    LOG_INFO("[HYPR_EVENTS] Connected to event socket: %s", path);
    return fd;
}

/*
 * Parse a single event line in the format "EVENT>>DATA"
 */
static bool parse_event_line(const char *line, HyprEvent *event) {
    if (!line || !event) {
        return false;
    }

    /* Initialize event */
    memset(event, 0, sizeof(*event));
    event->type = HYPR_EVENT_NONE;
    event->workspace_id = -1;

    /* Find the >> separator */
    const char *sep = strstr(line, ">>");
    if (!sep) {
        LOG_DEBUG("[HYPR_EVENTS] Invalid event format (no >>): %s", line);
        return false;
    }

    /* Extract event name */
    size_t name_len = sep - line;
    char event_name[64];
    if (name_len >= sizeof(event_name)) {
        name_len = sizeof(event_name) - 1;
    }
    strncpy(event_name, line, name_len);
    event_name[name_len] = '\0';

    /* Get data part */
    const char *data = sep + 2;

    /* Determine event type and parse data */
    if (strcmp(event_name, "openwindow") == 0) {
        event->type = HYPR_EVENT_OPEN_WINDOW;
        /* Format: ADDRESS,WORKSPACE_ID,CLASS,TITLE */
        /* Example: 5c4fe19a0,1,kitty,Kitty Terminal */
        char *data_copy = strdup(data);
        if (data_copy) {
            char *saveptr;
            char *token;
            int field = 0;

            token = strtok_r(data_copy, ",", &saveptr);
            while (token && field < 4) {
                switch (field) {
                    case 0: /* address */
                        snprintf(event->address, sizeof(event->address), "0x%s", token);
                        break;
                    case 1: /* workspace_id */
                        event->workspace_id = atoi(token);
                        break;
                    case 2: /* class */
                        strncpy(event->window_class, token, sizeof(event->window_class) - 1);
                        break;
                    case 3: /* title - may contain commas, so get rest */
                        strncpy(event->title, token, sizeof(event->title) - 1);
                        while ((token = strtok_r(NULL, ",", &saveptr)) != NULL) {
                            size_t cur_len = strlen(event->title);
                            size_t remaining = sizeof(event->title) - cur_len - 1;

                            if (remaining > 0) {
                                event->title[cur_len] = ',';
                                event->title[cur_len + 1] = '\0';
                                cur_len += 1;
                                remaining -= 1;
                            }

                            strncat(event->title, token, remaining);
                        }
                        break;
                }
                if (field < 3) {
                    token = strtok_r(NULL, ",", &saveptr);
                }
                field++;
            }
            free(data_copy);
        }
        LOG_DEBUG("[HYPR_EVENTS] openwindow: addr=%s ws=%d class=%s title=%s",
                  event->address, event->workspace_id, event->window_class, event->title);

    } else if (strcmp(event_name, "closewindow") == 0) {
        event->type = HYPR_EVENT_CLOSE_WINDOW;
        /* Format: ADDRESS */
        snprintf(event->address, sizeof(event->address), "0x%s", data);
        LOG_DEBUG("[HYPR_EVENTS] closewindow: addr=%s", event->address);

    } else if (strcmp(event_name, "activewindow") == 0) {
        event->type = HYPR_EVENT_ACTIVE_WINDOW;
        /* Format: CLASS,TITLE */
        char *data_copy = strdup(data);
        if (data_copy) {
            char *comma = strchr(data_copy, ',');
            if (comma) {
                *comma = '\0';
                strncpy(event->window_class, data_copy, sizeof(event->window_class) - 1);
                strncpy(event->title, comma + 1, sizeof(event->title) - 1);
            } else {
                strncpy(event->window_class, data_copy, sizeof(event->window_class) - 1);
            }
            free(data_copy);
        }
        LOG_DEBUG("[HYPR_EVENTS] activewindow: class=%s title=%s",
                  event->window_class, event->title);

    } else if (strcmp(event_name, "movewindow") == 0) {
        event->type = HYPR_EVENT_MOVE_WINDOW;
        /* Format: ADDRESS,WORKSPACE_NAME */
        char *data_copy = strdup(data);
        if (data_copy) {
            char *comma = strchr(data_copy, ',');
            if (comma) {
                *comma = '\0';
                snprintf(event->address, sizeof(event->address), "0x%s", data_copy);
                /* Try to parse workspace ID from name */
                event->workspace_id = atoi(comma + 1);
            }
            free(data_copy);
        }
        LOG_DEBUG("[HYPR_EVENTS] movewindow: addr=%s ws=%d",
                  event->address, event->workspace_id);

    } else {
        event->type = HYPR_EVENT_UNKNOWN;
        LOG_DEBUG("[HYPR_EVENTS] Unknown event: %s", event_name);
        return false;
    }

    return true;
}

/*
 * Read and parse pending events from the event socket.
 */
bool hypr_events_read(int fd, HyprEvent *event) {
    if (fd < 0 || !event) {
        return false;
    }

    /* First, check if we have a complete line in the buffer */
    char *newline = strchr(s_event_buffer, '\n');
    if (newline) {
        /* Extract the line */
        *newline = '\0';
        bool parsed = parse_event_line(s_event_buffer, event);

        /* Shift buffer to remove processed line */
        size_t line_len = newline - s_event_buffer + 1;
        size_t remaining = s_buffer_len - line_len;
        if (remaining > 0) {
            memmove(s_event_buffer, newline + 1, remaining);
        }
        s_buffer_len = remaining;
        s_event_buffer[s_buffer_len] = '\0';

        if (parsed) {
            return true;
        }
        /* If parsing failed, try to read more or return next line */
    }

    /* Try to read more data */
    size_t space = EVENT_BUFFER_SIZE - s_buffer_len - 1;
    if (space > 0) {
        ssize_t nread = read(fd, s_event_buffer + s_buffer_len, space);
        if (nread > 0) {
            s_buffer_len += nread;
            s_event_buffer[s_buffer_len] = '\0';

            /* Check again for complete line */
            newline = strchr(s_event_buffer, '\n');
            if (newline) {
                *newline = '\0';
                bool parsed = parse_event_line(s_event_buffer, event);

                size_t line_len = newline - s_event_buffer + 1;
                size_t remaining = s_buffer_len - line_len;
                if (remaining > 0) {
                    memmove(s_event_buffer, newline + 1, remaining);
                }
                s_buffer_len = remaining;
                s_event_buffer[s_buffer_len] = '\0';

                return parsed;
            }
        } else if (nread < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_WARN("[HYPR_EVENTS] read() failed: %s", strerror(errno));
            }
        } else {
            /* nread == 0: connection closed */
            LOG_WARN("[HYPR_EVENTS] Event socket connection closed");
        }
    }

    return false;
}

/*
 * Check if there are more buffered events to read.
 */
bool hypr_events_pending(void) {
    return (s_buffer_len > 0 && strchr(s_event_buffer, '\n') != NULL);
}

/*
 * Close the event socket and clean up resources.
 */
void hypr_events_disconnect(int fd) {
    if (fd >= 0) {
        close(fd);
        LOG_DEBUG("[HYPR_EVENTS] Disconnected from event socket");
    }

    /* Clear buffer */
    s_buffer_len = 0;
    s_event_buffer[0] = '\0';
}

/*
 * Get a human-readable name for an event type.
 */
const char *hypr_event_type_name(HyprEventType type) {
    switch (type) {
        case HYPR_EVENT_NONE:         return "none";
        case HYPR_EVENT_OPEN_WINDOW:  return "openwindow";
        case HYPR_EVENT_CLOSE_WINDOW: return "closewindow";
        case HYPR_EVENT_ACTIVE_WINDOW: return "activewindow";
        case HYPR_EVENT_MOVE_WINDOW:  return "movewindow";
        case HYPR_EVENT_UNKNOWN:      return "unknown";
        default:                      return "invalid";
    }
}
