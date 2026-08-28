//===- CallEffects.h - Exact external-call effect summaries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_CALLEFFECTS_H
#define NEVERD_LIB_SAFETY_CALLEFFECTS_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>

namespace neverd {

struct MedCallInfo;

namespace safety {

struct AnalysisInput;
class SinkCatalog;
struct SinkEntry;

/// Independently consumable parts of an external-call summary. The zero value
/// is deliberate: discovery by a sink/source catalog does not establish any
/// executable semantics.
enum class CallEffectCapability : uint16_t {
  NoEffect = 0,
  Control = uint16_t{1} << 0,
  Return = uint16_t{1} << 1,
  ModRef = uint16_t{1} << 2,
  Taint = uint16_t{1} << 3,
  StringExtent = uint16_t{1} << 4,
  Allocation = uint16_t{1} << 5,
  Release = uint16_t{1} << 6,
  /// Argument-capture behavior is fully described. Current built-ins carrying
  /// this capability are non-retaining; absence must be treated as unknown.
  Capture = uint16_t{1} << 7,
  /// The call may consume the process's standard-input stream.
  MayConsumeStandardInput = uint16_t{1} << 8,
  /// Argument zero is a POSIX descriptor; only value zero denotes stdin.
  DescriptorZeroIsStandardInput = uint16_t{1} << 9,
  /// A literal environment key can drive an exact process-input replay.
  LiteralEnvironmentReplay = uint16_t{1} << 10,
  /// The first descriptor-zero call can drive exact stdin replay.
  ExactStandardInputReplay = uint16_t{1} << 11,
};

constexpr CallEffectCapability operator|(CallEffectCapability A,
                                         CallEffectCapability B) {
  return static_cast<CallEffectCapability>(static_cast<uint16_t>(A) |
                                           static_cast<uint16_t>(B));
}

constexpr CallEffectCapability operator&(CallEffectCapability A,
                                         CallEffectCapability B) {
  return static_cast<CallEffectCapability>(static_cast<uint16_t>(A) &
                                           static_cast<uint16_t>(B));
}

/// Semantic families are intentionally exact. Similar spelling alone never
/// promotes a call into a family.
enum class CallEffectFamily : uint8_t {
  None,
  StringLength,
  BoundedStringLength,
  Input,
  FormattedInput,
  Copy,
  Format,
  Allocation,
  Reallocation,
  Release,
  StackAllocation,
};

/// Object formats to which a descriptor is restricted. Unconstrained means
/// the described property is independent of the file format, including when a
/// unit-level query has no loaded image.
enum class CallEffectFormat : uint8_t {
  Unconstrained = 0,
  ELF = uint8_t{1} << 0,
  COFF = uint8_t{1} << 1,
  MachO = uint8_t{1} << 2,
};

constexpr CallEffectFormat operator|(CallEffectFormat A, CallEffectFormat B) {
  return static_cast<CallEffectFormat>(static_cast<uint8_t>(A) |
                                       static_cast<uint8_t>(B));
}

/// Calling-ABI restrictions for summaries whose layout is platform-specific.
enum class CallEffectABI : uint8_t {
  Unconstrained = 0,
  SysV = uint8_t{1} << 0,
  Microsoft = uint8_t{1} << 1,
  Darwin = uint8_t{1} << 2,
  AAPCS = uint8_t{1} << 3,
};

constexpr CallEffectABI operator|(CallEffectABI A, CallEffectABI B) {
  return static_cast<CallEffectABI>(static_cast<uint8_t>(A) |
                                    static_cast<uint8_t>(B));
}

struct CallEffectDescriptor {
  static constexpr unsigned VariadicArity =
      std::numeric_limits<unsigned>::max();

  const char *Name = "";
  CallEffectFamily Family = CallEffectFamily::None;
  CallEffectCapability Capabilities = CallEffectCapability::NoEffect;
  CallEffectFormat Formats = CallEffectFormat::Unconstrained;
  CallEffectABI ABIs = CallEffectABI::Unconstrained;
  unsigned MinArity = 0;
  unsigned MaxArity = 0;

  bool appliesTo(BinaryFormat Format, Arch Architecture,
                 unsigned RecoveredArity) const;
};

/// Result of an applicability-checked lookup. A failed or incomplete query is
/// represented as NoEffect, never as a partially trusted descriptor.
struct CallEffects {
  CallEffectFamily Family = CallEffectFamily::None;
  CallEffectCapability Capabilities = CallEffectCapability::NoEffect;
  bool Applicable = false;

  CallEffects() = default;
  explicit CallEffects(const CallEffectDescriptor *Descriptor)
      : Family(Descriptor ? Descriptor->Family : CallEffectFamily::None),
        Capabilities(Descriptor ? Descriptor->Capabilities
                                : CallEffectCapability::NoEffect),
        Applicable(Descriptor != nullptr) {}
  CallEffects(CallEffectFamily Family, CallEffectCapability Capabilities)
      : Family(Family), Capabilities(Capabilities), Applicable(true) {}

  explicit operator bool() const { return Applicable; }
  CallEffectCapability capabilities() const { return Capabilities; }
  CallEffectFamily family() const { return Family; }
  bool has(CallEffectCapability Capability) const {
    return (capabilities() & Capability) == Capability;
  }
  /// Whether these applicability-checked effects establish the catalog
  /// entry's complete executable family contract.
  bool supports(const SinkEntry &Entry) const;
};

/// Find a descriptor by normalized exact name without applying site facts.
/// Analyses should use resolveCallEffects instead.
const CallEffectDescriptor *lookupCallEffectDescriptor(llvm::StringRef Name);

/// Resolve an exact external-call summary. Arity, object-format, and ABI
/// mismatches fail closed to NoEffect.
CallEffects resolveCallEffects(llvm::StringRef Name, BinaryFormat Format,
                               Arch Architecture, unsigned RecoveredArity);
CallEffects resolveCallEffects(const SinkCatalog &Catalog, llvm::StringRef Name,
                               BinaryFormat Format, Arch Architecture,
                               unsigned RecoveredArity);
CallEffects resolveCallEffects(const AnalysisInput &In,
                               const MedCallInfo &Call);
CallEffects resolveCallEffects(const AnalysisInput &In,
                               const SinkCatalog &Catalog,
                               const MedCallInfo &Call);

} // namespace safety
} // namespace neverd

#endif // NEVERD_LIB_SAFETY_CALLEFFECTS_H
