#include "raw_data.h"
#include <stdio.h>

int write_raw(const char *path, const int32_t *values, size_t count) {
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return 1; }
    for (size_t i = 0; i < count; i++) {
        uint32_t value = (uint32_t)values[i];
        unsigned char bytes[4] = {value, value >> 8, value >> 16, value >> 24};
        if (fwrite(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
            fclose(file);
            return 1;
        }
    }
    return fclose(file) != 0;
}

