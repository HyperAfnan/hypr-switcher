#define _POSIX_C_SOURCE 200809L

#include "switcher_ipc.h"
#include "logger/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* Socket directory and file names */
#define SWITCHER_DIR_NAME "hyprswitcher"
#define SWITCHER_SOCKET_NAME "socket"

/* Static path buffer for socket path (computed once) */
static char s_socket_path[256] = {0};
static char s_socket_dir[256] = {0};
static bool s_paths_initialized = false;

/*
 * Initialize socket paths from environment.
 * Returns 0 on success, -1 on error.
 */
static int init_paths(void) {
    if (s_paths_initialized) {
        return 0;
    }

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || xdg[0] == '\0') {
        LOG_ERROR("[SWITCHER_IPC] XDG_RUNTIME_DIR not set");
        return -1;
    }

    int ret = snprintf(s_socket_dir, sizeof(s_socket_dir), "%s/%s", xdg, SWITCHER_DIR_NAME);
    if (ret < 0 || (size_t)ret >= sizeof(s_socket_dir)) {
        LOG_ERROR("[SWITCHER_IPC] Socket directory path too long");
        return -1;
    }

    ret = snprintf(s_socket_path, sizeof(s_socket_path), "%s/%s", s_socket_dir, SWITCHER_SOCKET_NAME);
    if (ret < 0 || (size_t)ret >= sizeof(s_socket_path)) {
        LOG_ERROR("[SWITCHER_IPC] Socket path too long");
        return -1;
    }

    s_paths_initialized = true;
    LOG_DEBUG("[SWITCHER_IPC] Socket path: %s", s_socket_path);
    return 0;
}

