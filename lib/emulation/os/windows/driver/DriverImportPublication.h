//===- DriverImportPublication.h - Bootstrap IAT publication ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bootstrap IAT publication.
///

#ifndef NEVERD_EMULATION_DRIVERIMPORTPUBLICATION_H
#define NEVERD_EMULATION_DRIVERIMPORTPUBLICATION_H
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <memory>
namespace neverd::emulation {
class DriverSessionState;
class ModulePublication;
struct ModuleInstance;
llvm::Expected<std::unique_ptr<ModulePublication>>
prepareDriverImportPublication(DriverSessionState &,
                               llvm::ArrayRef<ModuleInstance>);
} // namespace neverd::emulation
#endif
