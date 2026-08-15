//===- StagedCSDKFlatHeader.c - Historical staged C SDK test -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include <NeverDCAPI.h>

#ifdef __cplusplus
#error "the staged SDK contract must be compiled as C"
#endif

_Static_assert(NEVERD_OUTPUT_RUST == 2, "output-language ABI drift");
_Static_assert(NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE == 14,
               "translation error ABI drift");

int neverd_staged_c_sdk_flat_header_test(void) { return 0; }
