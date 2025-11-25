#include "wayland.h"
#include "ipc.h"
#include "switcher_ipc.h"
#include "config.h"
#include "logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * hyprswitcher - Wayland-native Alt-Tab window switcher for Hyprland
 *
 * Process model:
 *   - First invocation: becomes "main instance"
 *     - Creates UNIX domain socket at $XDG_RUNTIME_DIR/hyprswitcher/socket
 *     - Shows overlay, handles keyboard input
 *     - Listens for commands from helper instances
 *
 *   - Subsequent invocations: become "helper instances"
 *     - Connect to existing socket
 *     - Send command (CYCLE, CYCLE_BACKWARD, COMMIT, CANCEL)
 *     - Exit immediately
 *
 * This allows Hyprland to use a simple binding:
 *   bind = ALT, TAB, exec, hyprswitcher
 *
 * Each Alt+Tab press spawns hyprswitcher, but only the first one shows the overlay.
 * Subsequent presses just send cycle commands to the existing instance.
 */

typedef enum {
    CMD_CYCLE,
    CMD_CYCLE_BACKWARD,
    CMD_COMMIT,
    CMD_CANCEL
} CommandType;

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --backward, -b    Send CYCLE_BACKWARD instead of CYCLE (for Shift+Alt+Tab)\n");
    fprintf(stderr, "  --commit, -c      Send COMMIT to focus selected window and close overlay\n");
    fprintf(stderr, "  --cancel, -x      Send CANCEL to restore original focus and close overlay\n");
    fprintf(stderr, "  --help, -h        Show this help message\n");
    fprintf(stderr, "\nIf a main instance is already running, sends the specified command and exits.\n");
    fprintf(stderr, "Otherwise, becomes the main instance and shows the overlay.\n");
    fprintf(stderr, "\nDefault command is CYCLE (forward cycling).\n");
}

static const char *command_to_string(CommandType cmd) {
    switch (cmd) {
        case CMD_CYCLE:          return SWITCHER_CMD_CYCLE;
        case CMD_CYCLE_BACKWARD: return SWITCHER_CMD_CYCLE_BACKWARD;
        case CMD_COMMIT:         return SWITCHER_CMD_COMMIT;
        case CMD_CANCEL:         return SWITCHER_CMD_CANCEL;
        default:                 return SWITCHER_CMD_CYCLE;
    }
}

static const char *command_name(CommandType cmd) {
    switch (cmd) {
        case CMD_CYCLE:          return "CYCLE";
        case CMD_CYCLE_BACKWARD: return "CYCLE_BACKWARD";
        case CMD_COMMIT:         return "COMMIT";
        case CMD_CANCEL:         return "CANCEL";
        default:                 return "CYCLE";
    }
}

int main(int argc, char *argv[]) {
    /* Parse arguments */
    CommandType command = CMD_CYCLE;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backward") == 0 || strcmp(argv[i], "-b") == 0) {
            command = CMD_CYCLE_BACKWARD;
        } else if (strcmp(argv[i], "--commit") == 0 || strcmp(argv[i], "-c") == 0) {
            command = CMD_COMMIT;
        } else if (strcmp(argv[i], "--cancel") == 0 || strcmp(argv[i], "-x") == 0) {
            command = CMD_CANCEL;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Initialize logger (level can be overridden by HYPRSWITCHER_LOG env var) */
    if (log_init("logger.log", LOG_INFO) != 0) {
        fprintf(stderr, "Failed to initialize logger\n");
        return 1;
    }
    
    /* Load configuration (uses defaults if no config file found) */
    config_load();

    LOG_INFO("[MAIN] hyprswitcher starting (command=%s)", command_name(command));

    /*
     * Try to connect to an existing main instance.
     * If successful, we're a helper instance: send command and exit.
     */
    int conn_fd = switcher_ipc_try_connect();
    if (conn_fd >= 0) {
        /* Helper instance mode */
        const char *cmd_str = command_to_string(command);
        LOG_INFO("[MAIN] Connected to main instance, sending %s", cmd_str);

        int ret = switcher_ipc_send(conn_fd, cmd_str);
        close(conn_fd);

        if (ret != 0) {
            LOG_ERROR("[MAIN] Failed to send command to main instance");
            log_close();
            return 1;
        }

        LOG_INFO("[MAIN] Helper instance exiting after sending command");
        log_close();
        return 0;
    }

    /*
     * No existing main instance found.
     * 
     * For COMMIT and CANCEL commands, there's nothing to do if no instance exists.
     */
    if (command == CMD_COMMIT || command == CMD_CANCEL) {
        LOG_INFO("[MAIN] No main instance running, %s command ignored", command_name(command));
        log_close();
        return 0;
    }

    /*
     * We become the main instance.
     */
    LOG_INFO("[MAIN] No existing instance, becoming main instance");

    /* Verify Hyprland IPC is available */
    hypr_ipc_connect();

    /* Create listening socket for helper instances */
    int listen_fd = switcher_ipc_listen();
    if (listen_fd < 0) {
        LOG_ERROR("[MAIN] Failed to create IPC socket");
        log_close();
        return 1;
    }

    LOG_INFO("[MAIN] IPC socket created (fd=%d)", listen_fd);

    /* Initialize Wayland and create overlay */
    init_wayland();
    create_layer_surface();

    /* Run the main event loop (handles both Wayland events and IPC commands) */
    wayland_loop_with_ipc(listen_fd);

    /* Cleanup */
    switcher_ipc_cleanup(listen_fd);

    LOG_INFO("[MAIN] Main instance exiting");
    log_close();
    return 0;
}