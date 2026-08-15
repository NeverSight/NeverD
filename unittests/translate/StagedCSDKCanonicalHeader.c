//===- StagedCSDKCanonicalHeader.c - Canonical staged C SDK test ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include <neverd/sdk/NeverDCAPI.h>

#ifdef __cplusplus
#error "the staged SDK contract must be compiled as C"
#endif

_Static_assert(NEVERD_OUTPUT_C == 0, "output-language ABI drift");
_Static_assert(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF == 1,
               "translation object-format ABI drift");

int neverd_staged_c_sdk_flat_header_test(void);

int main(void) { return neverd_staged_c_sdk_flat_header_test(); }
