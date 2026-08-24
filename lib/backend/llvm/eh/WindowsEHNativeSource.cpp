//===- WindowsEHNativeSource.cpp - Native WinEH source checks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsEHNativeSource.h"

#include "neverd/loader/ExceptionInfo.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace neverd {
namespace {

WindowsEHNativeSourceClassification
reject(WindowsEHNativeSourceModel Model, WindowsEHNativeSourceReason Reason) {
  return {Model, Reason};
}

bool hasConsistentDecodeProvenance(const ExceptionFunction &EH) {
  if (!EH.DecodeProvenance)
    return true;

  const ExceptionFunctionDecodeProvenance &Provenance = *EH.DecodeProvenance;
  if (EH.ParseStatus !=
      mergeExceptionParseStatus(Provenance.Structural.ParseStatus,
                                Provenance.Language.ParseStatus))
    return false;

  std::vector<std::string> Diagnostics = Provenance.Structural.Diagnostics;
  Diagnostics.insert(Diagnostics.end(), Provenance.Language.Diagnostics.begin(),
                     Provenance.Language.Diagnostics.end());
  return EH.Diagnostics == Diagnostics;
}

bool hasConflictingLanguageModel(const ExceptionFunction &EH,
                                 WindowsEHNativeSourceModel Model) {
  if (Model == WindowsEHNativeSourceModel::SEH && EH.Cxx)
    return true;
  if (Model == WindowsEHNativeSourceModel::CxxFH3 && EH.SEH)
    return true;
  return EH.Dwarf || EH.Itanium || EH.ARMEHABI || EH.Compact ||
         EH.Registration || EH.Delphi || EH.DelphiScopes || EH.Go;
}

WindowsEHNativeSourceReason validateSEH(const ExceptionFunction &EH) {
  if (!EH.SEH)
    return WindowsEHNativeSourceReason::MissingSEHTable;
  if (EH.SEH->Scopes.empty())
    return WindowsEHNativeSourceReason::EmptySEHScopeTable;
  if (EH.SEH->Scopes.size() > std::numeric_limits<uint32_t>::max())
    return WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph;

  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !Scope.GuardedRange.isValid() ||
        !EH.CodeRange.contains(Scope.GuardedRange))
      return WindowsEHNativeSourceReason::InvalidSEHScope;

    switch (Scope.Kind) {
    case SEHScopeKind::Finally:
      if (Scope.FilterOrFinallyVA == 0 ||
          Scope.HandlerVA != Scope.FilterOrFinallyVA ||
          Scope.ContinuationVA != 0 ||
          EH.CodeRange.contains(Scope.FilterOrFinallyVA))
        return WindowsEHNativeSourceReason::InvalidSEHScope;
      break;
    case SEHScopeKind::CatchAll:
      if (Scope.FilterOrFinallyVA != 0 || Scope.HandlerVA == 0 ||
          Scope.ContinuationVA != Scope.HandlerVA ||
          !EH.CodeRange.contains(Scope.HandlerVA) ||
          Scope.GuardedRange.contains(Scope.HandlerVA))
        return WindowsEHNativeSourceReason::InvalidSEHScope;
      break;
    case SEHScopeKind::Filter:
      if (Scope.FilterOrFinallyVA == 0 || Scope.HandlerVA == 0 ||
          Scope.ContinuationVA != Scope.HandlerVA ||
          EH.CodeRange.contains(Scope.FilterOrFinallyVA) ||
          !EH.CodeRange.contains(Scope.HandlerVA) ||
          Scope.GuardedRange.contains(Scope.HandlerVA))
        return WindowsEHNativeSourceReason::InvalidSEHScope;
      break;
    default:
      return WindowsEHNativeSourceReason::InvalidSEHScope;
    }
  }

  for (size_t I = 0; I < EH.SEH->Scopes.size(); ++I) {
    const ExceptionAddressRange &A = EH.SEH->Scopes[I].GuardedRange;
    for (size_t J = I + 1; J < EH.SEH->Scopes.size(); ++J) {
      const ExceptionAddressRange &B = EH.SEH->Scopes[J].GuardedRange;
      if (!A.overlaps(B))
        continue;
      if ((A.Begin == B.Begin && A.End == B.End) ||
          (!A.contains(B) && !B.contains(A)))
        return WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph;
    }
  }
  return WindowsEHNativeSourceReason::Eligible;
}

