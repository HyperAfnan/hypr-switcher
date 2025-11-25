#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include "util.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <logger/logger.h>

static int hypr_open_socket(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!xdg || !sig) {
        LOG_DEBUG("[IPC] Missing XDG_RUNTIME_DIR or HYPRLAND_INSTANCE_SIGNATURE\n");
        return -1;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/hypr/%s/.socket.sock", xdg, sig);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_DEBUG("[IPC] socket() failed\n");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_DEBUG("[IPC] connect() failed\n");
        close(fd);
        return -1;
    }

    return fd;
}

int hypr_ipc_send_recv(const char *cmd, char **out_json) {
    if (!cmd || !out_json) return -1;

    int fd = hypr_open_socket();
    if (fd < 0) return -1;

    /* Send NUL-terminated command as Hyprland expects */
    size_t to_write = strlen(cmd) + 1;
    ssize_t w = write(fd, cmd, to_write);
    if (w < 0 || (size_t)w != to_write) {
        LOG_DEBUG("[IPC] write() failed\n");
        close(fd);
        return -1;
    }

    /* Non-blocking read with poll-based timeout */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int timeout_ms = 3000;

    /* Incremental JSON parsing: stop as soon as a full JSON document is parsed */
    json_tokener *tok = json_tokener_new();
    if (!tok) {
        close(fd);
        return -1;
    }
    json_object *obj = NULL;

    for (;;) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOG_DEBUG("[IPC] poll() failed\n");
            json_tokener_free(tok);
            close(fd);
            return -1;
        } else if (pr == 0) {
            LOG_DEBUG("[IPC] read timeout\n");
            json_tokener_free(tok);
            close(fd);
            return -1;
        }

        for (;;) {
            char tmp[4096];
            ssize_t r = read(fd, tmp, sizeof(tmp));
            if (r > 0) {
                obj = json_tokener_parse_ex(tok, tmp, (int)r);
                enum json_tokener_error jerr = json_tokener_get_error(tok);
                if (obj && jerr == json_tokener_success) {
                    /* We have a complete JSON object; stop reading */
                    goto got_json;
                }
                if (jerr == json_tokener_continue) {
                    /* Need more data; try reading again without polling */
                    continue;
                }
                if (jerr != json_tokener_success) {
                    LOG_DEBUG("[IPC] JSON parse error: %s\n", json_tokener_error_desc(jerr));
                    json_tokener_free(tok);
                    close(fd);
                    return -1;
                }
            } else if (r == 0) {
                /* EOF reached before full JSON parsed */
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Back to poll for more data */
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    LOG_DEBUG("[IPC] read() failed\n");
                    json_tokener_free(tok);
                    close(fd);
                    return -1;
                }
            }
        }
        /* loop to poll again */
    }

got_json:
    close(fd);
    const char *js = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    if (!js) {
        json_object_put(obj);
        json_tokener_free(tok);
        return -1;
    }
    char *dup = strdup(js);
    json_object_put(obj);
    json_tokener_free(tok);
    if (!dup) return -1;
    *out_json = dup;
    return 0;
}

void hypr_ipc_connect() {
    int fd = hypr_open_socket();
    if (fd < 0) {
        DIE("IPC connect failed\n");
        return;
    }
    LOG_INFO("[IPC] Connected to Hyprland.\n");
    close(fd);
}

/* Removed unused hypr_ipc_print_clients() */

/* Removed unused hypr_ipc_get_client_titles() */

void hypr_ipc_free_titles(char **titles, size_t count) {
    if (!titles) return;
    for (size_t i = 0; i < count; i++) {
        free(titles[i]);
    }
    free(titles);
}

/* ---- Focused client API (focusHistoryID == 0) ---- */

static char *dup_json_string_field(json_object *obj, const char *key) {
    if (!obj || !key) return NULL;
    json_object *v = json_object_object_get(obj, key);
    if (!v) return NULL;
    const char *s = json_object_get_string(v);
    return s ? strdup(s) : NULL;
}

static int get_workspace_id_from_client(json_object *c) {
    int ws_id = -1;
    if (!c) return -1;
    json_object *ws = json_object_object_get(c, "workspace");
    if (!ws) return -1;

    if (json_object_is_type(ws, json_type_object)) {
        json_object *id = json_object_object_get(ws, "id");
        if (id && json_object_is_type(id, json_type_int))
            ws_id = json_object_get_int(id);
    } else if (json_object_is_type(ws, json_type_int)) {
        ws_id = json_object_get_int(ws);
    }
    return ws_id;
}

