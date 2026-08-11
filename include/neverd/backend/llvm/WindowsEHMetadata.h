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
inline constexpr unsigned SchemaVersion = 3;

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
