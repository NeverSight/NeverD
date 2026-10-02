//===- WindowsModuleFixture.h - Independent freestanding ABI ----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_WINDOWS_MODULE_FIXTURE_H
#define NEVERD_WINDOWS_MODULE_FIXTURE_H
typedef unsigned int DWORD;
typedef unsigned long long ULONG_PTR;
typedef unsigned short WCHAR;
#define NEVERD_MODULE_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_MODULE_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_MODULE_WIDE(Name, Text) static const WCHAR Name[] = Text;
#include "WindowsModuleCases.def"
#undef NEVERD_MODULE_WIDE
#undef NEVERD_MODULE_TEXT
#undef NEVERD_MODULE_VALUE
__declspec(dllimport) DWORD GetCurrentProcessId(void);
__declspec(dllimport) DWORD GetLastError(void);
__declspec(dllimport) void SetLastError(DWORD);
#endif