/* Removed unused hypr_ipc_get_focused_client() */

void hypr_ipc_free_client_info(HyprClientInfo *info) {
    if (!info) return;
    free(info->address);
    free(info->title);
    free(info->app_class);
    info->address = NULL;
    info->title = NULL;
    info->app_class = NULL;
}

/* Return a heap-allocated array of HyprClientInfo for all clients.
   Each entry contains: address, title, app_class, workspace_id, pid, focusHistoryID,
   and focused (true if focusHistoryID == 0).
   Caller must free with hypr_ipc_free_client_infos.
   Returns 0 on success (even if zero clients), -1 on error. */
int hypr_ipc_get_clients_basic(HyprClientInfo **list_out, size_t *count_out) {
    if (!list_out || !count_out) return -1;
    *list_out = NULL;
    *count_out = 0;

    char *resp = NULL;
    if (hypr_ipc_send_recv("j/clients", &resp) != 0) {
        return -1;
    }

    json_object *arr = json_tokener_parse(resp);
    free(resp);
    if (!arr || !json_object_is_type(arr, json_type_array)) {
        if (arr) json_object_put(arr);
        return -1;
    }

    int len = json_object_array_length(arr);
    if (len <= 0) {
        json_object_put(arr);
        return 0; /* success, empty list */
    }

    HyprClientInfo *list = calloc((size_t)len, sizeof(HyprClientInfo));
    if (!list) {
        json_object_put(arr);
        return -1;
    }

    size_t n = 0;
    for (int i = 0; i < len; i++) {
        json_object *c = json_object_array_get_idx(arr, i);
        if (!c || !json_object_is_type(c, json_type_object)) continue;

        HyprClientInfo info;
        memset(&info, 0, sizeof(info));
        info.workspace_id = get_workspace_id_from_client(c);
        info.pid = -1;
        info.focusHistoryID = -1;
        info.focused = false;

        json_object *pid_obj = json_object_object_get(c, "pid");
        if (pid_obj && json_object_is_type(pid_obj, json_type_int))
            info.pid = json_object_get_int(pid_obj);

        json_object *fh_obj = json_object_object_get(c, "focusHistoryID");
        if (fh_obj && json_object_is_type(fh_obj, json_type_int)) {
            info.focusHistoryID = json_object_get_int(fh_obj);
            if (info.focusHistoryID == 0)
                info.focused = true;
        }

        info.address = dup_json_string_field(c, "address");
        info.title   = dup_json_string_field(c, "title");
        if (!info.title || info.title[0] == '\0') {
            free(info.title);
            info.title = strdup("(untitled)");
        }
        info.app_class = dup_json_string_field(c, "class");
        if (!info.app_class)
            info.app_class = dup_json_string_field(c, "initialClass");

        list[n++] = info;
    }

    json_object_put(arr);

    if (n == 0) {
        free(list);
        return 0;
    }

    *list_out = list;
    *count_out = n;
    return 0;
}

/* Free an array produced by hypr_ipc_get_clients_basic */
void hypr_ipc_free_client_infos(HyprClientInfo *infos, size_t count) {
    if (!infos) return;
    for (size_t i = 0; i < count; i++) {
        hypr_ipc_free_client_info(&infos[i]);
    }
    free(infos);
}

/* ================= Multi-strategy focus (address, class, title) =================
   Hyprland sometimes requires explicit prefixes (address:, class:, title:) or treats
   the argument as a regex. We attempt several patterns until one succeeds.
   Success heuristic: response does NOT contain "No such window found".
*/

int hypr_ipc_send_command_capture(const char *cmd, char *resp, size_t resp_len) {
    int fd = hypr_open_socket();
    if (fd < 0) {
        LOG_WARN("[IPC] send_command_capture: socket open failed for '%s'", cmd);
        return -1;
    }

    size_t len = strlen(cmd) + 1; /* NUL terminated per protocol */
    ssize_t w = write(fd, cmd, len);
    if (w != (ssize_t)len) {
        LOG_WARN("[IPC] send_command_capture: write failed for '%s' (wrote=%zd expected=%zu)", cmd, w, len);
        close(fd);
        return -1;
    }

    if (resp && resp_len) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
            ssize_t r = read(fd, resp, resp_len - 1);
            if (r > 0) {
                resp[r] = '\0';
            } else {
                resp[0] = '\0';
            }
        } else {
            if (resp_len > 0) resp[0] = '\0';
        }
    }

    close(fd);
    return 0;
}

