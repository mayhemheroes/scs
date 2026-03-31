#include <stdint.h>
#include <stdio.h>
#include <climits>

#include <fuzzer/FuzzedDataProvider.h>>
#include "linalg.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzedDataProvider provider(data, size);
    scs_float f = provider.ConsumeFloatingPoint<scs_float>();
    _scs_norm_sq(&f, 1);

    return 0;
}