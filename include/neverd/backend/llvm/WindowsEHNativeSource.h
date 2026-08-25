//===- WindowsEHNativeSource.h - Native WinEH source checks -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Target-aware, fail-closed classification of normalized Windows exception
/// records that can be lowered to native LLVM WinEH IR without guessing.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_WINDOWSEHNATIVESOURCE_H
#define NEVERD_BACKEND_LLVM_WINDOWSEHNATIVESOURCE_H

#include "neverd/Common.h"

#include <cstdint>

namespace neverd {

struct ExceptionFunction;

/// The native language model whose source record passed every regeneration
/// predicate.  None also describes unwind-only records: structural unwind
/// regeneration is deliberately independent of this language capability.
enum class WindowsEHNativeSourceModel : uint8_t {
  None,
  SEH,
  CxxFH3,
  CxxFH4,
};

/// The operation for which a normalized Windows exception source is being
/// classified.  Native IR lowering and output reconstruction intentionally
/// have separate capability gates: verifier-clean analysis IR does not imply
/// that a binary writer can reproduce the source encoding.
enum class WindowsEHNativeCapability : uint8_t {
  IRLowering,
  OutputPatch,
};

/// Stable reason for accepting or rejecting a native Windows language source.
///
/// Keep these values append-only.  Callers may persist the corresponding
/// spelling in diagnostics and tests, while the enum keeps policy decisions
/// out of string comparisons.
enum class WindowsEHNativeSourceReason : uint8_t {
  Eligible,
  NoLanguagePersonality,
  UnsupportedObjectFormat,
  UnsupportedArchitecture,
  NonPrimaryRuntimeFunction,
  InvalidCodeRange,
  InconsistentDecodeProvenance,
  IncompleteDecode,
  LanguageOverlay,
  UnsupportedUnwindEncoding,
  MissingPersonalityAddress,
  UnknownPersonality,
  GSWrappedPersonality,
  FH4AnalysisOnly,
  UnsupportedPersonality,
  ConflictingLanguageModel,
  UnexpectedGSCookie,
  MissingSEHTable,
  EmptySEHScopeTable,
  InvalidSEHScope,
  UnsupportedSEHScopeGraph,
  MissingCxxTable,
  NonFH3Encoding,
  UnsupportedCxxVersion,
  InvalidCxxStateGraph,
  EmptyCxxTryMap,
  EmptyCxxIPMap,
  UnsupportedCxxFlags,
  UnsupportedCxxBBT,
  UnsupportedCxxCatchFunclet,
  UnsupportedCxxSeparated,
  UnsupportedCxxAsynchronous,
  UnsupportedCxxNoexcept,
  UnsupportedCxxExceptionSpecification,
  UnsupportedCxxDynamicStackAlignment,
  UnsupportedCxxUnwindAction,
  InvalidCxxTryBlock,
  UnsupportedCxxHandlerFrameState,
  InvalidCxxHandler,
  UnsupportedCxxContinuation,
  OutputReconstructionUnavailable,
  UnsupportedSEHCallbackABI,
};

struct WindowsEHNativeSourceClassification {
  WindowsEHNativeSourceModel Model = WindowsEHNativeSourceModel::None;
  WindowsEHNativeSourceReason Reason =
      WindowsEHNativeSourceReason::NoLanguagePersonality;
  WindowsEHNativeCapability RequestedCapability =
      WindowsEHNativeCapability::OutputPatch;

  bool isEligible() const {
    return Model != WindowsEHNativeSourceModel::None &&
           Reason == WindowsEHNativeSourceReason::Eligible;
  }

  bool canLowerNativeIR() const {
    return RequestedCapability == WindowsEHNativeCapability::IRLowering &&
           isEligible();
  }

  bool canPatchOutput() const {
    return RequestedCapability == WindowsEHNativeCapability::OutputPatch &&
           isEligible();
  }

  /// Compatibility spelling for output-metadata regeneration callers.
  bool canRegenerateLanguageMetadata() const { return canPatchOutput(); }
};

/// Classify whether \p EH supports \p Capability for \p TargetArch and
/// \p TargetFormat.  The default remains the conservative output-patch query
/// so existing writer callers cannot inherit newly added IR-only support.
/// The result says nothing about structural unwind-only regeneration.
WindowsEHNativeSourceClassification
classifyWindowsEHNativeSource(const ExceptionFunction &EH, Arch TargetArch,
                              BinaryFormat TargetFormat,
                              WindowsEHNativeCapability Capability =
                                  WindowsEHNativeCapability::OutputPatch);

/// Return the stable diagnostic spelling of \p Reason.
const char *
getWindowsEHNativeSourceReasonName(WindowsEHNativeSourceReason Reason);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSEHNATIVESOURCE_H
