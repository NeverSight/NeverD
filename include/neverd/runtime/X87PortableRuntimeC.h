//===- X87PortableRuntimeC.h - Generated x87 runtime calls --------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Plain C calls for an explicitly provisioned portable x87 execution context.
//===----------------------------------------------------------------------===//
/* Generated code uses an explicit, host-provisioned execution context. Guest
 * addresses remain uint64_t values. No context is acquired from the ambient
 * machine, and this interface does not grant permission to execute a binary.
 * Step returns zero only after a completed instruction. Result is valid once,
 * immediately after a successful scalar-producing Step; violating that checked
 * generated-code contract terminates rather than inventing a value. */

#ifndef NEVERD_RUNTIME_X87PORTABLERUNTIMEC_H
#define NEVERD_RUNTIME_X87PORTABLERUNTIMEC_H

#include <stdint.h>

/* Fixed numeric statuses; enum storage is never part of the C call ABI. */
enum nvd_x87_status_v1 {
#define NEVERD_X87_PORTABLE_STATUS(CxxName, CName, Value, Diagnostic)          \
  NVD_X87_STATUS_##CName = Value,
#include "neverd/ir/X86/X87PortableStatus.def"
#undef NEVERD_X87_PORTABLE_STATUS
};

#if defined(_WIN32) && defined(NEVERD_X87_RUNTIME_EXPORTS)
#define NEVERD_X87_RUNTIME_API __declspec(dllexport)
#elif defined(_WIN32)
#define NEVERD_X87_RUNTIME_API __declspec(dllimport)
#else
#define NEVERD_X87_RUNTIME_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#define NEVERD_X87_RUNTIME_NOEXCEPT noexcept
extern "C" {
#else
#define NEVERD_X87_RUNTIME_NOEXCEPT
#endif

#define NEVERD_X87_PORTABLE_HELPER(Role, Name, Result, Parameters)             \
  NEVERD_X87_RUNTIME_API Result Name Parameters NEVERD_X87_RUNTIME_NOEXCEPT;
#include "neverd/ir/X86/X87PortableABI.def"
#undef NEVERD_X87_PORTABLE_HELPER

#ifdef __cplusplus
}
#endif
#undef NEVERD_X87_RUNTIME_NOEXCEPT
#undef NEVERD_X87_RUNTIME_API

#endif /* NEVERD_RUNTIME_X87PORTABLERUNTIMEC_H */