WindowsEHNativeSourceReason validateCxxFH3(const ExceptionFunction &EH) {
  if (!EH.Cxx)
    return WindowsEHNativeSourceReason::MissingCxxTable;
  const CxxExceptionInfo &Cxx = *EH.Cxx;
  if (Cxx.NativeEncoding != CxxExceptionInfo::Encoding::FH3)
    return WindowsEHNativeSourceReason::NonFH3Encoding;
  if (Cxx.Magic != 0x19930522u ||
      Cxx.Version != CxxFuncInfoVersion::WithEHFlags)
    return WindowsEHNativeSourceReason::UnsupportedCxxVersion;
  if (!Cxx.hasValidStateGraph())
    return WindowsEHNativeSourceReason::InvalidCxxStateGraph;
  if (Cxx.TryBlocks.empty())
    return WindowsEHNativeSourceReason::EmptyCxxTryMap;
  if (Cxx.IPMap.empty())
    return WindowsEHNativeSourceReason::EmptyCxxIPMap;
  if (Cxx.Flags != 1u)
    return WindowsEHNativeSourceReason::UnsupportedCxxFlags;
  if (Cxx.BBTFlags != 0)
    return WindowsEHNativeSourceReason::UnsupportedCxxBBT;
  if (Cxx.IsCatchFunclet)
    return WindowsEHNativeSourceReason::UnsupportedCxxCatchFunclet;
  if (Cxx.IsSeparated)
    return WindowsEHNativeSourceReason::UnsupportedCxxSeparated;
  if (!Cxx.IsSynchronous)
    return WindowsEHNativeSourceReason::UnsupportedCxxAsynchronous;
  if (Cxx.IsNoExcept)
    return WindowsEHNativeSourceReason::UnsupportedCxxNoexcept;
  if (Cxx.hasExceptionSpecification() || !Cxx.ExceptionSpecTypes.empty())
    return WindowsEHNativeSourceReason::UnsupportedCxxExceptionSpecification;
  if (Cxx.HasDynamicStackAlignment)
    return WindowsEHNativeSourceReason::UnsupportedCxxDynamicStackAlignment;
  if (Cxx.FrameOffset != 0)
    return WindowsEHNativeSourceReason::UnsupportedCxxHandlerFrameState;

  for (const CxxUnwindAction &Action : Cxx.UnwindMap)
    if (Action.ActionVA != 0 ||
        Action.Kind != CxxUnwindAction::ActionKind::None ||
        Action.ObjectOffset != 0)
      return WindowsEHNativeSourceReason::UnsupportedCxxUnwindAction;

  for (const CxxIPState &IP : Cxx.IPMap)
    if (!EH.CodeRange.contains(IP.IP) && IP.IP != EH.CodeRange.End)
      return WindowsEHNativeSourceReason::InvalidCxxStateGraph;

  if (Cxx.TryBlocks.size() > std::numeric_limits<uint32_t>::max())
    return WindowsEHNativeSourceReason::InvalidCxxTryBlock;
  auto StateAt = [&](va_t Address) {
    int32_t State = -1;
    for (const CxxIPState &IP : Cxx.IPMap) {
      if (IP.IP > Address)
        break;
      State = IP.State;
    }
    return State;
  };
  for (const CxxTryBlock &Try : Cxx.TryBlocks) {
    if (Try.Handlers.empty() ||
        Try.Handlers.size() > std::numeric_limits<uint32_t>::max())
      return WindowsEHNativeSourceReason::InvalidCxxTryBlock;
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      if (Catch.CatchObjectOffset != 0 || Catch.ParentFrameOffset != 0)
        return WindowsEHNativeSourceReason::UnsupportedCxxHandlerFrameState;
      if (Catch.HandlerVA == 0 || Catch.HandlerVA == EH.CodeRange.Begin ||
          !EH.CodeRange.contains(Catch.HandlerVA))
        return WindowsEHNativeSourceReason::InvalidCxxHandler;
      const int32_t HandlerState = StateAt(Catch.HandlerVA);
      if (HandlerState <= Try.TryHigh || HandlerState > Try.CatchHigh)
        return WindowsEHNativeSourceReason::InvalidCxxHandler;
      if (!Catch.ContinuationVAs.empty())
        return WindowsEHNativeSourceReason::UnsupportedCxxContinuation;
    }
  }

  for (size_t I = 0; I < Cxx.TryBlocks.size(); ++I) {
    const CxxTryBlock &A = Cxx.TryBlocks[I];
    for (size_t J = I + 1; J < Cxx.TryBlocks.size(); ++J) {
      const CxxTryBlock &B = Cxx.TryBlocks[J];
      const bool Overlap = A.TryLow <= B.TryHigh && B.TryLow <= A.TryHigh;
      if (!Overlap)
        continue;
      const bool AContainsB = A.TryLow <= B.TryLow && A.TryHigh >= B.TryHigh;
      const bool BContainsA = B.TryLow <= A.TryLow && B.TryHigh >= A.TryHigh;
      const bool Equal = A.TryLow == B.TryLow && A.TryHigh == B.TryHigh;
      if (Equal || (!AContainsB && !BContainsA))
        return WindowsEHNativeSourceReason::InvalidCxxTryBlock;
    }
  }
  return WindowsEHNativeSourceReason::Eligible;
}

} // namespace

