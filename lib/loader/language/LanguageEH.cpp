//===- LanguageEH.cpp - Non-Windows exception model names -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Out-of-line names for the language exception models.  Kept in one place so
/// a new enumerator produces a link error until every model has been given a
/// stable spelling, rather than silently printing "unknown" in a dump.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/LanguageEH.h"

namespace neverd {

const char *getCFIOpKindName(CFIOpKind Kind) {
  switch (Kind) {
  case CFIOpKind::Nop:
    return "nop";
  case CFIOpKind::SetLoc:
    return "set-loc";
  case CFIOpKind::AdvanceLoc:
    return "advance-loc";
  case CFIOpKind::DefCFA:
    return "def-cfa";
  case CFIOpKind::DefCFARegister:
    return "def-cfa-register";
  case CFIOpKind::DefCFAOffset:
    return "def-cfa-offset";
  case CFIOpKind::DefCFAExpression:
    return "def-cfa-expression";
  case CFIOpKind::Offset:
    return "offset";
  case CFIOpKind::ValOffset:
    return "val-offset";
  case CFIOpKind::Register:
    return "register";
  case CFIOpKind::Expression:
    return "expression";
  case CFIOpKind::ValExpression:
    return "val-expression";
  case CFIOpKind::Restore:
    return "restore";
  case CFIOpKind::Undefined:
    return "undefined";
  case CFIOpKind::SameValue:
    return "same-value";
  case CFIOpKind::RememberState:
    return "remember-state";
  case CFIOpKind::RestoreState:
    return "restore-state";
  case CFIOpKind::GnuArgsSize:
    return "gnu-args-size";
  case CFIOpKind::NegateRAState:
    return "negate-ra-state";
  case CFIOpKind::NegateRAStateWithPC:
    return "negate-ra-state-with-pc";
  case CFIOpKind::Opaque:
    return "opaque";
  }
  return "unknown";
}

const char *getCompactUnwindKindName(CompactUnwindKind Kind) {
  switch (Kind) {
  case CompactUnwindKind::None:
    return "none";
  case CompactUnwindKind::FramePointer:
    return "frame-pointer";
  case CompactUnwindKind::FramelessImmediate:
    return "frameless-immediate";
  case CompactUnwindKind::FramelessIndirect:
    return "frameless-indirect";
  case CompactUnwindKind::DwarfFDE:
    return "dwarf-fde";
  case CompactUnwindKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *getARMEHABIEntryKindName(ARMEHABIEntryKind Kind) {
  switch (Kind) {
  case ARMEHABIEntryKind::CantUnwind:
    return "cantunwind";
  case ARMEHABIEntryKind::InlineCompact:
    return "inline-compact";
  case ARMEHABIEntryKind::Compact:
    return "compact";
  case ARMEHABIEntryKind::Generic:
    return "generic";
  }
  return "unknown";
}

const char *getARMTypeTableConventionName(ARMTypeTableConvention Convention) {
  switch (Convention) {
  case ARMTypeTableConvention::Unknown:
    return "unknown";
  case ARMTypeTableConvention::Absolute:
    return "absolute";
  case ARMTypeTableConvention::PCRelative:
    return "pcrel";
  case ARMTypeTableConvention::PCRelativeIndirect:
    return "pcrel-indirect";
  }
  return "unknown";
}

const char *getDelphiHandlerKindName(DelphiHandlerKind Kind) {
  switch (Kind) {
  case DelphiHandlerKind::Unknown:
    return "unknown";
  case DelphiHandlerKind::Finally:
    return "finally";
  case DelphiHandlerKind::AnyException:
    return "any-exception";
  case DelphiHandlerKind::OnException:
    return "on-exception";
  case DelphiHandlerKind::AutoException:
    return "auto-exception";
  }
  return "unknown";
}

const char *getDelphiScopeKindName(DelphiScopeKind Kind) {
  switch (Kind) {
  case DelphiScopeKind::Finally:
    return "finally";
  case DelphiScopeKind::SafecallCatch:
    return "safecall-catch";
  case DelphiScopeKind::CatchAll:
    return "catch-all";
  case DelphiScopeKind::OnException:
    return "on-exception";
  }
  return "unknown";
}

const char *getObjCRuntimeKindName(ObjCRuntimeKind Kind) {
  switch (Kind) {
  case ObjCRuntimeKind::AppleNonFragile:
    return "apple-non-fragile";
  case ObjCRuntimeKind::GNU:
    return "gnu";
  case ObjCRuntimeKind::GNUstepObjCXX:
    return "gnustep-objcxx";
  }
  return "unknown";
}

const char *getObjCCatchKindName(ObjCCatchKind Kind) {
  switch (Kind) {
  case ObjCCatchKind::Class:
    return "class";
  case ObjCCatchKind::AnyObject:
    return "any-object";
  case ObjCCatchKind::CatchAll:
    return "catch-all";
  }
  return "unknown";
}

const char *getObjCPadKindName(ObjCPadKind Kind) {
  switch (Kind) {
  case ObjCPadKind::Cleanup:
    return "cleanup";
  case ObjCPadKind::Catch:
    return "catch";
  case ObjCPadKind::SynchronizedExit:
    return "synchronized-exit";
  }
  return "unknown";
}

const char *getObjCRuntimeCallKindName(ObjCRuntimeCallKind Kind) {
  switch (Kind) {
  case ObjCRuntimeCallKind::Throw:
    return "throw";
  case ObjCRuntimeCallKind::Rethrow:
    return "rethrow";
  case ObjCRuntimeCallKind::BeginCatch:
    return "begin-catch";
  case ObjCRuntimeCallKind::EndCatch:
    return "end-catch";
  case ObjCRuntimeCallKind::SyncEnter:
    return "sync-enter";
  case ObjCRuntimeCallKind::SyncExit:
    return "sync-exit";
  case ObjCRuntimeCallKind::Terminate:
    return "terminate";
  case ObjCRuntimeCallKind::ARCCleanup:
    return "arc-cleanup";
  case ObjCRuntimeCallKind::FragileTry:
    return "fragile-try";
  }
  return "unknown";
}

const char *getRustLandingPadKindName(RustLandingPadKind Kind) {
  switch (Kind) {
  case RustLandingPadKind::DropGlue:
    return "drop-glue";
  case RustLandingPadKind::CatchUnwind:
    return "catch-unwind";
  case RustLandingPadKind::NoUnwindGuard:
    return "nounwind-guard";
  }
  return "unknown";
}

const char *getRustPanicKindName(RustPanicKind Kind) {
  switch (Kind) {
  case RustPanicKind::Explicit:
    return "explicit";
  case RustPanicKind::BoundsCheck:
    return "bounds-check";
  case RustPanicKind::Arithmetic:
    return "arithmetic";
  case RustPanicKind::NoUnwind:
    return "nounwind";
  case RustPanicKind::Resume:
    return "resume";
  }
  return "unknown";
}

const char *getRustPanicStrategyName(RustPanicStrategy Strategy) {
  switch (Strategy) {
  case RustPanicStrategy::Unknown:
    return "unknown";
  case RustPanicStrategy::Unwind:
    return "unwind";
  case RustPanicStrategy::Abort:
    return "abort";
  }
  return "unknown";
}

const char *getGoDeferKindName(GoDeferKind Kind) {
  switch (Kind) {
  case GoDeferKind::Heap:
    return "heap";
  case GoDeferKind::Stack:
    return "stack";
  case GoDeferKind::OpenCoded:
    return "open-coded";
  }
  return "unknown";
}

const char *getGoOpenCodedDeferLayoutName(GoOpenCodedDeferLayout Layout) {
  switch (Layout) {
  case GoOpenCodedDeferLayout::Contiguous:
    return "contiguous";
  case GoOpenCodedDeferLayout::Enumerated:
    return "enumerated";
  case GoOpenCodedDeferLayout::LegacyEnumerated:
    return "legacy-enumerated";
  }
  return "unknown";
}

const char *getGoUnsafePointKindName(GoUnsafePointKind Kind) {
  switch (Kind) {
  case GoUnsafePointKind::Safe:
    return "safe";
  case GoUnsafePointKind::Unsafe:
    return "unsafe";
  case GoUnsafePointKind::RestartSequence:
    return "restart-sequence";
  case GoUnsafePointKind::RestartAtEntry:
    return "restart-at-entry";
  case GoUnsafePointKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace neverd
