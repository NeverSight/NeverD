//===- TranslationIRVerifier.h - Validate host translation IR -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed validation boundary between guest-code lowering and
/// host code generation.  Validation never rewrites the module.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONIRVERIFIER_H
#define NEVERD_TRANSLATE_TRANSLATIONIRVERIFIER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <system_error>

namespace llvm {
class DataLayout;
class FunctionType;
class Module;
class Triple;
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

/// Stable categories returned by the translation-IR validation boundary.
enum class TranslationIRViolation : uint8_t {
  InvalidLLVMIR = 0,
  HostTripleMismatch = 1,
  HostDataLayoutMismatch = 2,
  NonStandardBlockABI = 3,
  InlineAssembly = 4,
  TargetSpecificIntrinsic = 5,
  GuestIntegerToPointer = 6,
  ExternalSymbolNotAllowed = 7,
  HostExceptionHandling = 8,
  GuestObservableUndefOrPoison = 9,
  UnprovenPoisonGeneratingFlag = 10,
  DirectGuestMemoryAccess = 11,
  UnboundedStateOrRuntimeAccess = 12,
  InvalidPolicy = 13,
  HostPointerExposure = 14,
  MutableGlobalState = 15,
  UnsupportedHostIROperation = 16,
  IntrinsicNotAllowed = 17,
  RuntimeHelperABIMismatch = 18,
  DirectBlockCall = 19,
  UnboundedPrivateMemoryAccess = 20,
  UnprovenUndefinedBehavior = 21,
  UnprovenSemanticMetadata = 22,
  MemorySlotNotAllowed = 23,
  DispatchCycle = 24,
  BackendLibcallRisk = 25,
  RuntimeProtocolViolation = 26,
};

enum class TranslationIRMemoryRegion : uint8_t {
  State = 0,
  Runtime = 1,
};

enum class TranslationIRMemoryAccess : uint8_t {
  None = 0,
  Read = 1u << 0,
  Write = 1u << 1,
};

constexpr TranslationIRMemoryAccess operator|(TranslationIRMemoryAccess Left,
                                              TranslationIRMemoryAccess Right) {
  return static_cast<TranslationIRMemoryAccess>(static_cast<uint8_t>(Left) |
                                                static_cast<uint8_t>(Right));
}

constexpr bool hasTranslationIRMemoryAccess(TranslationIRMemoryAccess Set,
                                            TranslationIRMemoryAccess Value) {
  return (static_cast<uint8_t>(Set) & static_cast<uint8_t>(Value)) != 0;
}

/// One integer-accessible field in the backend-private state/runtime ABI.
/// Direct LLVM loads and stores must fit wholly inside one declared slot.
/// Alignment is the guaranteed alignment at Offset, not the containing
/// object's alignment; it must be a nonzero power of two.
struct TranslationIRMemorySlot {
  TranslationIRMemoryRegion Region = TranslationIRMemoryRegion::State;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  TranslationIRMemoryAccess Access = TranslationIRMemoryAccess::None;
  uint32_t Alignment = 1;
};

/// Provenance required for one runtime-helper parameter.  Scalar parameters
/// carry guest values.  Pointer parameters may name only the exact state or
/// runtime object supplied to the translated block; translated code never
/// hands a helper an arbitrary host pointer.
enum class TranslationRuntimeParameterKind : uint8_t {
  ScalarInteger = 0,
  StatePointer = 1,
  RuntimePointer = 2,
};

/// Exact runtime-helper ABI admitted by the verifier.
///
/// Name, Type, and Parameters are borrowed for the duration of
/// verifyTranslationIR().  Type must belong to the module's LLVMContext.
/// Prefix matching is deliberately unsupported: the native symbol registry
/// and this policy must identify the same finite set of functions.
struct TranslationRuntimeHelper {
  llvm::StringRef Name;
  const llvm::FunctionType *Type = nullptr;
  llvm::ArrayRef<TranslationRuntimeParameterKind> Parameters;
};

/// A typed verifier failure.  Callers inspect reason() instead of parsing the
/// diagnostic text emitted by log().
class TranslationIRVerificationError final
    : public llvm::ErrorInfo<TranslationIRVerificationError> {
public:
  static char ID;

  TranslationIRVerificationError(TranslationIRViolation Reason,
                                 std::string FunctionName,
                                 std::string Detail = {});

  TranslationIRViolation reason() const { return Reason; }
  llvm::StringRef functionName() const { return FunctionName; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationIRViolation Reason;
  std::string FunctionName;
  std::string Detail;
};

/// Verify host-native translation IR.
///
/// Every definition is a non-preemptible hidden translated block with the
/// canonical C ABI `i32 (ptr state, ptr runtime)`.  The runtime discovers
/// blocks through its private registry, never through ambient process lookup.
/// Declarations are limited to a small positive list of scalar LLVM intrinsics
/// and exact runtime helper contracts.  Calls to other translated definitions
/// are forbidden so dispatch, cancellation, invalidation, and accounting
/// cannot be bypassed.
/// Direct memory operations and private constants are single scalar integers;
/// aggregate storage must be decomposed before reaching this boundary so a
/// compact IR object cannot trigger unbounded backend expansion.
/// Instructions are limited to the host's scalar register width because wider
/// legal LLVM IR can introduce unregistered compiler-runtime calls.  Exact
/// helper declarations may use the v1 ABI's i64 guest-address and payload
/// fields on a 32-bit host; declaring that ABI performs no wide computation,
/// while instructions that construct its operands remain subject to the host
/// width rule.
/// Passing this verifier is necessary but not sufficient for execution: an
/// executable backend must also audit post-codegen control transfers and
/// relocations against the same finite runtime-symbol registry.
llvm::Error verifyTranslationIR(
    const llvm::Module &Module, const llvm::Triple &ExpectedHostTriple,
    const llvm::DataLayout &ExpectedHostDataLayout, uint64_t StateSize,
    uint64_t RuntimeSize,
    llvm::ArrayRef<TranslationIRMemorySlot> MemorySlots = {},
    llvm::ArrayRef<TranslationRuntimeHelper> RuntimeHelpers = {});

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONIRVERIFIER_H
