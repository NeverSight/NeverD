//===- MedToHigh.h - MedIR to HighIR conversion -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares MedToHighConverter which transforms a MedFunc into a HighFunc
/// by building expression trees, structuring control flow (if/while/switch),
/// inferring types, and performing cleanup passes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_MEDTOHIGH_H
#define NEVERD_IR_HIGH_MEDTOHIGH_H

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include <set>

namespace neverd {

struct BinaryImage;

Intrinsic intrinsicId(const MedOp &Op);
std::string intrinsicName(const MedOp &Op);
uint16_t inferReturnSize(const MedFunc &Med);
std::set<uint64_t> detectPtrParamRegs(const MedFunc &Med);
/// Stack-passed parameter ids (`MedVar::Param`) used as memory addresses.
std::set<int> detectPtrParamIds(const MedFunc &Med);

/// Rewrite expressions by what they compute rather than by how they are
/// written, replacing each with the shortest equivalent the symbolic engine
/// can find.
///
/// This is what reaches a mixture of arithmetic and bitwise operators, where
/// every rewrite either algebra can state is blocked by an operator belonging
/// to the other — the shape obfuscation is built out of, and the one a
/// peephole pass cannot touch.  Only whole-word bitvector arithmetic is
/// carried across; a load, a call, a cast or a comparison becomes one opaque
/// input and comes back untouched.
///
/// The standard HighIR cleanup runs this after copy propagation, dead-code
/// elimination and renaming have assembled the final expression DAG.  A
/// rewrite must strictly improve the engine's rendered-tree cost, which avoids
/// replacing a compact shared form with text that repeats one of its
/// subexpressions.
void simplifyExprSemantics(std::vector<HighStmt> &Stmts);

/// Fold shared branch continuations after dead assignments have been removed.
/// Preserve external entries and require exact fallthrough destinations.
void foldStructuredContinuations(HighFunc &Func, const MedFunc *Med = nullptr);

/// Emit a label-per-block goto/return skeleton.  Used when structuring would
/// exceed SSA limits, or when conversion fails and identity alone would leave
/// an empty HighFunc that HighC can only trap.
void fillUnstructuredGotoSkeleton(HighFunc &Func, const MedFunc &Med);

class MedToHighConverter {
public:
  HighFunc convert(const MedFunc &Med, Arch TheArch = Arch::Unknown);

  void setBinaryImage(const BinaryImage *Img) { Image = Img; }

  void setFuncNames(const std::map<va_t, std::string> *Names) {
    FuncNames = Names;
  }
  void setJumpTables(const std::vector<JumpTable> &JTs) { JumpTables = JTs; }

  struct CallIndTarget {
    std::string Name = "indirect";
    va_t Addr = 0;
    bool IsIndirect = true;
    int IndirectParam = -1;
  };

private:
  void buildExpressions(const MedFunc &Med);
  void structureControlFlow(HighFunc &Func, const MedFunc &Med);
  void structureExceptionRegions(HighFunc &Func, const MedFunc &Med);
  void inferTypes(HighFunc &Func);
  void simplifyControlFlow(HighFunc &Func, const MedFunc &Med);
  void inlineGotoReturns(HighFunc &Func, const MedFunc &Med);
  void eliminateDeadStmts(HighFunc &Func);
  void stripPrologueEpilogue(HighFunc &Func);
  void ensureTrailingReturn(HighFunc &Func, const MedFunc &Med);

  ExprPtr medOpToExpr(const MedOp &Op);
  ExprPtr medvarToExpr(const MedVar &V);
  ExprPtr sourceBitSlice(const ExprPtr &Value, uint64_t ByteOffset,
                         uint16_t Bytes, unsigned Depth = 0);
  ExprPtr sourceFloatValue(const MedVar &Value, uint16_t Bytes);
  ExprPtr sourceScalarValue(const MedVar &Value, const TypeRef &Type);
  ExprPtr inlineableDefinition(VarKey Key) const;
  ExprPtr forceInlineExpr(const ExprPtr &E);
  /// Like \ref forceInlineExpr, but a unique Load / add / copy may inline
  /// even when the dest is a memory-read output.  Used for `INDIR_CALL`
  /// callees so HighC prints `(*(*p))(...)` instead of an undeclared temp.
  ExprPtr forceInlineCallTarget(const ExprPtr &E);

