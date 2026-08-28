//===- CallEffects.cpp - Exact external-call effect summaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CallEffects.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SinkCatalog.h"
#include "neverd/safety/SinkScanner.h"

#include <string>
#include <type_traits>

using namespace neverd;
using namespace neverd::safety;

namespace {

constexpr CallEffectDescriptor Descriptors[] = {
#define SAFETY_CALL_EFFECT(NAME, FAMILY, CAPABILITIES, FORMATS, ABIS, MIN,     \
                           MAX)                                                \
  {NAME, CallEffectFamily::FAMILY, CAPABILITIES, FORMATS, ABIS, MIN, MAX},
#include "neverd/safety/SafetyCallEffects.def"
};

template <typename Enum> bool contains(Enum Set, Enum Value) {
  using Raw = std::underlying_type_t<Enum>;
  return (static_cast<Raw>(Set) & static_cast<Raw>(Value)) ==
         static_cast<Raw>(Value);
}

CallEffectFormat callFormat(BinaryFormat Format) {
  switch (Format) {
  case BinaryFormat::ELF:
    return CallEffectFormat::ELF;
  case BinaryFormat::COFF:
    return CallEffectFormat::COFF;
  case BinaryFormat::MachO:
    return CallEffectFormat::MachO;
  default:
    return CallEffectFormat::Unconstrained;
  }
}

CallEffectABI callABI(BinaryFormat Format, Arch Architecture) {
  switch (Format) {
  case BinaryFormat::COFF:
    switch (Architecture) {
    case Arch::X64:
    case Arch::X86:
    case Arch::AArch64:
    case Arch::ARM:
      return CallEffectABI::Microsoft;
    default:
      return CallEffectABI::Unconstrained;
    }
  case BinaryFormat::MachO:
    switch (Architecture) {
    case Arch::X64:
    case Arch::X86:
    case Arch::AArch64:
    case Arch::ARM:
      return CallEffectABI::Darwin;
    default:
      return CallEffectABI::Unconstrained;
    }
  case BinaryFormat::ELF:
    switch (Architecture) {
    case Arch::ARM:
    case Arch::AArch64:
      return CallEffectABI::AAPCS;
    case Arch::X64:
    case Arch::X86:
      return CallEffectABI::SysV;
    default:
      return CallEffectABI::Unconstrained;
    }
  default:
    return CallEffectABI::Unconstrained;
  }
}

CallEffectFormat
configuredFormats(ConfiguredCallEffect::Format ConfiguredFormats) {
  if (ConfiguredFormats == ConfiguredCallEffect::Format::Unconstrained)
    return CallEffectFormat::Unconstrained;
  CallEffectFormat Formats = CallEffectFormat::Unconstrained;
  if (contains(ConfiguredFormats, ConfiguredCallEffect::Format::ELF))
    Formats = Formats | CallEffectFormat::ELF;
  if (contains(ConfiguredFormats, ConfiguredCallEffect::Format::COFF))
    Formats = Formats | CallEffectFormat::COFF;
  if (contains(ConfiguredFormats, ConfiguredCallEffect::Format::MachO))
    Formats = Formats | CallEffectFormat::MachO;
  return Formats;
}

CallEffectABI configuredABIs(ConfiguredCallEffect::ABI ConfiguredABIs) {
  if (ConfiguredABIs == ConfiguredCallEffect::ABI::Unconstrained)
    return CallEffectABI::Unconstrained;
  CallEffectABI ABIs = CallEffectABI::Unconstrained;
  if (contains(ConfiguredABIs, ConfiguredCallEffect::ABI::SysV))
    ABIs = ABIs | CallEffectABI::SysV;
  if (contains(ConfiguredABIs, ConfiguredCallEffect::ABI::Microsoft))
    ABIs = ABIs | CallEffectABI::Microsoft;
  if (contains(ConfiguredABIs, ConfiguredCallEffect::ABI::Darwin))
    ABIs = ABIs | CallEffectABI::Darwin;
  if (contains(ConfiguredABIs, ConfiguredCallEffect::ABI::AAPCS))
    ABIs = ABIs | CallEffectABI::AAPCS;
  return ABIs;
}

bool isExternalOccurrence(const AnalysisInput &In, const MedCallInfo &Call) {
  const NameSource Source =
      classifyNameSource(In, Call.TargetAddr, Call.TargetName, Call.IsIndirect);
  if (Source == NameSource::Import)
    return true;

  // An indirect spelling that was not tied to an import occurrence is not an
  // exact external identity.  In particular, a recovered local function
  // pointer named like libc must not inherit libc's executable contract.
  if (Call.IsIndirect)
    return false;

  const va_t CanonicalTarget =
      In.Img ? normalizeCodeAddress(Call.TargetAddr, In.Img->Arch, In.Img->Mode)
             : Call.TargetAddr;
  const MedFunc *Callee = In.findMedFunc(CanonicalTarget);
  if (!Callee)
    return true;
  if (Call.TargetAddr != 0)
    return false;

  // Address zero is also an unresolved-relocation sentinel.  A real lifted
  // entry there is the target only when the resolved occurrence name agrees;
  // otherwise a named external relocation must not be mistaken for the caller.
  const std::string ResolvedName = resolveCallName(In, Call);
  if (ResolvedName.empty() || isSynthesizedFuncName(ResolvedName))
    return true;
  const std::string TargetName = SinkCatalog::normalize(ResolvedName);
  const auto matchesTarget = [&](llvm::StringRef Candidate) {
    return !Candidate.empty() &&
           SinkCatalog::normalize(Candidate) == TargetName;
  };
  return !matchesTarget(Callee->Name) && !matchesTarget(Callee->DebugName);
}

} // namespace

