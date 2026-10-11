//===- WindowsNativeFailure.h - Native failure diagnostics -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_WINDOWS_NATIVE_FAILURE_H
#define NEVERD_UNITTESTS_WINDOWS_NATIVE_FAILURE_H

#if defined(_WIN32) && defined(_M_X64)
void diagnoseNativeFailure(const wchar_t *Path);
#endif

#endif // NEVERD_UNITTESTS_WINDOWS_NATIVE_FAILURE_H