WindowsEHNativeSourceClassification
classifyWindowsEHNativeSource(const ExceptionFunction &EH, Arch TargetArch,
                              BinaryFormat TargetFormat) {
  if (!hasConsistentDecodeProvenance(EH))
    return reject(
        WindowsEHNativeSourceModel::None,
        WindowsEHNativeSourceReason::InconsistentDecodeProvenance);
  if (EH.Personality == ExceptionPersonality::None) {
    if (EH.SEH || EH.Cxx || EH.GSCookie)
      return reject(WindowsEHNativeSourceModel::None,
                    WindowsEHNativeSourceReason::ConflictingLanguageModel);
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::NoLanguagePersonality);
  }

  if (TargetFormat != BinaryFormat::COFF)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::UnsupportedObjectFormat);
  if (TargetArch != Arch::X64)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::UnsupportedArchitecture);
  if (EH.Kind != RuntimeFunctionKind::Primary)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::NonPrimaryRuntimeFunction);
  if (!EH.CodeRange.isValid())
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::InvalidCodeRange);
  if (EH.ParseStatus != ExceptionParseStatus::Complete)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::IncompleteDecode);
  WindowsEHNativeSourceModel Model = WindowsEHNativeSourceModel::None;
  switch (EH.Personality) {
  case ExceptionPersonality::CSpecificHandler:
    Model = WindowsEHNativeSourceModel::SEH;
    break;
  case ExceptionPersonality::CxxFrameHandler3:
    Model = WindowsEHNativeSourceModel::CxxFH3;
    break;
  case ExceptionPersonality::Unknown:
    return reject(Model, WindowsEHNativeSourceReason::UnknownPersonality);
  case ExceptionPersonality::CxxFrameHandler4:
    return reject(Model, WindowsEHNativeSourceReason::FH4AnalysisOnly);
  case ExceptionPersonality::GSHandlerCheckSEH:
  case ExceptionPersonality::GSHandlerCheckEH:
  case ExceptionPersonality::GSHandlerCheckEH4:
    return reject(Model, WindowsEHNativeSourceReason::GSWrappedPersonality);
  default:
    return reject(Model, WindowsEHNativeSourceReason::UnsupportedPersonality);
  }

  if (EH.Rust || EH.ObjC)
    return reject(Model, WindowsEHNativeSourceReason::LanguageOverlay);
  if (EH.Encoding != ExceptionEncoding::X64UnwindV1 &&
      EH.Encoding != ExceptionEncoding::X64UnwindV2)
    return reject(Model, WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);
  if (EH.PersonalityVA == 0)
    return reject(Model,
                  WindowsEHNativeSourceReason::MissingPersonalityAddress);
  if (EH.GSCookie)
    return reject(Model, WindowsEHNativeSourceReason::UnexpectedGSCookie);
  if (hasConflictingLanguageModel(EH, Model))
    return reject(Model, WindowsEHNativeSourceReason::ConflictingLanguageModel);

  const WindowsEHNativeSourceReason Reason =
      Model == WindowsEHNativeSourceModel::SEH ? validateSEH(EH)
                                               : validateCxxFH3(EH);
  return {Model, Reason};
}

