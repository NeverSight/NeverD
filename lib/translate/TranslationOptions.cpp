//===- TranslationOptions.cpp - Cross-architecture policy validation -----===//

#include "neverd/translate/TranslationOptions.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"
#include "llvm/TargetParser/Triple.h"

#include <set>

namespace neverd::translate {
namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isKnown(TranslationMode Value) {
  return Value == TranslationMode::JIT || Value == TranslationMode::AOT;
}

bool isKnown(UnsupportedInstructionPolicy Value) {
  return Value == UnsupportedInstructionPolicy::Fail ||
         Value == UnsupportedInstructionPolicy::InterpreterFallback;
}

bool isKnown(TranslationOptimizationPolicy Value) {
  return Value == TranslationOptimizationPolicy::None ||
         Value == TranslationOptimizationPolicy::ProvenSemantic ||
         Value == TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
}

bool isKnown(LLVMOptimizationLevel Value) {
  return Value == LLVMOptimizationLevel::O0 ||
         Value == LLVMOptimizationLevel::O1 ||
         Value == LLVMOptimizationLevel::O2 ||
         Value == LLVMOptimizationLevel::O3;
}

bool isKnown(BlockCachePolicy Value) {
  return Value == BlockCachePolicy::Disabled ||
         Value == BlockCachePolicy::Enabled;
}

bool isKnown(CodeInvalidationPolicy Value) {
  return Value == CodeInvalidationPolicy::RejectExecutableWrites ||
         Value == CodeInvalidationPolicy::InvalidateOnExecutableWrite ||
         Value == CodeInvalidationPolicy::ValidateBeforeDispatch;
}

bool isKnown(DeterministicReplayPolicy Value) {
  return Value == DeterministicReplayPolicy::Disabled ||
         Value == DeterministicReplayPolicy::Record ||
         Value == DeterministicReplayPolicy::Replay;
}

bool isKnown(HostTargetKind Value) {
  return Value == HostTargetKind::Native || Value == HostTargetKind::Explicit;
}

std::optional<GuestArchitecture> architectureFromTriple(llvm::StringRef Text) {
  switch (llvm::Triple(Text).getArch()) {
  case llvm::Triple::x86_64:
    return GuestArchitecture::X86_64;
  case llvm::Triple::x86:
    return GuestArchitecture::X86_32;
  case llvm::Triple::aarch64:
    return GuestArchitecture::AArch64;
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    return GuestArchitecture::ARM32;
  default:
    return std::nullopt;
  }
}

bool isCanonicalTargetAtom(llvm::StringRef Text) {
  if (Text.empty())
    return false;
  return llvm::all_of(Text, [](char Character) {
    const unsigned char Byte = static_cast<unsigned char>(Character);
    return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
           Character == '_' || Character == '.' || Character == '-';
  });
}

bool isCanonicalTargetFeature(llvm::StringRef Text) {
  return Text.size() > 1 && (Text.front() == '+' || Text.front() == '-') &&
         isCanonicalTargetAtom(Text.drop_front());
}

llvm::Error validateHostTarget(const TranslationOptions &Options) {
  if (!isKnown(Options.Target.Kind))
    return invalid("unknown host-target kind");
  if (Options.Mode == TranslationMode::JIT) {
    if (Options.Target.Kind != HostTargetKind::Native)
      return invalid("JIT translation only accepts the native process target");
    if (Options.Target.Architecture || !Options.Target.Triple.empty() ||
        !Options.Target.CPU.empty() || !Options.Target.Features.empty())
      return invalid("native JIT target cannot carry an explicit target");
    return llvm::Error::success();
  }

  if (Options.Target.Kind != HostTargetKind::Explicit)
    return invalid("AOT translation requires an explicit host target");
  if (!Options.Target.Architecture)
    return invalid("explicit AOT target is missing its architecture");
  if (!getArchitectureDescription(*Options.Target.Architecture))
    return invalid("explicit AOT target has an unknown architecture");
  if (Options.Target.Triple.empty())
    return invalid("explicit AOT target is missing its triple");
  const std::optional<GuestArchitecture> TripleArchitecture =
      architectureFromTriple(Options.Target.Triple);
  if (!TripleArchitecture)
    return invalid("explicit AOT triple has an unsupported architecture");
  if (*TripleArchitecture != *Options.Target.Architecture)
    return invalid("explicit AOT triple does not match its architecture");
  if (!Options.Target.CPU.empty() && !isCanonicalTargetAtom(Options.Target.CPU))
    return invalid("explicit AOT CPU is not canonical lower-case ASCII");
  std::set<std::string> FeatureNames;
  std::optional<std::string> PreviousFeature;
  for (const std::string &Feature : Options.Target.Features) {
    if (!isCanonicalTargetFeature(Feature))
      return invalid("explicit AOT feature is not a signed target feature");
    if (PreviousFeature && Feature <= *PreviousFeature)
      return invalid("explicit AOT features are not in canonical order");
    PreviousFeature = Feature;
    if (!FeatureNames.insert(Feature.substr(1)).second)
      return invalid("explicit AOT target contains duplicate or conflicting "
                     "features");
  }
  return llvm::Error::success();
}

} // namespace

