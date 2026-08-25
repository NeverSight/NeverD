//===- WindowsEHNativeSource.cpp - Native WinEH source checks -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsEHNativeSource.h"

#include "neverd/loader/ExceptionInfo.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace neverd {
namespace {

WindowsEHNativeSourceClassification
reject(WindowsEHNativeSourceModel Model, WindowsEHNativeSourceReason Reason,
       WindowsEHNativeCapability Capability) {
  return {Model, Reason, Capability};
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
  if ((Model == WindowsEHNativeSourceModel::CxxFH3 ||
       Model == WindowsEHNativeSourceModel::CxxFH4) &&
      EH.SEH)
    return true;
  return EH.Dwarf || EH.Itanium || EH.ARMEHABI || EH.Compact ||
         EH.Registration || EH.Delphi || EH.DelphiScopes || EH.Go;
}

bool hasExactBoundedX64GSCookie(const ExceptionFunction &EH) {
  if (!EH.GSCookie)
    return false;
  const GSCookieInfo &Cookie = *EH.GSCookie;
  if (Cookie.ParseStatus != ExceptionParseStatus::Complete ||
      Cookie.CookieOffset <= 0 ||
      (static_cast<uint32_t>(Cookie.CookieOffset) & 7u) != 0 ||
      !Cookie.HasExceptionHandler || !Cookie.HasUnwindHandler ||
      Cookie.HasAlignment || Cookie.AlignmentBaseOffset != 0 ||
      Cookie.Alignment != 0 || Cookie.Payload.size() != sizeof(uint32_t))
    return false;
  const uint32_t Expected =
      static_cast<uint32_t>(Cookie.CookieOffset) | uint32_t(3);
  return readLE<uint32_t>(Cookie.Payload.data()) == Expected;
}

WindowsEHNativeSourceReason validateSEH(const ExceptionFunction &EH,
                                        Arch TargetArch) {
  if (!EH.SEH)
    return WindowsEHNativeSourceReason::MissingSEHTable;
  if (EH.SEH->Scopes.empty())
    return WindowsEHNativeSourceReason::EmptySEHScopeTable;
  if (EH.SEH->Scopes.size() > std::numeric_limits<uint32_t>::max())
    return WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph;

  std::vector<ExceptionAddressRange> SemanticRanges;
  SemanticRanges.reserve(EH.SEH->Scopes.size());
  for (const SEHScopeRecord &Scope : EH.SEH->Scopes) {
    const std::optional<ExceptionAddressRange> SemanticRange =
        getSemanticSEHGuardedRange(Scope, TargetArch, EH.CodeRange);
    if (Scope.ParseStatus != ExceptionParseStatus::Complete ||
        !SemanticRange)
      return WindowsEHNativeSourceReason::InvalidSEHScope;

    switch (Scope.Kind) {
    case SEHScopeKind::Finally:
      if (Scope.NormalizedFilterVA != 0 || Scope.FilterOrFinallyVA == 0 ||
          Scope.HandlerVA != Scope.FilterOrFinallyVA ||
          Scope.ContinuationVA != 0 ||
          EH.CodeRange.contains(Scope.FilterOrFinallyVA))
        return WindowsEHNativeSourceReason::InvalidSEHScope;
      break;
    case SEHScopeKind::CatchAll:
      if (Scope.FilterOrFinallyVA != 0 || Scope.HandlerVA == 0 ||
          Scope.ContinuationVA != Scope.HandlerVA ||
          !EH.CodeRange.contains(Scope.HandlerVA) ||
          SemanticRange->contains(Scope.HandlerVA) ||
          (Scope.NormalizedFilterVA != 0 &&
           (EH.CodeRange.contains(Scope.NormalizedFilterVA) ||
            SemanticRange->contains(Scope.NormalizedFilterVA))))
        return WindowsEHNativeSourceReason::InvalidSEHScope;
      break;
    case SEHScopeKind::Filter:
      if (Scope.NormalizedFilterVA != 0 || Scope.FilterOrFinallyVA == 0 ||
          Scope.HandlerVA == 0 ||
          Scope.ContinuationVA != Scope.HandlerVA ||
          EH.CodeRange.contains(Scope.FilterOrFinallyVA) ||
          !EH.CodeRange.contains(Scope.HandlerVA) ||
          SemanticRange->contains(Scope.HandlerVA))
        return WindowsEHNativeSourceReason::InvalidSEHScope;
      break;
    default:
      return WindowsEHNativeSourceReason::InvalidSEHScope;
    }
    SemanticRanges.push_back(*SemanticRange);
  }

  for (size_t I = 0; I < SemanticRanges.size(); ++I) {
    const ExceptionAddressRange &A = SemanticRanges[I];
    for (size_t J = I + 1; J < SemanticRanges.size(); ++J) {
      const ExceptionAddressRange &B = SemanticRanges[J];
      if (!A.overlaps(B))
        continue;
      if ((A.Begin == B.Begin && A.End == B.End) ||
          (!A.contains(B) && !B.contains(A)))
        return WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph;
      // __C_specific_handler scans the native table in order. For one PC that
      // lies in nested ranges, the inner row must precede its enclosing row or
      // the outer action wins first. Native lowering builds that same
      // inner-to-outer funclet chain, so reject a source order it would change.
      if (A.contains(B))
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

WindowsEHNativeSourceReason validateCxxFH4(const ExceptionFunction &EH) {
  if (!EH.Cxx)
    return WindowsEHNativeSourceReason::MissingCxxTable;
  const CxxExceptionInfo &Cxx = *EH.Cxx;
  if (Cxx.NativeEncoding != CxxExceptionInfo::Encoding::FH4)
    return WindowsEHNativeSourceReason::NonFH3Encoding;
  if (!Cxx.hasValidStateGraph())
    return WindowsEHNativeSourceReason::InvalidCxxStateGraph;
  if (Cxx.TryBlocks.empty())
    return WindowsEHNativeSourceReason::EmptyCxxTryMap;
  if (Cxx.IPMap.empty())
    return WindowsEHNativeSourceReason::EmptyCxxIPMap;

  // The rewrite writer has one deliberately closed FH4 wire contract.  Match
  // the decoded header exactly so omitted maps, BBT/separation flags,
  // asynchronous/noexcept semantics, and future header bits never inherit
  // output eligibility by accident.
  if (Cxx.Flags != 0x38u)
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

  if (Cxx.MaxState != 2 || Cxx.UnwindMap.size() != 2)
    return WindowsEHNativeSourceReason::InvalidCxxStateGraph;
  for (const CxxUnwindAction &Action : Cxx.UnwindMap)
    if (Action.ToState != -1 || Action.ActionVA != 0 ||
        Action.Kind != CxxUnwindAction::ActionKind::None ||
        Action.ObjectOffset != 0)
      return WindowsEHNativeSourceReason::UnsupportedCxxUnwindAction;

  if (Cxx.TryBlocks.size() != 1)
    return WindowsEHNativeSourceReason::InvalidCxxTryBlock;
  const CxxTryBlock &Try = Cxx.TryBlocks.front();
  if (Try.TryLow != 0 || Try.TryHigh != 0 || Try.CatchHigh != 1 ||
      Try.Handlers.size() != 1)
    return WindowsEHNativeSourceReason::InvalidCxxTryBlock;
  const CxxCatchHandler &Catch = Try.Handlers.front();
  if (Catch.CatchObjectOffset != 0 || Catch.ParentFrameOffset != 0)
    return WindowsEHNativeSourceReason::UnsupportedCxxHandlerFrameState;
  if ((Catch.TypeDescriptorVA == 0 && Catch.Adjectives != 0x40) ||
      Catch.HandlerVA == 0 || Catch.HandlerVA == EH.CodeRange.Begin ||
      !EH.CodeRange.contains(Catch.HandlerVA))
    return WindowsEHNativeSourceReason::InvalidCxxHandler;
  if (!Catch.ContinuationVAs.empty())
    return WindowsEHNativeSourceReason::UnsupportedCxxContinuation;

  // FuncInfo4's root IP map excludes the catch funclet.  The bounded writer
  // reproduces exactly one protected interval bracketed by the empty state.
  if (Cxx.IPMap.size() != 3 || Cxx.IPMap[0].IP != EH.CodeRange.Begin ||
      Cxx.IPMap[0].State != -1 || Cxx.IPMap[1].State != 0 ||
      Cxx.IPMap[2].State != -1 ||
      !EH.CodeRange.contains(Cxx.IPMap[1].IP) ||
      (Cxx.IPMap[2].IP != EH.CodeRange.End &&
       !EH.CodeRange.contains(Cxx.IPMap[2].IP)))
    return WindowsEHNativeSourceReason::InvalidCxxStateGraph;
  return WindowsEHNativeSourceReason::Eligible;
}

} // namespace

WindowsEHNativeSourceClassification
classifyWindowsEHNativeSource(const ExceptionFunction &EH, Arch TargetArch,
                              BinaryFormat TargetFormat,
                              WindowsEHNativeCapability Capability) {
  if (!hasConsistentDecodeProvenance(EH))
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::InconsistentDecodeProvenance,
                  Capability);
  if (EH.Personality == ExceptionPersonality::None) {
    if (EH.SEH || EH.Cxx || EH.GSCookie)
      return reject(WindowsEHNativeSourceModel::None,
                    WindowsEHNativeSourceReason::ConflictingLanguageModel,
                    Capability);
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::NoLanguagePersonality,
                  Capability);
  }

  if (TargetFormat != BinaryFormat::COFF)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::UnsupportedObjectFormat,
                  Capability);
  if (TargetArch != Arch::X64 && TargetArch != Arch::ARM &&
      TargetArch != Arch::AArch64)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::UnsupportedArchitecture,
                  Capability);
  if (EH.Kind != RuntimeFunctionKind::Primary)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::NonPrimaryRuntimeFunction,
                  Capability);
  if (!EH.CodeRange.isValid())
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::InvalidCodeRange, Capability);
  if (EH.ParseStatus != ExceptionParseStatus::Complete)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::IncompleteDecode, Capability);
  if (TargetArch == Arch::ARM &&
      EH.Personality != ExceptionPersonality::CSpecificHandler)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::UnsupportedArchitecture,
                  Capability);
  if (TargetArch == Arch::AArch64 &&
      EH.Personality != ExceptionPersonality::CSpecificHandler &&
      EH.Personality != ExceptionPersonality::CxxFrameHandler3)
    return reject(WindowsEHNativeSourceModel::None,
                  WindowsEHNativeSourceReason::UnsupportedArchitecture,
                  Capability);
  WindowsEHNativeSourceModel Model = WindowsEHNativeSourceModel::None;
  switch (EH.Personality) {
  case ExceptionPersonality::CSpecificHandler:
    Model = WindowsEHNativeSourceModel::SEH;
    break;
  case ExceptionPersonality::CxxFrameHandler3:
    Model = WindowsEHNativeSourceModel::CxxFH3;
    break;
  case ExceptionPersonality::CxxFrameHandler4:
    Model = WindowsEHNativeSourceModel::CxxFH4;
    break;
  case ExceptionPersonality::GSHandlerCheckEH4:
    Model = WindowsEHNativeSourceModel::CxxFH4;
    break;
  case ExceptionPersonality::Unknown:
    return reject(Model, WindowsEHNativeSourceReason::UnknownPersonality,
                  Capability);
  case ExceptionPersonality::GSHandlerCheckSEH:
  case ExceptionPersonality::GSHandlerCheckEH:
    return reject(Model, WindowsEHNativeSourceReason::GSWrappedPersonality,
                  Capability);
  default:
    return reject(Model, WindowsEHNativeSourceReason::UnsupportedPersonality,
                  Capability);
  }

  if (EH.Rust || EH.ObjC)
    return reject(Model, WindowsEHNativeSourceReason::LanguageOverlay,
                  Capability);
  const bool HasSupportedUnwindEncoding =
      TargetArch == Arch::AArch64
          ? EH.Encoding == ExceptionEncoding::ARM64Unpacked
      : TargetArch == Arch::ARM
          ? EH.Encoding == ExceptionEncoding::ARM32Unpacked
          : (EH.Encoding == ExceptionEncoding::X64UnwindV1 ||
             EH.Encoding == ExceptionEncoding::X64UnwindV2);
  if (!HasSupportedUnwindEncoding)
    return reject(Model, WindowsEHNativeSourceReason::UnsupportedUnwindEncoding,
                  Capability);
  if (EH.PersonalityVA == 0)
    return reject(Model, WindowsEHNativeSourceReason::MissingPersonalityAddress,
                  Capability);
  if (EH.Personality == ExceptionPersonality::GSHandlerCheckEH4) {
    if (TargetArch != Arch::X64 || !hasExactBoundedX64GSCookie(EH))
      return reject(Model, WindowsEHNativeSourceReason::GSWrappedPersonality,
                    Capability);
  } else if (EH.GSCookie) {
    return reject(Model, WindowsEHNativeSourceReason::UnexpectedGSCookie,
                  Capability);
  }
  if (hasConflictingLanguageModel(EH, Model))
    return reject(Model, WindowsEHNativeSourceReason::ConflictingLanguageModel,
                  Capability);

  const WindowsEHNativeSourceReason Reason =
      Model == WindowsEHNativeSourceModel::SEH
          ? validateSEH(EH, TargetArch)
      : Model == WindowsEHNativeSourceModel::CxxFH3 ? validateCxxFH3(EH)
                                                    : validateCxxFH4(EH);
  if (Reason != WindowsEHNativeSourceReason::Eligible)
    return reject(Model, Reason, Capability);
  if (Model == WindowsEHNativeSourceModel::SEH) {
    const bool HasNormalizedFilter = std::any_of(
        EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
        [](const SEHScopeRecord &Scope) {
          return Scope.NormalizedFilterVA != 0;
        });
    if (TargetArch != Arch::AArch64 && HasNormalizedFilter)
      return reject(Model, WindowsEHNativeSourceReason::InvalidSEHScope,
                    Capability);
    if (TargetArch == Arch::X64)
      return {Model, Reason, Capability};
    if (TargetArch == Arch::ARM) {
      if (((EH.CodeRange.Begin | EH.CodeRange.End) & 1u) != 0 ||
          std::any_of(EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
                      [](const SEHScopeRecord &Scope) {
                        return ((Scope.GuardedRange.Begin |
                                 Scope.GuardedRange.End | Scope.HandlerVA) &
                                1u) != 0;
                      }))
        return reject(Model, WindowsEHNativeSourceReason::InvalidSEHScope,
                      Capability);
      if (EH.SEH->Scopes.size() != 1)
        return reject(Model,
                      WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph,
                      Capability);
      if (std::any_of(EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
                      [](const SEHScopeRecord &Scope) {
                        return Scope.Kind != SEHScopeKind::CatchAll;
                      }))
        return reject(
            Model, WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI,
            Capability);
      return {Model, Reason, Capability};
    }
    if (std::any_of(EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
                    [](const SEHScopeRecord &Scope) {
                      return ((Scope.FilterOrFinallyVA |
                               Scope.NormalizedFilterVA | Scope.HandlerVA) &
                              3u) != 0;
                    }))
      return reject(Model, WindowsEHNativeSourceReason::InvalidSEHScope,
                    Capability);
    if (Capability == WindowsEHNativeCapability::OutputPatch &&
        EH.SEH->Scopes.size() != 1)
      return reject(Model,
                    WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph,
                    Capability);
    if ((Capability == WindowsEHNativeCapability::OutputPatch &&
         HasNormalizedFilter) ||
        std::any_of(EH.SEH->Scopes.begin(), EH.SEH->Scopes.end(),
                    [](const SEHScopeRecord &Scope) {
                      return Scope.Kind != SEHScopeKind::CatchAll;
                    }))
      return reject(
          Model, WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI,
          Capability);
  }
  return {Model, Reason, Capability};
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
  case WindowsEHNativeSourceReason::OutputReconstructionUnavailable:
    return "output-reconstruction-unavailable";
  case WindowsEHNativeSourceReason::UnsupportedSEHCallbackABI:
    return "unsupported-seh-callback-abi";
  }
  return "unknown";
}

} // namespace neverd
