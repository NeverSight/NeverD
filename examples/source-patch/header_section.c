//===- header_section.c - Fill the example PE section table
//----------------===//
//
// NeverD Decompiler
//
/// \file
/// Each build places one exported integer in a distinct read-only section.
//
//===----------------------------------------------------------------------===//

__declspec(dllexport) __attribute__((section(EXAMPLE_SECTION)))
const int EXAMPLE_SYMBOL = 1;
