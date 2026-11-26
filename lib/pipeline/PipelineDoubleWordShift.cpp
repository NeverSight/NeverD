//===- PipelineDoubleWordShift.cpp - Register-pair shift evidence ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prove the output bits of an unnamed, two-word shift without a caller.
///
//===----------------------------------------------------------------------===//

#include "PipelineWideIntReturnDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowAlignmentGuard.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/solver/BitBlaster.h"
#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatSolver.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>

namespace neverd {

bool returnsDoubleWordShift(const LowFunc &Low, const MedFunc &Med,
                            const TargetRegInfo &TRI, llvm::endianness Order) {
  using namespace symbolic;
  if (Low.Blocks.empty() || Low.Blocks.front().Ops.empty() ||
      Low.AnalysisOnly || !Low.UnprovenReturnAddresses.empty() ||
      Med.SkippedSSA || Med.SourceTypeHint || Med.SourceParametersBound ||
      !Med.MultiReturn.empty())
    return false;
  const unsigned Word = TRI.integerReturnWordBytes(), Bits = 8 * Word;
  const LowOp &First = Low.Blocks.front().Ops.front();
  const NdVar Amount = NdVar::reg(TRI.IntParamRegs[2], Word);
  // The word-selection test is a cheap filter. Only the pair proof below
  // grants a return width.
  if (First.Opcode != NdOp::INT_AND || First.Inputs[0] != Amount ||
      First.Inputs[1] != NdVar::scalar(Bits, Word))
    return false;
  if (llvm::any_of(Low.ModuleAnalysisRoots,
                   [&](va_t Root) { return Root != Low.Entry; }))
    return false;
  unsigned Branches = 0;
  for (const LowBlock &Block : Low.Blocks) {
    if (!Block.ExceptionalSuccs.empty())
      return false;
    for (const LowOp &Op : Block.Ops) {
      if (Op.Opcode == NdOp::COND_BR)
        ++Branches;
      else if (Op.Opcode != NdOp::RETURN && Op.Opcode != NdOp::BRANCH &&
               !isSourceFrameScalarTransfer(Op.Opcode) &&
               !isDefaultAddressAlignmentGuard(Op))
        return false;
    }
  }
  // The helper splits on the word boundary and the identity case.
  if (Branches != 2)
    return false;

  SymContext Ctx;
  ExploreOptions Options;
  Options.ByteOrder = Order;
  Options.MaxPaths = Branches + 1;
  Options.MaxBlockVisits = 1;
  SymState Entry(Ctx, Order);
  const auto Pair = [&](SymState &State) {
    SymRef First = State.read(SymSpace::Register, TRI.IntReturnReg, Word);
    SymRef Second = State.read(SymSpace::Register, TRI.IntReturnReg2, Word);
    return TRI.IntegerReturnPairHighFirst ? Ctx.mkConcat(First, Second)
                                          : Ctx.mkConcat(Second, First);
  };
  const SymRef Input = Pair(Entry);
  const SymRef Count = Entry.read(SymSpace::Register, Amount.Offset, Word);
  auto Walk = explorePathsDetailed(Ctx, Low, Options);
  if (!Walk.Complete || Walk.UnmodelledOps || Walk.Paths.empty())
    return false;

  solver::SatSolver Solver;
  solver::CnfEncoder Encoder(Solver);
  solver::BitBlaster Blaster(Ctx, Encoder);
  solver::BitLits InputBits;
  if (!Blaster.blast(Input, InputBits))
    return false;
  const unsigned Width = InputBits.size();
  bool Left = true, LogicalRight = true, ArithmeticRight = true;
  for (SymPath &Path : Walk.Paths) {
    if (Path.Outcome != PathOutcome::Returned)
      return false;
    // Prove the returned words for all inputs on this branch, including the
    // superset obtained by dropping a return-address alignment precondition.
    // The guard itself remains in the reconstructed function.
    SymRef Guard = Ctx.mkTrue();
    for (const SymBranchDecision &Decision : Path.BranchDecisions)
      Guard = Ctx.mkAnd(Guard, Decision.Taken ? Decision.Condition
                                              : Ctx.mkNot(Decision.Condition));
    const SymRef Result = Pair(Path.State);
    SymEvalPlan Predicate(Ctx, Guard);
    if (!Predicate.fitsU64() ||
        llvm::any_of(Predicate.vars(),
                     [&](uint32_t Id) { return Id != Ctx.varId(Count); }))
      return false;
    bool Reached = false;
    for (unsigned N = 0; N != Width; ++N) {
      if (!Predicate.evalU64With([&](uint32_t) { return N; }))
        continue;
      Reached = true;
      // Fix only the shift count. Each result bit must be the exact incoming
      // bit required by one shift, or its specified fill. Circuit literals
      // compare all input and scratch values without a SAT search.
      auto Shifted =
          Ctx.substitute(Result, {{Count.index(), Ctx.mkConst(Bits, N)}});
      solver::BitLits Actual;
      if (!Blaster.blast(Shifted, Actual))
        return false;
      for (unsigned I = 0; I != Width; ++I) {
        Left &= Actual[I] == (I < N ? Encoder.falseLit() : InputBits[I - N]);
        LogicalRight &= Actual[I] ==
                        (I + N < Width ? InputBits[I + N] : Encoder.falseLit());
        ArithmeticRight &= Actual[I] == InputBits[std::min(I + N, Width - 1)];
      }
      if (!Left && !LogicalRight && !ArithmeticRight)
        return false;
    }
    if (!Reached)
      return false;
  }
  return true;
}

} // namespace neverd
