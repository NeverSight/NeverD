//===- GoCode.h - Code the Go toolchain built ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Whether the Go toolchain compiled or assembled the code at an address.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_GO_GOCODE_H
#define NEVERD_LOADER_GO_GOCODE_H

#include "neverd/Common.h"
#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/LanguageEHGo.h"

#include <algorithm>
#include <iterator>

namespace neverd {

/// Whether \p Module's Go code (GoModuleInfo::GoCode) holds \p Address: Go
/// compiled or assembled it.  The Go call contract (isGoCode) and the AArch64
/// decoder's reading of Go's legacy UNDEF word both ask this.
inline bool holdsGoCode(const GoModuleInfo &Module, va_t Address) {
  const auto After = std::upper_bound(
      Module.GoCode.begin(), Module.GoCode.end(), Address,
      [](va_t A, const ExceptionAddressRange &R) { return A < R.Begin; });
  return After != Module.GoCode.begin() && std::prev(After)->contains(Address);
}

} // namespace neverd

#endif // NEVERD_LOADER_GO_GOCODE_H
