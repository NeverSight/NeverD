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

/// Bumped whenever an operand's position or meaning changes.  Version 5 widened
/// each unwind operation with the register file, register mask, and instruction
/// width that the ARM and ARM64 codes carry and the x64 ones do not.
inline constexpr unsigned SchemaVersion = 5;

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
  OperandCount,
};

} // namespace neverd::windows_eh_md

#endif // NEVERD_BACKEND_LLVM_WINDOWSEHMETADATA_H
