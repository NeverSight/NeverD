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
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include <set>

namespace neverd {

Intrinsic intrinsicId(const MedOp &Op);
std::string intrinsicName(const MedOp &Op);
uint16_t inferReturnSize(const MedFunc &Med);
std::set<uint64_t> detectPtrParamRegs(const MedFunc &Med);

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

class MedToHighConverter {
public:
  HighFunc convert(const MedFunc &Med, Arch TheArch = Arch::Unknown);

  void setFuncNames(const std::map<va_t, std::string> *Names) {
    FuncNames = Names;
  }
  void setJumpTables(const std::vector<JumpTable> &JTs) { JumpTables = JTs; }

  struct CallIndTarget {
    std::string Name = "indirect";
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
  void stripStackCanary(HighFunc &Func);
  void stripPrologueEpilogue(HighFunc &Func);
  void ensureTrailingReturn(HighFunc &Func, const MedFunc &Med);

  ExprPtr medOpToExpr(const MedOp &Op);
  ExprPtr medvarToExpr(const MedVar &V);
  ExprPtr forceInlineExpr(const ExprPtr &E);

  int regToArgIdx(uint64_t RegOff) const;

  std::vector<ExprPtr> collectCallArgs(const MedBlock &CurBlock,
                                       size_t CallIdx);

  /// Resolve the SSA variable of register \p RegOff reaching the ENTRY of
  /// \p B (its live-in value: a PHI in B, else the single reaching definition
  /// walked back through predecessors).  Used to recover a register call
  /// argument that is live-in to the call block rather than written before the
  /// call.  Returns false when unresolved.  Requires CurMed.
  bool reachingRegAtBlockEntry(const MedBlock &B, uint64_t RegOff,
                               MedVar &Out) const;

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
      const std::map<int, std::vector<std::pair<MedVar, MedVar>>> &PhiCopies);

  CallIndTarget resolveCallIndTarget(const MedBlock &CurBlock,
                                     const MedOp &CurOp,
                                     const ExprPtr &TargetExpr);

  VarKeyMap<int> UseCount;
  VarKeyMap<ExprPtr> DefExpr;
  VarKeySet CallOutputs;
  VarKeySet PhiOutputVars;
  /// The function currently being converted; set at the top of convert() so
  /// collectCallArgs can resolve a register argument that is live-in to the
  /// call block (loop-carried via a header PHI) rather than written before the
  /// call.
  const MedFunc *CurMed = nullptr;
  Arch TargetArch = Arch::Unknown;
  const std::map<va_t, std::string> *FuncNames = nullptr;
  std::vector<JumpTable> JumpTables;
  std::set<int> JtConsumedBlocks;
  int ExprRecurseDepth = 0;
  static constexpr int kMaxExprDepth = limits::kMaxExprDepth;
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_MEDTOHIGH_H