/*
 * Set socket to non-blocking mode.
 */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        LOG_WARN("[SWITCHER_IPC] fcntl(F_GETFL) failed: %s", strerror(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_WARN("[SWITCHER_IPC] fcntl(F_SETFL) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int switcher_ipc_get_socket_path(char *buf, size_t bufsize) {
    if (init_paths() != 0) {
        return -1;
    }
    if (bufsize < strlen(s_socket_path) + 1) {
        return -1;
    }
    strncpy(buf, s_socket_path, bufsize);
    buf[bufsize - 1] = '\0';
    return 0;
}

bool switcher_ipc_socket_exists(void) {
    if (init_paths() != 0) {
        return false;
    }
    struct stat st;
    return (stat(s_socket_path, &st) == 0);
}

int switcher_ipc_try_connect(void) {
    if (init_paths() != 0) {
        return -1;
    }

    /* Quick check if socket file exists */
    struct stat st;
    if (stat(s_socket_path, &st) != 0) {
        LOG_DEBUG("[SWITCHER_IPC] Socket file doesn't exist, no main instance running");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_WARN("[SWITCHER_IPC] socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, s_socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_DEBUG("[SWITCHER_IPC] connect() failed: %s (stale socket?)", strerror(errno));
        close(fd);
        /* Try to remove stale socket */
        unlink(s_socket_path);
        return -1;
    }

    LOG_INFO("[SWITCHER_IPC] Connected to existing main instance");
    return fd;
}

int switcher_ipc_send(int fd, const char *command) {
    if (fd < 0 || !command) {
        return -1;
    }

    char msg[SWITCHER_IPC_MSG_SIZE];
    memset(msg, 0, sizeof(msg));
    strncpy(msg, command, SWITCHER_IPC_MSG_SIZE - 1);

    ssize_t written = write(fd, msg, SWITCHER_IPC_MSG_SIZE);
    if (written != SWITCHER_IPC_MSG_SIZE) {
        LOG_WARN("[SWITCHER_IPC] write() failed: wrote %zd of %d bytes: %s",
                 written, SWITCHER_IPC_MSG_SIZE, strerror(errno));
        return -1;
    }

    LOG_INFO("[SWITCHER_IPC] Sent command: %s", command);
    return 0;
}

int switcher_ipc_listen(void) {
    if (init_paths() != 0) {
        return -1;
    }

    /* Create directory with secure permissions */
    if (mkdir(s_socket_dir, 0700) < 0 && errno != EEXIST) {
        LOG_ERROR("[SWITCHER_IPC] mkdir(%s) failed: %s", s_socket_dir, strerror(errno));
        return -1;
    }

    /* Verify directory permissions */
    struct stat st;
    if (stat(s_socket_dir, &st) == 0) {
        if ((st.st_mode & 0777) != 0700) {
            LOG_WARN("[SWITCHER_IPC] Directory %s has insecure permissions %o, fixing to 0700",
                     s_socket_dir, st.st_mode & 0777);
            chmod(s_socket_dir, 0700);
        }
    }

    /* Remove any stale socket file */
    unlink(s_socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("[SWITCHER_IPC] socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, s_socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("[SWITCHER_IPC] bind(%s) failed: %s", s_socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Set socket file permissions */
    chmod(s_socket_path, 0600);

    if (listen(fd, 5) < 0) {
        LOG_ERROR("[SWITCHER_IPC] listen() failed: %s", strerror(errno));
        close(fd);
        unlink(s_socket_path);
        return -1;
    }

    /* Set non-blocking for integration with event loop */
    set_nonblocking(fd);

    LOG_INFO("[SWITCHER_IPC] Listening on %s", s_socket_path);
    return fd;
}

int switcher_ipc_accept(int listen_fd) {
    if (listen_fd < 0) {
        return -1;
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No pending connection, not an error */
            return -1;
        }
        LOG_WARN("[SWITCHER_IPC] accept() failed: %s", strerror(errno));
        return -1;
    }

    /* Set client socket to non-blocking */
    set_nonblocking(client_fd);

    LOG_DEBUG("[SWITCHER_IPC] Accepted client connection (fd=%d)", client_fd);
    return client_fd;
}

SwitcherCmdType switcher_ipc_read_command(int client_fd) {
    if (client_fd < 0) {
        return SWITCHER_CMD_TYPE_NONE;
    }

    char msg[SWITCHER_IPC_MSG_SIZE + 1];
    memset(msg, 0, sizeof(msg));

    ssize_t nread = read(client_fd, msg, SWITCHER_IPC_MSG_SIZE);
    if (nread <= 0) {
        if (nread == 0) {
            LOG_DEBUG("[SWITCHER_IPC] Client disconnected");
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SWITCHER_CMD_TYPE_NONE;
        } else {
            LOG_WARN("[SWITCHER_IPC] read() failed: %s", strerror(errno));
        }
        return SWITCHER_CMD_TYPE_NONE;
    }

    /* Null-terminate for safety */
    msg[SWITCHER_IPC_MSG_SIZE] = '\0';

    LOG_DEBUG("[SWITCHER_IPC] Received command: '%s' (%zd bytes)", msg, nread);

    /* Parse command */
    if (strncmp(msg, SWITCHER_CMD_CYCLE_BACKWARD, strlen(SWITCHER_CMD_CYCLE_BACKWARD)) == 0) {
        return SWITCHER_CMD_TYPE_CYCLE_BACKWARD;
    } else if (strncmp(msg, SWITCHER_CMD_CYCLE, strlen(SWITCHER_CMD_CYCLE)) == 0) {
        return SWITCHER_CMD_TYPE_CYCLE;
    } else if (strncmp(msg, SWITCHER_CMD_COMMIT, strlen(SWITCHER_CMD_COMMIT)) == 0) {
        return SWITCHER_CMD_TYPE_COMMIT;
    } else if (strncmp(msg, SWITCHER_CMD_CANCEL, strlen(SWITCHER_CMD_CANCEL)) == 0) {
        return SWITCHER_CMD_TYPE_CANCEL;
    }

    LOG_WARN("[SWITCHER_IPC] Unknown command: '%s'", msg);
    return SWITCHER_CMD_TYPE_UNKNOWN;
}

void switcher_ipc_cleanup(int listen_fd) {
    if (listen_fd >= 0) {
        close(listen_fd);
        LOG_DEBUG("[SWITCHER_IPC] Closed listening socket (fd=%d)", listen_fd);
    }

    if (s_paths_initialized && s_socket_path[0] != '\0') {
        if (unlink(s_socket_path) == 0) {
            LOG_DEBUG("[SWITCHER_IPC] Removed socket file: %s", s_socket_path);
        }

        /* Try to remove directory (will only succeed if empty) */
        if (s_socket_dir[0] != '\0') {
            if (rmdir(s_socket_dir) == 0) {
                LOG_DEBUG("[SWITCHER_IPC] Removed socket directory: %s", s_socket_dir);
            }
        }
    }

    LOG_INFO("[SWITCHER_IPC] Cleanup complete");
}
