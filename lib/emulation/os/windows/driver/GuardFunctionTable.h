//===- GuardFunctionTable.h - Shared GFIDS record validation -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared GFIDS record validation.
///

#ifndef NEVERD_EMULATION_GUARDFUNCTIONTABLE_H
#define NEVERD_EMULATION_GUARDFUNCTIONTABLE_H
#include "GuardControlFlow.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Endian.h"

#include <vector>

namespace neverd::emulation::guard {
struct FunctionTarget {
  uint32_t RVA;
  uint8_t Flags;
};
/// Storage ownership and image policy are validated by the caller. Both PE
/// consumers use this one authority for packed record order, metadata and code.
inline llvm::Expected<std::vector<FunctionTarget>>
decodeFunctionTable(llvm::ArrayRef<uint8_t> Bytes, unsigned ExtraBytes,
                    uint8_t AllowedMetadata,
                    llvm::function_ref<bool(uint32_t)> Executable) {
  auto Error = [](const char *Text) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
  };
  const unsigned Stride = 4 + ExtraBytes;
  if (ExtraBytes > 1 || Bytes.size() % Stride)
    return Error("unsupported CFG function-table metadata stride");
  if (Bytes.size() / Stride > MaximumTargets)
    return Error("CFG target count exceeds execution profile");
  std::vector<FunctionTarget> Targets;
  uint32_t Previous = 0;
  for (size_t Offset = 0; Offset < Bytes.size(); Offset += Stride) {
    const auto RVA = llvm::support::endian::read32le(Bytes.data() + Offset);
    const uint8_t Flags = ExtraBytes ? Bytes[Offset + 4] : 0;
    if (!RVA || (!Targets.empty() && RVA <= Previous))
      return Error("CFG targets must be unique and sorted");
    if (!Executable(RVA))
      return Error("CFG function target is not file-backed executable code");
    if (Flags & ~AllowedMetadata)
      return Error("unsupported CFG function target metadata");
    if ((Flags & ExportSuppressed) && (RVA & 15))
      return Error("export-suppressed CFG target must be 16-byte aligned");
    Targets.push_back({RVA, Flags});
    Previous = RVA;
  }
  return Targets;
}
} // namespace neverd::emulation::guard
#endif
