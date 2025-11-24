#pragma once
#include <stddef.h>
#include <stdbool.h>

void hypr_ipc_connect();
void hypr_ipc_print_clients();

/* Fetch a heap-allocated array of duplicated client titles.
   On success returns 0 and sets titles_out/count_out.
   Call hypr_ipc_free_titles to free. */
int hypr_ipc_get_client_titles(char ***titles_out, size_t *count_out);
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

/* Returns 0 on success, fills 'out' with focused client info (whose focusHistoryID == 0).
   Returns -1 if none found or on error. Caller must free with hypr_ipc_free_client_info. */
int hypr_ipc_get_focused_client(HyprClientInfo *out);

/* Free a single HyprClientInfo's heap allocations. */
void hypr_ipc_free_client_info(HyprClientInfo *info);

/* Basic clients enumeration: returns all current clients (across all workspaces),
   including focusHistoryID so the caller can detect the focused one (focusHistoryID == 0).
   Returns 0 on success and sets list_out/count_out; caller must free with hypr_ipc_free_client_infos. */
int hypr_ipc_get_clients_basic(HyprClientInfo **list_out, size_t *count_out);

/* Free an array of HyprClientInfo structs allocated by hypr_ipc_get_clients_basic. */
void hypr_ipc_free_client_infos(HyprClientInfo *infos, size_t count);