TranslationPairSupport getTranslationPairSupport(GuestArchitecture Guest,
                                                 GuestArchitecture Host) {
  if ((Guest == GuestArchitecture::X86_64 &&
       Host == GuestArchitecture::AArch64) ||
      (Guest == GuestArchitecture::AArch64 &&
       Host == GuestArchitecture::X86_64) ||
      (Guest == GuestArchitecture::X86_32 &&
       (Host == GuestArchitecture::AArch64 ||
        Host == GuestArchitecture::ARM32)) ||
      (Guest == GuestArchitecture::ARM32 &&
       (Host == GuestArchitecture::X86_32 ||
        Host == GuestArchitecture::X86_64)))
    return TranslationPairSupport::ContractDefined;
  return TranslationPairSupport::Unsupported;
}

TranslationCapabilityStatus
getInitialTranslationCapability(TranslationCapability Capability) {
  return Capability == TranslationCapability::ScalarInteger
             ? TranslationCapabilityStatus::ContractDefined
             : TranslationCapabilityStatus::Unsupported;
}

llvm::Error validateTranslationOptions(const TranslationOptions &Options) {
  if (!getArchitectureDescription(Options.Guest))
    return invalid("unknown guest architecture");
  if (!isKnown(Options.Mode))
    return invalid("unknown translation mode");
  if (!isKnown(Options.UnsupportedInstructions))
    return invalid("unknown unsupported-instruction policy");
  if (!isKnown(Options.Optimization))
    return invalid("unknown translation optimization policy");
  if (!isKnown(Options.LLVMLevel))
    return invalid("unknown LLVM optimization level");
  if (!isKnown(Options.BlockCache))
    return invalid("unknown block-cache policy");
  if (!isKnown(Options.CodeInvalidation))
    return invalid("unknown code-invalidation policy");
  if (!isKnown(Options.DeterministicReplay))
    return invalid("unknown deterministic-replay policy");
  if (llvm::Error Error = validateHostTarget(Options))
    return Error;
  if (Options.Mode == TranslationMode::AOT &&
      getTranslationPairSupport(Options.Guest, *Options.Target.Architecture) !=
          TranslationPairSupport::ContractDefined)
    return invalid("unsupported translation matrix cell");

  constexpr uint32_t KnownCapabilities =
      static_cast<uint32_t>(TranslationCapability::ScalarInteger) |
      static_cast<uint32_t>(TranslationCapability::FloatingPoint) |
      static_cast<uint32_t>(TranslationCapability::SIMD) |
      static_cast<uint32_t>(TranslationCapability::X87) |
      static_cast<uint32_t>(TranslationCapability::Atomics) |
      static_cast<uint32_t>(TranslationCapability::SystemInstructions);
  if ((static_cast<uint32_t>(Options.RequiredCapabilities) &
       ~KnownCapabilities) != 0)
    return invalid("translation request contains unknown capability bits");
  for (TranslationCapability Capability :
       {TranslationCapability::FloatingPoint, TranslationCapability::SIMD,
        TranslationCapability::X87, TranslationCapability::Atomics,
        TranslationCapability::SystemInstructions}) {
    if (hasCapability(Options.RequiredCapabilities, Capability))
      return invalid("requested translation capability is unsupported by the "
                     "v1 contract");
  }

  if (Options.Mode == TranslationMode::AOT &&
      Options.UnsupportedInstructions ==
          UnsupportedInstructionPolicy::InterpreterFallback)
    return invalid("AOT translation cannot request interpreter fallback");
  if (Options.Mode == TranslationMode::AOT &&
      Options.DeterministicReplay != DeterministicReplayPolicy::Disabled)
    return invalid("AOT translation cannot record or replay execution");
  if (Options.Mode == TranslationMode::AOT &&
      Options.BlockCache != BlockCachePolicy::Disabled)
    return invalid("AOT translation cannot enable the JIT block cache");
  if (Options.Mode == TranslationMode::AOT &&
      Options.CodeInvalidation !=
          CodeInvalidationPolicy::RejectExecutableWrites)
    return invalid("AOT translation requires executable writes to be rejected");
  if (Options.Optimization !=
          TranslationOptimizationPolicy::ProvenSemanticAndLLVM &&
      Options.LLVMLevel != LLVMOptimizationLevel::O0)
    return invalid("inactive LLVM optimization requires level O0");
  if (!Options.VerifyGeneratedIR)
    return invalid("generated LLVM IR verification cannot be disabled");
  if (!Options.PreserveExceptionState)
    return invalid("guest exception-state preservation cannot be disabled");
  return llvm::Error::success();
}

} // namespace neverd::translate
