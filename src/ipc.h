#pragma once
#include <stddef.h>
#include <stdbool.h>

void hypr_ipc_connect();






void hypr_ipc_free_titles(char **titles, size_t count);

/* Focused client API (focusHistoryID == 0) */
typedef struct HyprClientInfo {
    char *address;
    char *title;
    char *app_class;
    int  workspace_id;
    int  pid;
    bool focused;
    int  focusHistoryID; /* 0 means currently focused; -1 or >0 otherwise */
} HyprClientInfo;





/* Free a single HyprClientInfo's heap allocations. */
void hypr_ipc_free_client_info(HyprClientInfo *info);

/* Basic clients enumeration: returns all current clients (across all workspaces),
   including focusHistoryID so the caller can detect the focused one (focusHistoryID == 0).
   Returns 0 on success and sets list_out/count_out; caller must free with hypr_ipc_free_client_infos. */
int hypr_ipc_get_clients_basic(HyprClientInfo **list_out, size_t *count_out);

/* Free an array of HyprClientInfo structs allocated by hypr_ipc_get_clients_basic. */
void hypr_ipc_free_client_infos(HyprClientInfo *infos, size_t count);

/* Sort clients by focus history (most recently focused first).
   focusHistoryID 0 = currently focused, higher values = older focus.
   Windows with focusHistoryID -1 (unknown) are placed at the end. */
void hypr_ipc_sort_clients_by_focus(HyprClientInfo *clients, size_t count);

/* Focus a client by multi-strategy:
   1) Try focusing by address (with "address:" prefix, then raw).
   2) Try focusing by class (escaped).
   3) Try focusing by title (escaped).
   Returns 0 on success, -1 on failure. */
int hypr_ipc_focus_client(const HyprClientInfo *client);

/* Focus a client by its address only (with "address:" prefix, then raw).
   Returns 0 on success, -1 on failure. */
int hypr_ipc_focus_address(const char *address);

/* ================= Internal IPC command sending/receiving ================= */
/* Send a command and capture the response into a heap-allocated string.
   On success returns 0 and sets *response_out (caller must free).
   On error returns -1 and *response_out is NULL. */
int hypr_ipc_send_recv(const char *command, char **response_out);

/* Send a command and capture the response into a fixed-size buffer.
   On success returns 0 and fills resp (up to resp_len).
   On error returns -1. */
int hypr_ipc_send_command_capture(const char *cmd, char *resp, size_t resp_len);
