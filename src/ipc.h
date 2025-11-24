#pragma once
#include <stddef.h>

void hypr_ipc_connect();
void hypr_ipc_print_clients();

/* Fetch a heap-allocated array of duplicated client titles.
   On success returns 0 and sets titles_out/count_out.
   Call hypr_ipc_free_titles to free. */
int hypr_ipc_get_client_titles(char ***titles_out, size_t *count_out);
void hypr_ipc_free_titles(char **titles, size_t count);
