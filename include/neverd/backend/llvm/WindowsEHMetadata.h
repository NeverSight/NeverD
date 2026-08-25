//===- WindowsEHMetadata.h - Lifted Windows EH schema --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Stable operand names for the lossless Windows exception metadata attached
/// to lifted LLVM functions.  Keeping the schema in one header prevents the
/// emitter and PE rewrite validator from drifting apart.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_WINDOWSEHMETADATA_H
#define NEVERD_BACKEND_LLVM_WINDOWSEHMETADATA_H

#include "llvm/ADT/StringRef.h"

namespace neverd::windows_eh_md {

inline constexpr llvm::StringLiteral FunctionAttachment("neverd.windows.eh");
inline constexpr llvm::StringLiteral
    NativeAttachment("neverd.windows.eh.native");
inline constexpr llvm::StringLiteral
    FunctionTable("neverd.windows.eh.functions");
/// Operand bundle carried by llvm.sideeffect anchors that bind regenerated
/// WinEH control-flow edges back to their normalized source records.  Numeric
/// operands keep the contract independent of block and SSA names.
inline constexpr llvm::StringLiteral
    ProvenanceBundle("neverd.windows.eh.provenance");
inline constexpr unsigned ProvenanceSchemaVersion = 2;

enum class NativeProvenanceModel : unsigned {
  SEH = 1,
  CxxFH3 = 2,
  CxxFH4 = 3,
};

enum class NativeProvenanceRole : unsigned {
  ProtectedInvoke = 1,
  RegionDispatch = 2,
  HandlerTarget = 3,
  RangeEnter = 4,
  RangeExit = 5,
  RangeEnterTarget = 6,
  RangeExitTarget = 7,
};

enum ProvenanceOperand : unsigned {
  ProvenanceVersion = 0,
  ProvenanceModel,
  ProvenanceRole,
  ProvenanceFunctionVA,
  ProvenanceSourceVA,
  ProvenanceRegion,
  ProvenanceClause,
  ProvenanceAuxVA,
  ProvenanceFlags,
  ProvenanceOperandCount,
};

/// Bumped whenever an operand's position or meaning changes.  Version 8 adds
/// the original filter-thunk address to each SEH scope so ARM64 constant-true
/// normalization remains lossless and cannot silently widen output support.
/// Version 7 adds
/// the native FuncInfo address to the C++ header so separated parent, catch,
/// and cleanup contributions retain one exact group identity after LLVM
/// serialization.  Version 6 adds the x86 registration-chain record, including
/// the recovered try-level stores and the `_except_handler4` cookie header.
/// Version 5 widened each unwind operation with the register file, register
/// mask, and instruction width that the ARM and ARM64 codes carry and the x64
/// ones do not.  LLVM may preserve older opaque attachments for analysis, but
/// rewrite authentication always requires a canonical node at this version and
/// therefore fails old schemas closed.
inline constexpr unsigned SchemaVersion = 8;
inline constexpr unsigned SchemaV5OperandCount = 33;

enum FunctionOperand : unsigned {
  Version = 0,
  ParseStatus,
  Encoding,
  RuntimeKind,
  CodeBegin,
  CodeEnd,
  RuntimeFunctionRVA,
  UnwindInfoRVA,
  UnwindInfoVA,
  UnwindVersion,
  UnwindFlags,
  PrologueSize,
  FrameRegister,
  FrameOffset,
  PackedUnwindData,
  Personality,
  PersonalityName,
  PersonalityVA,
  HandlerDataVA,
  NativeUnwindBytes,
  UnwindOperations,
  Epilogs,
  SEHScopes,
  CxxHeader,
  CxxUnwindMap,
  CxxTryMap,
  CxxIPMap,
  GSCookie,
  PrimaryFunctionIndex,
  ChainedPrimaryRange,
  ChainedUnwindInfoRVA,
  Diagnostics,
  CanRegenerate,
  /// Appended in schema v6 so every schema-v5 operand retains its index.
  Registration = SchemaV5OperandCount,
  OperandCount,
};

static_assert(CanRegenerate == SchemaV5OperandCount - 1);
static_assert(Registration == SchemaV5OperandCount);
static_assert(OperandCount == SchemaV5OperandCount + 1);

enum CxxHeaderOperand : unsigned {
  CxxNativeEncoding = 0,
  CxxMagic,
  CxxFlags,
  CxxMaxState,
  CxxUnwindHelpOffset,
  CxxESTypeListVA,
  CxxBBTFlags,
  CxxFrameOffset,
  CxxIsCatchFunclet,
  CxxIsSeparated,
  CxxIsSynchronous,
  CxxIsNoExcept,
  CxxVersion,
  CxxHasDynamicStackAlignment,
  CxxExceptionSpecTypes,
  /// Appended in schema v7 so every schema-v6 C++ header operand retains its
  /// index.
  CxxNativeFuncInfoVA,
  CxxHeaderOperandCount,
};

enum SEHScopeOperand : unsigned {
  SEHScopeGuardBegin = 0,
  SEHScopeGuardEnd,
  SEHScopeKindValue,
  SEHScopeFilterOrFinallyVA,
  SEHScopeNormalizedFilterVA,
  SEHScopeHandlerVA,
  SEHScopeContinuationVA,
  SEHScopeParseStatus,
  SEHScopeOperandCount,
};

enum RegistrationOperand : unsigned {
  RegistrationHandlerVA = 0,
  RegistrationScopeTableVA,
  RegistrationTryLevelOffset,
  RegistrationTryLevelStores,
  RegistrationSeededTryLevel,
  RegistrationRecordOffset,
  RegistrationHasSecurityCookies,
  RegistrationGSCookieOffset,
  RegistrationGSCookieXOROffset,
  RegistrationEHCookieOffset,
  RegistrationEHCookieXOROffset,
  RegistrationScopeTableMagic,
  RegistrationScopes,
  RegistrationChainInstallVA,
  RegistrationChainRemoveVA,
  RegistrationOperandCount,
};

enum RegistrationTryLevelStoreOperand : unsigned {
  RegistrationStoreVA = 0,
  RegistrationStoreEndVA,
  RegistrationStoreLevel,
  RegistrationTryLevelStoreOperandCount,
};

enum RegistrationScopeOperand : unsigned {
  RegistrationScopeEnclosingLevel = 0,
  RegistrationScopeFilterVA,
  RegistrationScopeHandlerVA,
  RegistrationScopeIsFinally,
  RegistrationScopeOperandCount,
};

} // namespace neverd::windows_eh_md

#endif // NEVERD_BACKEND_LLVM_WINDOWSEHMETADATA_H
