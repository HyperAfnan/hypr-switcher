#include "wayland.h"
#include "ipc.h"
#include "logger/logger.h"
#include <stdio.h>

int main() {

    if (log_init("logger.log", LOG_DEBUG) != 0) {
          fprintf(stderr, "Failed to initialize logger\n");
          return 1;
      }
    hypr_ipc_connect();

    init_wayland();
    create_layer_surface();

    wayland_loop();

    return 0;
}
