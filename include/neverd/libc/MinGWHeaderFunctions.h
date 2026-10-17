//===- MinGWHeaderFunctions.h - Functions mingw-w64 declares ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_MINGWHEADERFUNCTIONS_H
#define NEVERD_LIBC_MINGWHEADERFUNCTIONS_H

#include <string_view>

namespace neverd::libc {

/// Whether a mingw-w64 header the C writers can include for a Windows
/// program declares a function named \p Name (MinGWHeaderFunctions.inc): a
/// unit that includes it cannot define another function by that name.
bool mingwHeadersDeclare(std::string_view Name);

} // namespace neverd::libc

#endif // NEVERD_LIBC_MINGWHEADERFUNCTIONS_H