/* Escape regex special chars for literal match; produce ^...$ */
static void hypr_escape_regex(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    size_t pos = 0;
    if (pos < out_len - 1) out[pos++] = '^';
    if (!in) in = "";
    const char *special = ".^$*+?()[]{}|\\";
    for (const char *p = in; *p && pos < out_len - 2; ++p) {
        if (strchr(special, *p)) {
            if (pos < out_len - 2) out[pos++] = '\\';
        }
        out[pos++] = *p;
    }
    if (pos < out_len - 1) out[pos++] = '$';
    out[pos] = '\0';
}

/* Validate address (0x + hex) */
static int validate_address_multi(const char *address) {
    if (!address) return -1;
    if (strncmp(address, "0x", 2) != 0) return -1;
    const char *p = address + 2;
    if (*p == '\0') return -1;
    for (; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') ||
              (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F'))) {
            return -1;
        }
    }
    return 0;
}

/* Attempt focusing by address only (address: prefix then raw). Returns 0 if any succeeds. */
int hypr_ipc_focus_address(const char *address) {
    LOG_DEBUG("[IPC] multi-focus address attempt address='%s'", address ? address : "(null)");
    if (validate_address_multi(address) != 0) {
        LOG_WARN("[IPC] Invalid address format '%s'", address ? address : "(null)");
        return -1;
    }

    char resp[256];
    int overall_rc = -1;

    /* Try with address: prefix */
    {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "dispatch focuswindow address:%s", address);
        if (hypr_ipc_send_command_capture(cmd, resp, sizeof resp) == 0) {
            if (resp[0] == '\0' || !strstr(resp, "No such window found")) {
                LOG_INFO("[IPC] Focus success (address: prefix) '%s'", address);
                return 0;
            }
            LOG_DEBUG("[IPC] address: attempt failed response='%s'", resp);
        }
    }

    /* Try raw address */
    {
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "dispatch focuswindow %s", address);
        if (hypr_ipc_send_command_capture(cmd, resp, sizeof resp) == 0) {
            if (resp[0] == '\0' || !strstr(resp, "No such window found")) {
                LOG_INFO("[IPC] Focus success (raw address) '%s'", address);
                return 0;
            }
            LOG_DEBUG("[IPC] raw address attempt failed response='%s'", resp);
        }
    }

    LOG_WARN("[IPC] Focus by address failed '%s'", address);
    return overall_rc;
}

/* Full multi-strategy: address, class, title */
int hypr_ipc_focus_client(const HyprClientInfo *client) {
    LOG_DEBUG("[IPC] multi-focus client ptr=%p", (void*)client);
    if (!client) {
        LOG_WARN("[IPC] focus_client: NULL client");
        return -1;
    }

    /* 1. Address attempts */
    if (client->address) {
        if (hypr_ipc_focus_address(client->address) == 0) {
            return 0;
        }
    }

    char resp[256];

    /* 2. Class attempt (escaped) */
    if (client->app_class && client->app_class[0]) {
        char escaped[256];
        hypr_escape_regex(client->app_class, escaped, sizeof escaped);
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "dispatch focuswindow class:%s", escaped);
        LOG_DEBUG("[IPC] class attempt cmd='%s'", cmd);
        if (hypr_ipc_send_command_capture(cmd, resp, sizeof resp) == 0) {
            if (resp[0] == '\0' || !strstr(resp, "No such window found")) {
                LOG_INFO("[IPC] Focus success (class) '%s'", client->app_class);
                return 0;
            }
            LOG_DEBUG("[IPC] class attempt failed response='%s'", resp);
        }
    }

    /* 3. Title attempt (escaped) */
    if (client->title && client->title[0]) {
        char escaped[256];
        hypr_escape_regex(client->title, escaped, sizeof escaped);
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "dispatch focuswindow title:%s", escaped);
        LOG_DEBUG("[IPC] title attempt cmd='%s'", cmd);
        if (hypr_ipc_send_command_capture(cmd, resp, sizeof resp) == 0) {
            if (resp[0] == '\0' || !strstr(resp, "No such window found")) {
                LOG_INFO("[IPC] Focus success (title) '%s'", client->title);
                return 0;
            }
            LOG_DEBUG("[IPC] title attempt failed response='%s'", resp);
        }
    }

    LOG_WARN("[IPC] All focus attempts failed (address=%s class=%s title=%s)",
             client->address ? client->address : "(null)",
             client->app_class ? client->app_class : "(null)",
             client->title ? client->title : "(null)");
    return -1;
}
