//===- extra.c - New dynamic dependency example ---------------------------===//
//
// NeverD Decompiler
//
/// \file
/// This library is first imported by the replacement, not the original.
//
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

EXPORT int extra_state = 7;
EXPORT int extra_function(int X) { return 3 * X; }
