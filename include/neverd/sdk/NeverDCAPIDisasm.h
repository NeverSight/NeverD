//===- NeverDCAPIDisasm.h - C API disassembly and decompilation ---*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-function views of a loaded binary: the recovered function table, raw
/// bytes, disassembly, decompiled C, and each intermediate representation.
///
/// All returned strings are heap-allocated via strdup(); callers must
/// free them with neverd_free_string().
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_DISASM_H
#define NEVERD_SDK_CAPI_DISASM_H

#include "neverd/sdk/NeverDCAPITypes.h"

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

// ===--------------------------------------------------------------------===//
// Function list
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_func_count(neverd_session_t Sess);
NEVERD_API neverd_va_t neverd_func_entry(neverd_session_t Sess, int Idx);
NEVERD_API int neverd_func_size(neverd_session_t Sess, int Idx);
NEVERD_API const char *neverd_func_name(neverd_session_t Sess, int Idx);

// ===--------------------------------------------------------------------===//
// Function lookup helpers
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_func_find_by_name(neverd_session_t Sess,
                                        const char *Name);
NEVERD_API int neverd_func_find_by_addr(neverd_session_t Sess,
                                        neverd_va_t Addr);

// ===--------------------------------------------------------------------===//
// Raw bytes
// ===--------------------------------------------------------------------===//

NEVERD_API int neverd_read_bytes(neverd_session_t Sess, neverd_va_t Addr,
                                 unsigned char *Buf, int Size);

// ===--------------------------------------------------------------------===//
// Disassembly (returns JSON array)
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_disasm_json(neverd_session_t Sess,
                                          neverd_va_t Addr, int MaxInsns);

// ===--------------------------------------------------------------------===//
// Decompilation
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_decompile(neverd_session_t Sess,
                                        neverd_va_t FuncEntry);

// ===--------------------------------------------------------------------===//
// Multi-stage IR
// ===--------------------------------------------------------------------===//

NEVERD_API const char *neverd_ir_low(neverd_session_t Sess,
                                     neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_med(neverd_session_t Sess,
                                     neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_high(neverd_session_t Sess,
                                      neverd_va_t FuncEntry);
NEVERD_API const char *neverd_ir_llvm(neverd_session_t Sess,
                                      neverd_va_t FuncEntry);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_DISASM_H
