//===- DriverExceptionPublication.h - Native unwind ownership -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native unwind ownership.
///

#ifndef NEVERD_EMULATION_DRIVEREXCEPTIONPUBLICATION_H
#define NEVERD_EMULATION_DRIVEREXCEPTIONPUBLICATION_H
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <memory>
namespace neverd::emulation {
class DriverSessionState;
class ModulePublication;
struct ModuleInstance;
llvm::Expected<std::unique_ptr<ModulePublication>>
prepareDriverExceptionPublication(DriverSessionState &,
                                  llvm::ArrayRef<ModuleInstance>,
                                  std::unique_ptr<ModulePublication>);
} // namespace neverd::emulation
#endif
