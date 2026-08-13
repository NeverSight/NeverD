//===- NeverDCAPITypes.h - C API shared scalar types --------------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Handle, address, and output-language types shared by every NeverD C API
/// domain header.  Included by all of them; not intended to be used directly.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_TYPES_H
#define NEVERD_SDK_CAPI_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef NEVERD_EXPORTS
#define NEVERD_API __declspec(dllexport)
#else
#define NEVERD_API __declspec(dllimport)
#endif
#else
#define NEVERD_API __attribute__((visibility("default")))
#endif

typedef void *neverd_session_t;
typedef unsigned long long neverd_va_t;
/// A Solana slot. Distinct from an address because it orders time rather than
/// memory: it is what decides which runtime gates had been activated yet.
typedef unsigned long long neverd_slot_t;

typedef enum neverd_output_language {
#define NEVERD_OUTPUT_LANGUAGE(NAME, VALUE, SPELLING, DISPLAY_NAME)            \
  NEVERD_OUTPUT_##NAME = (VALUE),
#include "neverd/OutputLanguages.def"
} neverd_output_language_t;

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_TYPES_H
