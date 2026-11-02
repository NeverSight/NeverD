//===- AArch64ReturnAddressState.h - Static RA signing state ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded, static interpretation of the RA_SIGN_STATE CFI column. This
/// describes a signing recipe; it neither reads runtime values nor unwinds,
/// authenticates a pointer, or authorizes native metadata relocation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DWARF_AARCH64RETURNADDRESSSTATE_H
#define NEVERD_LOADER_DWARF_AARCH64RETURNADDRESSSTATE_H

#include "neverd/loader/LanguageEHDwarf.h"
#include "neverd/loader/Limits.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::dwarf_eh {
namespace aarch64 {
#define NEVERD_DWARF_A64_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define NEVERD_DWARF_A64_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/loader/DWARF/AArch64ReturnAddressState.def"
} // namespace aarch64

enum class AArch64RASigningKind : uint8_t { Unsigned, SP, SPAndPC };
enum class AArch64RAKey : uint8_t { A, B };
enum class AArch64RARecipe : uint8_t { HintPACM, NonHintPAuthLR };

struct AArch64ReturnAddressState {
  AArch64RASigningKind Kind = AArch64RASigningKind::Unsigned;
  AArch64RAKey Key = AArch64RAKey::A;
  /// Meaningful only for SPAndPC; hardware PACM enablement is not inferred.
  AArch64RARecipe Recipe = AArch64RARecipe::HintPACM;
  std::optional<va_t> SigningPC;
  bool operator==(const AArch64ReturnAddressState &) const = default;
};

enum class AArch64RAQueryStatus : uint8_t {
  Known,
  Unknown,
  Unsupported,
  Malformed,
  OutOfRange,
  ResourceLimit,
};

struct AArch64RAQueryResult {
  AArch64RAQueryStatus Status = AArch64RAQueryStatus::Unknown;
  /// Populated only for Known. Unknown never supplies an unsigned default.
  std::optional<AArch64ReturnAddressState> State;
  const char *Diagnostic = nullptr;
};

struct AArch64RAQueryLimits {
  size_t MaxInstructions = limits::loader::kAArch64RAStateMaxInstructions;
  size_t MaxRememberDepth = limits::loader::kAArch64RAStateMaxRememberDepth;
};

/// Query the exact code location (no return-PC subtraction). Every instruction
/// is validated, including the tail after PC, before a state is published.
/// CIE initial rules precede FDE rules and share the remembered-state stack.
/// A DW_CFA_restore of register 34 restores its CIE initial rule. Expressions
/// and runtime register/memory rules are explicitly Unknown. Reserved states,
/// opaque instructions and CIE location changes are Unsupported.
AArch64RAQueryResult
evaluateAArch64ReturnAddressState(Arch Architecture, const DwarfCIE &CIE,
                                  const DwarfFDE &FDE, va_t PC,
                                  const AArch64RAQueryLimits &Limits = {});

} // namespace neverd::dwarf_eh

#endif // NEVERD_LOADER_DWARF_AARCH64RETURNADDRESSSTATE_H
