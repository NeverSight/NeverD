//===- MachOLayoutGrowth.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reserve native metadata while preserving original code and data addresses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOLAYOUTGROWTH_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOLAYOUTGROWTH_H
#include "neverd/Common.h"
#include "neverd/backend/Limits.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <vector>
namespace neverd {
/// Finish the chained-fixup segment inventory before signing/publication.
llvm::Error completeMachOChainedSegmentTable(std::vector<uint8_t> &Binary,
                                             Arch Architecture);

/// Grow the header into a PIE guard or an available prefix of a nonzero-base
/// dylib or bundle. Original code, data and fixup slots retain their addresses.
/// Relocate supported loader metadata transactionally and reject instruction
/// references into the moving header.
llvm::Expected<bool> reserveMachOHeaderStorage(std::vector<uint8_t> &Binary,
                                               Arch Architecture,
                                               uint64_t AdditionalCommandBytes);

/// Declare new TEXT metadata while preserving all existing section ordinals.
llvm::Expected<uint64_t> declareMachOTextMetadata(std::vector<uint8_t> &Binary,
                                                  Arch Architecture,
                                                  llvm::StringRef Name,
                                                  uint64_t Size);

/// Declare storage in the unowned zero tail of the sole SG_READ_ONLY segment.
/// Preserve original addresses and adjust later symbol section ordinals.
llvm::Expected<uint64_t> declareMachOReadOnlyData(std::vector<uint8_t> &Binary,
                                                  Arch Architecture,
                                                  llvm::StringRef Name,
                                                  uint64_t Size);

/// Logical work across every old/new AArch64 frame-state comparison in one
/// metadata relocation. Budget exhaustion leaves the input image unchanged.
struct MachOUnwindRelocationLimits {
  /// Zero permits no AArch64 state comparisons.
  uint64_t MaxRAStateWork = limits::backend::kMachOFrameRAComparisonWork;
};

/// Reserve native unwind storage without moving the Mach header or original
/// code/data. Existing metadata is copied and its encoded references rebased.
/// Compact tables can reuse zero-filled TEXT capacity after all section and
/// load-command payload owners. If no existing capacity remains, a live segment
/// blocking TEXT extension requires a complete relocation contract and is
/// refused. Returns whether the private candidate changed.
llvm::Expected<bool>
reserveMachOSourceUnwind(std::vector<uint8_t> &Binary, Arch Architecture,
                         uint64_t AdditionalEHBytes,
                         uint64_t AdditionalFunctions,
                         const MachOUnwindRelocationLimits &Limits = {});

/// Reserve from a real emitted object before new RX/RW segments can occupy the
/// TEXT tail. Final installation still verifies the actual relocated records.
llvm::Expected<bool>
reserveMachONativePatchStorage(std::vector<uint8_t> &Binary, Arch Architecture,
                               llvm::StringRef Object, uint64_t CallbackCount,
                               uint64_t ImportFunctionCount);
} // namespace neverd
#endif
