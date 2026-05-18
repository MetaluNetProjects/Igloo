// version file, will be touched each build for date and time to be updated.
#include "fraise.h"

#include "build/git_describe.h"

void print_version() {
    fraise_printf("V igloo %s @ %s", gitdesc, __DATE__ " " __TIME__ "\n");
}

