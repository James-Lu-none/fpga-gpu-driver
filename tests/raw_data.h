#ifndef FPGAGPU_TEST_RAW_DATA_H
#define FPGAGPU_TEST_RAW_DATA_H

#include <stddef.h>
#include <stdint.h>

// Write count signed int32 elements as headerless little-endian bytes.
// Returns zero on success, nonzero on file open/write/close failure.
int write_raw(const char *path, const int32_t *values, size_t count);

#endif
