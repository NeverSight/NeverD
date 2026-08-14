//===- TranslationOptions.h - Cross-architecture policy options -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONOPTIONS_H
#define NEVERD_TRANSLATE_TRANSLATIONOPTIONS_H

#include "neverd/translate/GuestState.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::translate {

enum class TranslationMode : uint8_t {
  JIT = 1,
  AOT = 2,
};

enum class UnsupportedInstructionPolicy : uint8_t {
  Fail = 1,
  InterpreterFallback = 2,
};

enum class TranslationOptimizationPolicy : uint8_t {
  None = 0,
  ProvenSemantic = 1,
  ProvenSemanticAndLLVM = 2,
};

enum class LLVMOptimizationLevel : uint8_t {
  O0 = 0,
  O1 = 1,
  O2 = 2,
  O3 = 3,
};

enum class BlockCachePolicy : uint8_t {
  Disabled = 0,
  Enabled = 1,
};

enum class CodeInvalidationPolicy : uint8_t {
  RejectExecutableWrites = 1,
  InvalidateOnExecutableWrite = 2,
  ValidateBeforeDispatch = 3,
};

enum class DeterministicReplayPolicy : uint8_t {
  Disabled = 0,
  Record = 1,
  Replay = 2,
};

/// Contract-level support is intentionally distinct from engine availability.
/// ContractDefined means requests can be validated and persisted; executable
/// engines publish and validate their own availability independently.
enum class TranslationPairSupport : uint8_t {
  Unsupported = 0,
  ContractDefined = 1,
};

enum class HostTargetKind : uint8_t {
  Native = 0,
  Explicit = 1,
};

/// Native is the only valid JIT target and is resolved from the running
/// process by an engine implementation.  Explicit triples are AOT-only;
/// keeping the two forms distinct prevents a guest architecture enum from
/// masquerading as proof that host-native execution is possible.
struct HostTarget {
  HostTargetKind Kind = HostTargetKind::Native;
  std::optional<GuestArchitecture> Architecture;
  std::string Triple;
  std::string CPU;
  std::vector<std::string> Features;
};

enum class TranslationCapability : uint32_t {
  None = 0,
  ScalarInteger = 1u << 0,
  FloatingPoint = 1u << 1,
  SIMD = 1u << 2,
  X87 = 1u << 3,
  Atomics = 1u << 4,
  SystemInstructions = 1u << 5,
};

constexpr TranslationCapability operator|(TranslationCapability Left,
                                          TranslationCapability Right) {
  return static_cast<TranslationCapability>(static_cast<uint32_t>(Left) |
                                            static_cast<uint32_t>(Right));
}

constexpr bool hasCapability(TranslationCapability Set,
                             TranslationCapability Value) {
  return (static_cast<uint32_t>(Set) & static_cast<uint32_t>(Value)) != 0;
}

enum class TranslationCapabilityStatus : uint8_t {
  Unsupported = 0,
  ContractDefined = 1,
};

struct TranslationOptions {
  GuestArchitecture Guest = GuestArchitecture::X86_64;
  HostTarget Target;
  TranslationMode Mode = TranslationMode::JIT;
  UnsupportedInstructionPolicy UnsupportedInstructions =
      UnsupportedInstructionPolicy::Fail;
  TranslationOptimizationPolicy Optimization =
      TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  LLVMOptimizationLevel LLVMLevel = LLVMOptimizationLevel::O2;
  BlockCachePolicy BlockCache = BlockCachePolicy::Enabled;
  CodeInvalidationPolicy CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  DeterministicReplayPolicy DeterministicReplay =
      DeterministicReplayPolicy::Disabled;
  bool VerifyGeneratedIR = true;
  bool PreserveExceptionState = true;
  /// The v1 contract defines scalar-integer state transitions only.  FP, SIMD,
  /// x87, atomics, and system instructions are explicitly unsupported.
  /// GuestState preserves only its modeled baseline; additional machine state
  /// requires named extension registers or a new state-schema version.
  TranslationCapability RequiredCapabilities =
      TranslationCapability::ScalarInteger;

  /// Zero means no caller-imposed limit.  Implementations must not substitute
  /// an undocumented finite cap for any zero budget.
  uint64_t InstructionBudget = 0;
  uint64_t BlockBudget = 0;
  uint64_t GeneratedCodeByteBudget = 0;
};

TranslationPairSupport getTranslationPairSupport(GuestArchitecture Guest,
                                                 GuestArchitecture Host);

TranslationCapabilityStatus
getInitialTranslationCapability(TranslationCapability Capability);

llvm::Error validateTranslationOptions(const TranslationOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONOPTIONS_H