bool CallEffectDescriptor::appliesTo(BinaryFormat Format, Arch Architecture,
                                     unsigned RecoveredArity) const {
  if (RecoveredArity < MinArity || RecoveredArity > MaxArity)
    return false;
  if (Formats != CallEffectFormat::Unconstrained) {
    const CallEffectFormat Actual = callFormat(Format);
    if (Actual == CallEffectFormat::Unconstrained || !contains(Formats, Actual))
      return false;
  }
  if (ABIs != CallEffectABI::Unconstrained) {
    const CallEffectABI Actual = callABI(Format, Architecture);
    if (Actual == CallEffectABI::Unconstrained || !contains(ABIs, Actual))
      return false;
  }
  return true;
}

bool CallEffects::supports(const SinkEntry &Entry) const {
  switch (Entry.Kind) {
  case SinkKind::Copy:
    return has(CallEffectCapability::ModRef) &&
           (family() == CallEffectFamily::Copy ||
            (Entry.UnboundedWrite && family() == CallEffectFamily::Input &&
             has(CallEffectCapability::Taint)));
  case SinkKind::Format:
    return family() == CallEffectFamily::Format &&
           has(CallEffectCapability::ModRef);
  case SinkKind::Alloc:
    return family() == CallEffectFamily::Allocation &&
           has(CallEffectCapability::Allocation);
  case SinkKind::StackAlloc:
    return family() == CallEffectFamily::StackAllocation &&
           has(CallEffectCapability::Allocation);
  case SinkKind::Realloc:
    return family() == CallEffectFamily::Reallocation &&
           has(CallEffectCapability::Allocation) &&
           has(CallEffectCapability::Release);
  case SinkKind::Free:
    return family() == CallEffectFamily::Release &&
           has(CallEffectCapability::Release);
  case SinkKind::Source:
    return (family() == CallEffectFamily::Input ||
            family() == CallEffectFamily::FormattedInput) &&
           has(CallEffectCapability::Taint);
  case SinkKind::Exec:
    return false;
  }
  return false;
}

const CallEffectDescriptor *
neverd::safety::lookupCallEffectDescriptor(llvm::StringRef Name) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  for (const CallEffectDescriptor &Descriptor : Descriptors)
    if (Normalized == Descriptor.Name)
      return &Descriptor;
  return nullptr;
}

CallEffects neverd::safety::resolveCallEffects(llvm::StringRef Name,
                                               BinaryFormat Format,
                                               Arch Architecture,
                                               unsigned RecoveredArity) {
  const CallEffectDescriptor *Descriptor = lookupCallEffectDescriptor(Name);
  if (!Descriptor ||
      !Descriptor->appliesTo(Format, Architecture, RecoveredArity))
    return {};
  return CallEffects(Descriptor);
}

CallEffects neverd::safety::resolveCallEffects(const SinkCatalog &Catalog,
                                               llvm::StringRef Name,
                                               BinaryFormat Format,
                                               Arch Architecture,
                                               unsigned RecoveredArity) {
  const ConfiguredCallEffect *Configured =
      Catalog.matchConfiguredCallEffect(Name);
  if (!Configured)
    return resolveCallEffects(Name, Format, Architecture, RecoveredArity);

  CallEffectFamily Family = CallEffectFamily::None;
  switch (Configured->TheFamily) {
  case ConfiguredCallEffect::Family::Copy:
    Family = CallEffectFamily::Copy;
    break;
  case ConfiguredCallEffect::Family::Format:
    Family = CallEffectFamily::Format;
    break;
  case ConfiguredCallEffect::Family::None:
    return {};
  }

  CallEffectDescriptor Descriptor;
  Descriptor.Family = Family;
  Descriptor.Capabilities = CallEffectCapability::ModRef;
  Descriptor.Formats = configuredFormats(Configured->Formats);
  Descriptor.ABIs = configuredABIs(Configured->ABIs);
  Descriptor.MinArity = Configured->MinArity;
  Descriptor.MaxArity = Configured->MaxArity;
  if (!Descriptor.appliesTo(Format, Architecture, RecoveredArity))
    return {};
  return {Descriptor.Family, Descriptor.Capabilities};
}

CallEffects neverd::safety::resolveCallEffects(const AnalysisInput &In,
                                               const MedCallInfo &Call) {
  if (!isExternalOccurrence(In, Call) ||
      Call.Args.size() > CallEffectDescriptor::VariadicArity)
    return {};
  const BinaryFormat Format = In.Img ? In.Img->Format : BinaryFormat::Unknown;
  const Arch Architecture = In.Img ? In.Img->Arch : Arch::Unknown;
  return resolveCallEffects(resolveCallName(In, Call), Format, Architecture,
                            static_cast<unsigned>(Call.Args.size()));
}

CallEffects neverd::safety::resolveCallEffects(const AnalysisInput &In,
                                               const SinkCatalog &Catalog,
                                               const MedCallInfo &Call) {
  if (!isExternalOccurrence(In, Call) ||
      Call.Args.size() > CallEffectDescriptor::VariadicArity)
    return {};
  const BinaryFormat Format = In.Img ? In.Img->Format : BinaryFormat::Unknown;
  const Arch Architecture = In.Img ? In.Img->Arch : Arch::Unknown;
  return resolveCallEffects(Catalog, resolveCallName(In, Call), Format,
                            Architecture,
                            static_cast<unsigned>(Call.Args.size()));
}
