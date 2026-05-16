/* partition info API */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int print_partitions();

bool get_partition_address(const char *name, intptr_t *start, int *length);

#ifdef __cplusplus
}
#endif

