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
  X86RegistrationSEH = 4,
  X86RegistrationCxx = 5,
};

enum class NativeProvenanceRole : unsigned {
  ProtectedInvoke = 1,
  RegionDispatch = 2,
  HandlerTarget = 3,
  RangeEnter = 4,
  RangeExit = 5,
  RangeEnterTarget = 6,
  RangeExitTarget = 7,
  RegistrationChainAccess = 8,
  RegistrationCallback = 9,
};

inline constexpr llvm::StringLiteral
    RegistrationFrameAttachment("neverd.windows.registration.frame");
inline constexpr llvm::StringLiteral
    RegistrationRootAttachment("neverd.windows.registration.root");
inline constexpr llvm::StringLiteral
    RegistrationCatchStackAttachment("neverd.windows.registration.catch-stack");
inline constexpr llvm::StringLiteral RegistrationCallerFrameAttachment(
    "neverd.windows.registration.caller-frame");
inline constexpr llvm::StringLiteral RegistrationIncomingFrameAttachment(
    "neverd.windows.registration.incoming-frame");
inline constexpr llvm::StringLiteral
    RegistrationBlockAttachment("neverd.windows.registration.source-block");
inline constexpr llvm::StringLiteral RegistrationOperationAttachment(
    "neverd.windows.registration.source-operation");
inline constexpr llvm::StringLiteral RegistrationFinallyCallAttachment(
    "neverd.windows.registration.finally-call");

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

/// Bumped whenever an operand's position or meaning changes. Version 11 binds
/// disjoint callback and cold-code chunks. Version 10 retains
/// the checked realigned-frame anchor separately from entry EBP coordinates.
/// Version 9 records
/// each x86 state store's width; narrow immediates do not assert a whole level.
/// Version 8 adds
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
inline constexpr unsigned SchemaVersion = 11;
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
  /// Disjoint code chunks, appended in schema v11.
  FragmentRanges,
  OperandCount,
};

static_assert(CanRegenerate == SchemaV5OperandCount - 1);
static_assert(Registration == SchemaV5OperandCount);
static_assert(OperandCount == SchemaV5OperandCount + 2);

enum UnwindOperationOperand : unsigned {
  UnwindOpKind = 0,
  UnwindOpCodeOffset,
  UnwindOpInfo,
  UnwindOpSlotCount,
  UnwindOpRegister,
  UnwindOpStackOffset,
  UnwindOpOperandBytes,
  UnwindOpRegisterClass,
  UnwindOpRegisterMask,
  UnwindOpInstructionSize,
  UnwindOperationOperandCount,
};

enum EpilogOperand : unsigned {
  EpilogStartOffset = 0,
  EpilogFlags,
  EpilogFirstOperationOffset,
  EpilogLastInstructionOffset,
  EpilogOperations,
  EpilogOperandCount,
};

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

enum CxxExceptionSpecOperand : unsigned {
  CxxExceptionSpecAdjectives = 0,
  CxxExceptionSpecTypeDescriptorVA,
  CxxExceptionSpecOperandCount,
};

enum CxxUnwindOperand : unsigned {
  CxxUnwindToState = 0,
  CxxUnwindActionVA,
  CxxUnwindKind,
  CxxUnwindObjectOffset,
  CxxUnwindOperandCount,
};

enum CxxTryOperand : unsigned {
  CxxTryLow = 0,
  CxxTryHigh,
  CxxCatchHigh,
  CxxTryHandlers,
  CxxTryOperandCount,
};

enum CxxCatchOperand : unsigned {
  CxxCatchAdjectives = 0,
  CxxCatchTypeDescriptorVA,
  CxxCatchObjectOffset,
  CxxCatchHandlerVA,
  CxxCatchParentFrameOffset,
  CxxCatchContinuations,
  CxxCatchOperandCount,
};

enum CxxIPOperand : unsigned {
  CxxIPVA = 0,
  CxxIPStateValue,
  CxxIPOperandCount,
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

enum GSCookieOperand : unsigned {
  GSCookieParseStatus = 0,
  GSCookieOffset,
  GSCookieHasExceptionHandler,
  GSCookieHasUnwindHandler,
  GSCookieHasAlignment,
  GSCookieAlignmentBaseOffset,
  GSCookieAlignment,
  GSCookiePayload,
  GSCookieOperandCount,
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
  RegistrationRealignedFrame,
  RegistrationOperandCount,
};

enum RegistrationRealignedFrameOperand : unsigned {
  RegistrationFrameBaseRegister = 0,
  RegistrationFrameDefinitionVA,
  RegistrationFrameAlignment,
  RegistrationFrameAllocationBytes,
  RegistrationFrameBaseOffset,
  RegistrationFrameSavedParentOffset,
  RegistrationRealignedFrameOperandCount,
};

enum RegistrationTryLevelStoreOperand : unsigned {
  RegistrationStoreVA = 0,
  RegistrationStoreEndVA,
  RegistrationStoreLevel,
  RegistrationStoreWidth,
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
