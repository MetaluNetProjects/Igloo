// version file, will be touched each build for date and time to be updated.
#include "fraise.h"

void print_version() {
    fraise_printf("V igloo_fw 0.1 @ %s", __DATE__ " " __TIME__ "\n");
}

