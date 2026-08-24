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
};

struct WindowsEHNativeSourceClassification {
  WindowsEHNativeSourceModel Model = WindowsEHNativeSourceModel::None;
  WindowsEHNativeSourceReason Reason =
      WindowsEHNativeSourceReason::NoLanguagePersonality;

  bool canRegenerateLanguageMetadata() const {
    return Model != WindowsEHNativeSourceModel::None &&
           Reason == WindowsEHNativeSourceReason::Eligible;
  }
};

/// Classify whether \p EH can be regenerated as native LLVM WinEH language IR
/// for \p TargetArch and \p TargetFormat.  The result says nothing about
/// structural unwind-only regeneration.
WindowsEHNativeSourceClassification
classifyWindowsEHNativeSource(const ExceptionFunction &EH, Arch TargetArch,
                              BinaryFormat TargetFormat);

/// Return the stable diagnostic spelling of \p Reason.
const char *
getWindowsEHNativeSourceReasonName(WindowsEHNativeSourceReason Reason);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSEHNATIVESOURCE_H
