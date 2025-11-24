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

static int hypr_ipc_send_recv(const char *cmd, char **out_json) {
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

void hypr_ipc_print_clients() {
    char *resp = NULL;
    if (hypr_ipc_send_recv("j/clients", &resp) != 0) {
        LOG_INFO("[IPC] Failed to fetch clients\n");
        return;
    }

    json_object *parsed = json_tokener_parse(resp);
    free(resp);
    if (!parsed || !json_object_is_type(parsed, json_type_array)) {
        if (parsed) json_object_put(parsed);
        LOG_INFO("[IPC] Invalid clients payload\n");
        return;
    }

    int count = json_object_array_length(parsed);
    LOG_INFO("Windows:\n");
    for (int i = 0; i < count; i++) {
        json_object *c = json_object_array_get_idx(parsed, i);
        LOG_DEBUG(" logging c: %d", c);

        const char *title =
            json_object_get_string(json_object_object_get(c, "title"));
        const char *addr =
            json_object_get_string(json_object_object_get(c, "address"));

        LOG_INFO(" - %s (%s)\n", title ? title : "(untitled)", addr ? addr : "?");
    }
    json_object_put(parsed);
}

int hypr_ipc_get_client_titles(char ***titles_out, size_t *count_out) {
    if (!titles_out || !count_out) {
        return -1;
    }

    LOG_DEBUG("Fetching client titles");
    LOG_DEBUG("title out %d", titles_out);
    LOG_DEBUG("count out %d", count_out);
    char *resp = NULL;
    if (hypr_ipc_send_recv("j/clients", &resp) != 0) {
        return -1;
    }

    json_object *parsed = json_tokener_parse(resp);

    free(resp);
    if (!parsed || !json_object_is_type(parsed, json_type_array)) {
        if (parsed) json_object_put(parsed);
        return -1;
    }

    const char *json_str = json_object_to_json_string(parsed);
    LOG_DEBUG("json string %s", json_str);

    int icount = json_object_array_length(parsed);
    size_t count = (size_t)icount;

    char **titles = NULL;
    if (count > 0) {
        titles = (char **)calloc(count, sizeof(char *));
        if (!titles) {
            json_object_put(parsed);
            return -1;
        }
    }

    for (int i = 0; i < icount; i++) {
        json_object *c = json_object_array_get_idx(parsed, i);
        const char *title =
            json_object_get_string(json_object_object_get(c, "initialClass"));
        if (!title || title[0] == '\0') {
            title = "(untitled)";
        }
        titles[i] = strdup(title);
        if (!titles[i]) {
            for (int j = 0; j < i; j++) {
                free(titles[j]);
            }
            free(titles);
            json_object_put(parsed);
            return -1;
        }
    }

    *titles_out = titles;
    *count_out = count;

    json_object_put(parsed);
    return 0;
}

void hypr_ipc_free_titles(char **titles, size_t count) {
    if (!titles) return;
    for (size_t i = 0; i < count; i++) {
        free(titles[i]);
    }
    free(titles);
}
