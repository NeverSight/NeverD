//===- ReturnAddressSigning.h - Unified unwind signing query -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Loader-owned distinction between exact-PC DWARF state and a compact
/// prologue recipe. Neither result authenticates or reconstructs a pointer.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RETURNADDRESSSIGNING_H
#define NEVERD_LOADER_RETURNADDRESSSIGNING_H

#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/Limits.h"

#include <cstddef>
#include <optional>

namespace neverd {

struct BinaryImage;
struct ExceptionFunction;

struct AArch64UnwindSigningQuery {
  va_t QueryPC = 0;
  /// An absent optional means that this source metadata does not exist. A
  /// present Unknown result never contains a fabricated unsigned state.
  std::optional<dwarf_eh::AArch64RAQueryResult> DwarfStateAtPC;
  /// A compact row describes the completed prologue. Even a Known recipe
  /// does not prove that the return address is already signed at QueryPC.
  std::optional<dwarf_eh::AArch64RAQueryResult> CompactRecipe;
};

struct AArch64UnwindSigningLimits {
  dwarf_eh::AArch64RAQueryLimits Dwarf;
  size_t MaxCIEs = limits::loader::kReturnAddressQueryMaxCIEs;
};

/// Query the already selected frame at the exact PC, without return-address
/// subtraction. Each source retains its own status; compact metadata cannot
/// replace an unavailable DWARF result. A compact recipe uses its own range
/// begin, including when the function also owns other code. Existing parser
/// and evaluator authorities own all encoding and CFI semantics.
/// Incomplete image/owner decode provenance rejects both sources before this
/// independent evaluation, even when a surviving local CIE/FDE is complete.
AArch64UnwindSigningQuery
queryAArch64UnwindSigning(const BinaryImage &Image,
                          const ExceptionFunction &Function, va_t PC,
                          const AArch64UnwindSigningLimits &Limits = {});

} // namespace neverd

#endif // NEVERD_LOADER_RETURNADDRESSSIGNING_H
