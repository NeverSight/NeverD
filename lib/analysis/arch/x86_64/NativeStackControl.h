//===- NativeStackControl.h - Physical near-call semantics ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_NATIVE_STACK_CONTROL_H
#define NEVERD_ANALYSIS_NATIVE_STACK_CONTROL_H

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::analysis {

enum class NativeReturnExpansion : uint8_t {
  InternalTransfer,
  /// Keep the RETURN observation before the architectural pop. The caller
  /// must independently prove preservation of the entry stack and return slot.
  OuterFunctionBoundary,
};

struct NativeStackExpansionReceipt {
  uint32_t Version = 1;
  LowInstructionBoundary OriginalBoundary;
  std::string OriginalOperationDigest;
  /// V2 additionally binds a separately validated original bank fact and the
  /// exact resulting span. It does not turn the transformed span into a new
  /// architecture instruction or assume anything about the reached callee.
  std::string PreservedStateDigest;
  std::string ExpandedOperationDigest;
  NativeReturnExpansion ReturnMode =
      NativeReturnExpansion::OuterFunctionBoundary;
};

struct NativeStackExpansion {
  std::vector<LowOp> Ops;
  LowInstructionBoundary Boundary;
  LowInstructionUndefinedEffects UndefinedEffects;
  NativeStackExpansionReceipt Receipt;
  size_t OriginalPrefixSize = 0;
  bool ExpandedIndirectCall = false;
  bool ExpandedReturn = false;
};

/// Expand a provider-certified physical near CALL/RET, or validate an already
/// explicit call-to-fallthrough push. Targets are evaluated before the push;
/// internal returns load the actual current stack word, then advance by the
/// eight-byte address plus the unsigned imm16 cleanup, when present. Outer
/// source boundaries still require zero cleanup. No target projection,
/// call-stack prediction, reachability fact or return-slot assumption enters
/// this transformation. The original instruction and its evidence are intact.
///
/// Scratch must be an eight-byte temporary whose complete range is disjoint
/// from original operands, undefined effects and ReservedTemporaries. Missing
/// or Unsupported undefined coverage is never upgraded. Complete coverage is
/// rebound only for an exact, empty original sidecar; nonempty effects require
/// an explicit timing map and are currently rejected.
llvm::Expected<NativeStackExpansion>
expandNativeStackControl(const SpecializationInstruction &Instruction,
                         NdVar Stack, NdVar Scratch,
                         NativeReturnExpansion ReturnMode,
                         llvm::ArrayRef<NdVar> ReservedTemporaries = {});

} // namespace neverd::analysis

#endif
