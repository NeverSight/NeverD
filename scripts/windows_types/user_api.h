//===- user_api.h - Documented Windows user-mode exports -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Windows user-mode declarations absent from the bundled SDK headers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SCRIPTS_WINDOWS_TYPES_USER_API_H
#define NEVERD_SCRIPTS_WINDOWS_TYPES_USER_API_H

// BCryptPrimitives.dll. Microsoft documents BOOL, a byte buffer and SIZE_T.
// https://learn.microsoft.com/windows/win32/seccng/processprng
BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData);

#endif // NEVERD_SCRIPTS_WINDOWS_TYPES_USER_API_H