  int regToArgIdx(uint64_t RegOff) const;
  /// Map a MedIR parameter or its entry register to the ABI slot index in
  /// `CurMed->Params`.  MedIR often stores the SSA id in `MedVar::Id`, which
  /// is not the rcx/rdx/r8/r9 (or stack) slot HighC uses.
  int abiParamIndex(const MedVar &V) const;

  /// Prefer a non-synthetic FuncNames entry, then an import or image symbol.
  std::string calleeDisplayName(va_t Target) const;

  std::vector<ExprPtr> collectCallArgs(const MedBlock &CurBlock,
                                       size_t CallIdx);

  /// Resolve the SSA variable of register \p RegOff reaching the ENTRY of
  /// \p B (its live-in value: a PHI in B, else the single reaching definition
  /// walked back through predecessors).  Used to recover a register call
  /// argument that is live-in to the call block rather than written before the
  /// call.  Returns false when unresolved.  Requires CurMed.
  bool reachingRegAtBlockEntry(const MedBlock &B, uint64_t RegOff,
                               MedVar &Out) const;
  /// True when an immediate predecessor wrote \p LiveIn into \p RegOff
  /// as call setup. Earlier leftover values (Find's nKey still in r9)
  /// are not arguments of a later call. Also covers `lea r8` in the
  /// shared fork of `mov edx; jmp call` join arms.
  bool isCallArgSetupDef(const MedBlock &CallBlk, const MedVar &LiveIn,
                         uint64_t RegOff) const;

  void lowerStore(HighFunc &Func, const MedOp &CurOp);
  void lowerCall(HighFunc &Func, const MedBlock &CurBlock, const MedOp &CurOp);
  void lowerIntrinsic(HighFunc &Func, const MedBlock &CurBlock,
                      const MedOp &CurOp, size_t OpIdx,
                      std::set<size_t> &IntrinsicSkip);
  void lowerCallInd(HighFunc &Func, const MedBlock &CurBlock,
                    const MedOp &CurOp);
  void lowerCBranch(HighFunc &Func, const MedOp &CurOp);
  void lowerBranch(HighFunc &Func, const MedOp &CurOp);
  void lowerBranchInd(HighFunc &Func, const MedBlock &CurBlock,
                      const MedOp &CurOp, const MedFunc &Med);
  bool lowerSwitchFromJumpTable(HighFunc &Func, const MedBlock &CurBlock,
                                const MedOp &CurOp, const MedFunc &Med,
                                const JumpTable &JT);
  void lowerReturn(HighFunc &Func, const MedBlock &CurBlock, const MedOp &CurOp,
                   const MedFunc &Med);
  void lowerGenericAssign(HighFunc &Func, const MedOp &CurOp,
                          const VarKeySet &PhiArgVars);
  void insertPhiCopies(
      HighFunc &Func, const MedBlock &CurBlock, int BlkIdx, size_t BlkBodyStart,
      const std::map<std::pair<int, int>,
                     std::vector<std::pair<MedVar, MedVar>>> &PhiCopies);

  CallIndTarget resolveCallIndTarget(const MedBlock &CurBlock,
                                     const MedOp &CurOp,
                                     const ExprPtr &TargetExpr);

  VarKeyMap<int> UseCount;
  TypeRef sourceCallResultType(const MedOp &Op) const;
  VarKeyMap<ExprPtr> DefExpr;
  VarKeyMap<TypeRef> SourceRecordValues;
  std::vector<SourceABIParameter> SourceParameters;
  VarKeySet CallOutputs;
  VarKeySet PhiOutputVars;
  /// A native read is evaluated at its statement, then used as an SSA value.
  /// Re-expanding it at a use could cross an aliasing write or repeat the read.
  VarKeySet MemoryReadOutputs;
  /// The function currently being converted; set at the top of convert() so
  /// collectCallArgs can resolve a register argument that is live-in to the
  /// call block (loop-carried via a header PHI) rather than written before the
  /// call.
  const MedFunc *CurMed = nullptr;
  const BinaryImage *Image = nullptr;
  Arch TargetArch = Arch::Unknown;
  const std::map<va_t, std::string> *FuncNames = nullptr;
  std::vector<JumpTable> JumpTables;
  int NextHighTempId = 0;
  int ExprRecurseDepth = 0;
  static constexpr int kMaxExprDepth = limits::kMaxExprDepth;
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_MEDTOHIGH_H
