//===- KernelAPIIRQL.cpp - Kernel routine IRQL contracts -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact-name lookup of the supported kernel routine caller-IRQL ceilings.
///
//===----------------------------------------------------------------------===//

#include "KernelAPIIRQL.h"

#include "KernelDispatcher.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {

enum class Limit : uint8_t {
#define NEVERD_KERNEL_IRQL_VALUE(Name, Value) Name = Value,
#include "KernelAPIIRQL.def"
#undef NEVERD_KERNEL_IRQL_VALUE
};

struct RoutineLimit {
  llvm::StringLiteral Name;
  Limit Maximum;
};

constexpr RoutineLimit RoutineLimits[] = {
#define NEVERD_KERNEL_IRQL_API(Name, Maximum) {#Name, Limit::Maximum},
#include "KernelAPIIRQL.def"
#undef NEVERD_KERNEL_IRQL_API
};

} // namespace

llvm::Expected<uint8_t> maximumKernelIRQL(llvm::StringRef Name) {
  for (const auto &Routine : RoutineLimits)
    if (Name == Routine.Name)
      return static_cast<uint8_t>(Routine.Maximum);
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "no kernel IRQL contract for " + Name);
}

} // namespace neverd::emulation
