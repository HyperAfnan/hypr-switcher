#pragma once
/*
 * switcher_ipc.h - UNIX domain socket IPC for hyprswitcher single-instance coordination
 *
 * This module implements the process model where:
 * - First invocation becomes the "main instance" (creates socket, shows overlay)
 * - Subsequent invocations become "helper instances" (send command to main, exit immediately)
 *
 * Socket location: $XDG_RUNTIME_DIR/hyprswitcher/socket
 *
 * Commands (fixed 16-byte messages, null-padded):
 *   "CYCLE"          - Cycle selection forward
 *   "CYCLE_BACKWARD" - Cycle selection backward (Shift+Tab)
 *   "COMMIT"         - Commit current selection and close
 *   "CANCEL"         - Cancel and restore original focus
 */

#ifndef SWITCHER_IPC_H
#define SWITCHER_IPC_H

#include <stdbool.h>
#include <stddef.h>

/* Fixed message size for IPC commands */
#define SWITCHER_IPC_MSG_SIZE 16

/* Command strings */
#define SWITCHER_CMD_CYCLE          "CYCLE"
#define SWITCHER_CMD_CYCLE_BACKWARD "CYCLE_BACKWARD"
#define SWITCHER_CMD_COMMIT         "COMMIT"
#define SWITCHER_CMD_CANCEL         "CANCEL"

/* Command type enum for easier handling */
typedef enum {
    SWITCHER_CMD_TYPE_NONE = 0,
    SWITCHER_CMD_TYPE_CYCLE,
    SWITCHER_CMD_TYPE_CYCLE_BACKWARD,
    SWITCHER_CMD_TYPE_COMMIT,
    SWITCHER_CMD_TYPE_CANCEL,
    SWITCHER_CMD_TYPE_UNKNOWN
} SwitcherCmdType;

/*
 * Try to connect to an existing main instance.
 *
 * Returns:
 *   >= 0: Connected socket FD (caller should send command then close)
 *   -1:   No main instance running (socket doesn't exist or connection refused)
 */
int switcher_ipc_try_connect(void);

/*
 * Send a command to an existing main instance.
 *
 * @param fd      Socket FD from switcher_ipc_try_connect()
 * @param command Command string (e.g., SWITCHER_CMD_CYCLE)
 *
 * Returns:
 *   0:  Success
 *   -1: Error
 */
int switcher_ipc_send(int fd, const char *command);

/*
 * Create and bind the listening socket (main instance).
 * Creates directory $XDG_RUNTIME_DIR/hyprswitcher with mode 0700 if needed.
 *
 * Returns:
 *   >= 0: Listening socket FD (non-blocking)
 *   -1:   Error (logged)
 */
int switcher_ipc_listen(void);

/*
 * Accept a pending connection on the listening socket.
 *
 * @param listen_fd Listening socket FD from switcher_ipc_listen()
 *
 * Returns:
 *   >= 0: Client socket FD (non-blocking)
 *   -1:   No pending connection or error
 */
int switcher_ipc_accept(int listen_fd);

/*
 * Read a command from a connected client socket.
 *
 * @param client_fd Client socket FD from switcher_ipc_accept()
 *
 * Returns:
 *   Command type enum value
 *   SWITCHER_CMD_TYPE_NONE if no data available
 *   SWITCHER_CMD_TYPE_UNKNOWN if unrecognized command
 */
SwitcherCmdType switcher_ipc_read_command(int client_fd);

/*
 * Get the socket file path.
 * Useful for logging/debugging.
 *
 * @param buf     Buffer to write path into
 * @param bufsize Size of buffer
 *
 * Returns:
 *   0:  Success
 *   -1: Error (XDG_RUNTIME_DIR not set or buffer too small)
 */
int switcher_ipc_get_socket_path(char *buf, size_t bufsize);

/*
 * Cleanup: close socket FD, unlink socket file, remove directory if empty.
 * Safe to call multiple times or with fd=-1.
 *
 * @param listen_fd Listening socket FD to close (-1 to skip)
 */
void switcher_ipc_cleanup(int listen_fd);

/*
 * Check if socket file exists (quick check without connecting).
 *
 * Returns:
 *   true:  Socket file exists
 *   false: Socket file doesn't exist
 */
bool switcher_ipc_socket_exists(void);

#endif /* SWITCHER_IPC_H */