const char *
getWindowsEHNativeSourceReasonName(WindowsEHNativeSourceReason Reason) {
  switch (Reason) {
  case WindowsEHNativeSourceReason::Eligible:
    return "eligible";
  case WindowsEHNativeSourceReason::NoLanguagePersonality:
    return "no-language-personality";
  case WindowsEHNativeSourceReason::UnsupportedObjectFormat:
    return "unsupported-object-format";
  case WindowsEHNativeSourceReason::UnsupportedArchitecture:
    return "unsupported-architecture";
  case WindowsEHNativeSourceReason::NonPrimaryRuntimeFunction:
    return "non-primary-runtime-function";
  case WindowsEHNativeSourceReason::InvalidCodeRange:
    return "invalid-code-range";
  case WindowsEHNativeSourceReason::InconsistentDecodeProvenance:
    return "inconsistent-decode-provenance";
  case WindowsEHNativeSourceReason::IncompleteDecode:
    return "incomplete-decode";
  case WindowsEHNativeSourceReason::LanguageOverlay:
    return "language-overlay";
  case WindowsEHNativeSourceReason::UnsupportedUnwindEncoding:
    return "unsupported-unwind-encoding";
  case WindowsEHNativeSourceReason::MissingPersonalityAddress:
    return "missing-personality-address";
  case WindowsEHNativeSourceReason::UnknownPersonality:
    return "unknown-personality";
  case WindowsEHNativeSourceReason::GSWrappedPersonality:
    return "gs-wrapped-personality";
  case WindowsEHNativeSourceReason::FH4AnalysisOnly:
    return "fh4-analysis-only";
  case WindowsEHNativeSourceReason::UnsupportedPersonality:
    return "unsupported-personality";
  case WindowsEHNativeSourceReason::ConflictingLanguageModel:
    return "conflicting-language-model";
  case WindowsEHNativeSourceReason::UnexpectedGSCookie:
    return "unexpected-gs-cookie";
  case WindowsEHNativeSourceReason::MissingSEHTable:
    return "missing-seh-table";
  case WindowsEHNativeSourceReason::EmptySEHScopeTable:
    return "empty-seh-scope-table";
  case WindowsEHNativeSourceReason::InvalidSEHScope:
    return "invalid-seh-scope";
  case WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph:
    return "unsupported-seh-scope-graph";
  case WindowsEHNativeSourceReason::MissingCxxTable:
    return "missing-cxx-table";
  case WindowsEHNativeSourceReason::NonFH3Encoding:
    return "non-fh3-encoding";
  case WindowsEHNativeSourceReason::UnsupportedCxxVersion:
    return "unsupported-cxx-version";
  case WindowsEHNativeSourceReason::InvalidCxxStateGraph:
    return "invalid-cxx-state-graph";
  case WindowsEHNativeSourceReason::EmptyCxxTryMap:
    return "empty-cxx-try-map";
  case WindowsEHNativeSourceReason::EmptyCxxIPMap:
    return "empty-cxx-ip-map";
  case WindowsEHNativeSourceReason::UnsupportedCxxFlags:
    return "unsupported-cxx-flags";
  case WindowsEHNativeSourceReason::UnsupportedCxxBBT:
    return "unsupported-cxx-bbt";
  case WindowsEHNativeSourceReason::UnsupportedCxxCatchFunclet:
    return "unsupported-cxx-catch-funclet";
  case WindowsEHNativeSourceReason::UnsupportedCxxSeparated:
    return "unsupported-cxx-separated";
  case WindowsEHNativeSourceReason::UnsupportedCxxAsynchronous:
    return "unsupported-cxx-asynchronous";
  case WindowsEHNativeSourceReason::UnsupportedCxxNoexcept:
    return "unsupported-cxx-noexcept";
  case WindowsEHNativeSourceReason::UnsupportedCxxExceptionSpecification:
    return "unsupported-cxx-exception-specification";
  case WindowsEHNativeSourceReason::UnsupportedCxxDynamicStackAlignment:
    return "unsupported-cxx-dynamic-stack-alignment";
  case WindowsEHNativeSourceReason::UnsupportedCxxUnwindAction:
    return "unsupported-cxx-unwind-action";
  case WindowsEHNativeSourceReason::InvalidCxxTryBlock:
    return "invalid-cxx-try-block";
  case WindowsEHNativeSourceReason::UnsupportedCxxHandlerFrameState:
    return "unsupported-cxx-handler-frame-state";
  case WindowsEHNativeSourceReason::InvalidCxxHandler:
    return "invalid-cxx-handler";
  case WindowsEHNativeSourceReason::UnsupportedCxxContinuation:
    return "unsupported-cxx-continuation";
  }
  return "unknown";
}

} // namespace neverd
