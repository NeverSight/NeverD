//===- SEHLocalUnwindProtocolDetail.h - Unwind conventions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Each architecture's local unwind convention, which
/// sehLocalUnwindConvention dispatches to.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOLDETAIL_H
#define NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOLDETAIL_H

#include "neverd/ir/low/SEHLocalUnwindProtocol.h"
#include "neverd/loader/BinaryImage.h"

#include <optional>
#include <set>

namespace neverd::seh_local_unwind {

/// What the decoded prologue of a function allocates by its own instructions,
/// and how many calls it makes: a GS prologue on ARM and ARM64 allocates the
/// cookie's slot in the helper it calls, which the unwind codes count too.
struct DecodedPrologue {
  uint64_t Allocation = 0;
  /// With what the first block goes on to allocate: an ARM64 function that sets
  /// its frame pointer allocates the body's locals past the unwind codes.
  uint64_t BodyAllocation = 0;
  /// The calls before the last of those allocations.
  unsigned Calls = 0;
};

/// \p Function's decoded prologue, or nullopt when it writes the stack pointer
/// any way but by constants.
std::optional<DecodedPrologue> decodedPrologue(const LowFunc &Function,
                                               const TargetRegInfo &TRI,
                                               size_t &WorkRemaining);

/// The constant targets \p Function gives \p Routine.
std::set<va_t> surveyUnwindTargets(const LowFunc &Function,
                                   const BinaryImage &Image,
                                   const TargetRegInfo &TRI,
                                   llvm::StringRef Routine);

/// Windows x64 (X86/SEHLocalUnwindX64.cpp).
llvm::Expected<SEHLocalUnwindConvention> x64Convention(const LowFunc &Function,
                                                       const TargetRegInfo &TRI,
                                                       size_t &WorkRemaining);

/// Windows ARM64 (AArch64/SEHLocalUnwindAArch64.cpp).
llvm::Expected<SEHLocalUnwindConvention>
aarch64Convention(const LowFunc &Function, const TargetRegInfo &TRI,
                  size_t &WorkRemaining);
std::set<va_t> aarch64UnwindTargets(const LowFunc &Function,
                                    const BinaryImage &Image,
                                    const TargetRegInfo &TRI);

/// Windows ARM (ARM/SEHLocalUnwindARM.cpp).
llvm::Expected<SEHLocalUnwindConvention> armConvention(const LowFunc &Function,
                                                       const TargetRegInfo &TRI,
                                                       size_t &WorkRemaining);
std::set<va_t> armUnwindTargets(const LowFunc &Function,
                                const BinaryImage &Image,
                                const TargetRegInfo &TRI);

/// What a Thumb function's literal pool holds at \p Address, for a load of
/// \p Size bytes, or nullopt, as on any other architecture
/// (ARM/SEHLocalUnwindARM.cpp).
std::optional<uint64_t> armLiteralPoolValue(const BinaryImage &Image,
                                            uint64_t Address, uint16_t Size);

/// The import an ARM64 import veneer at \p Address branches through:
/// `adrp xN, page; ldr xN, [xN, #slot]; br xN` with the slot an import's IAT
/// entry, or nullptr, as on any other architecture
/// (AArch64/SEHLocalUnwindAArch64.cpp).
const Import *aarch64VeneerImport(const BinaryImage &Image, va_t Address);

} // namespace neverd::seh_local_unwind

#endif // NEVERD_IR_LOW_SEHLOCALUNWINDPROTOCOLDETAIL_H
