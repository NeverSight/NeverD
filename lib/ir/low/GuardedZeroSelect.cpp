//===- GuardedZeroSelect.cpp - Selects a zero test has decided ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/GuardedZeroSelect.h"

#include "neverd/ir/Limits.h"

#include "llvm/ADT/STLExtras.h"

#include <limits>
#include <map>
#include <optional>

namespace neverd {

namespace {

bool same(const NdVar &A, const NdVar &B) {
  return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
}

bool overlaps(const NdVar &A, const NdVar &B) {
  return A.Space == B.Space && A.Size && B.Size &&
         A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
}

bool isZero(const NdVar &V) { return V.isConst() && V.Offset == 0; }

/// Whether \p Op may change register \p Reg: a write to any of its bytes, or
/// a call, which changes registers it does not list.
bool mayWrite(const LowOp &Op, const NdVar &Reg) {
  return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
         overlaps(Op.Output, Reg);
}

/// The index of the op that last wrote \p Var before op \p End of \p Block,
/// following a temporary only within instruction \p Instruction.  None when
/// another write to its bytes comes first, or none is found.
std::optional<size_t> lastWrite(const LowBlock &Block, size_t End,
                                const NdVar &Var, va_t Instruction) {
  for (size_t K = End; K-- > 0;) {
    const LowOp &Op = Block.Ops[K];
    if (Var.isTemp() && Op.Addr != Instruction)
      return std::nullopt;
    if (mayWrite(Op, Var))
      return same(Op.Output, Var) ? std::optional<size_t>(K) : std::nullopt;
  }
  return std::nullopt;
}

/// A condition that holds exactly when register \p Reg is zero, or exactly
/// when it is not, as op \p ReadAt reads the register.
struct ZeroTest {
  NdVar Reg;
  bool TrueWhenZero = true;
  size_t ReadAt = 0;
};

/// The zero test that \p Cond, as op \p End of \p Block reads it, is:
/// `x == 0` or `x != 0` of a register, through copies and negations, the
/// register perhaps tested against itself (`test x, x`).
std::optional<ZeroTest> zeroTestOf(const LowBlock &Block, size_t End,
                                   NdVar Cond) {
  bool TrueWhenZero = true;
  va_t Instruction = Block.Ops[End].Addr;
  for (int Depth = 0;
       Depth < ::neverd::limits::ir::kGuardedZeroSelectZeroTestOfDepthLimit;
       ++Depth) {
    const std::optional<size_t> At = lastWrite(Block, End, Cond, Instruction);
    if (!At)
      return std::nullopt;
    const LowOp &Def = Block.Ops[*At];
    End = *At;
    Instruction = Def.Addr;
    if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::BOOL_NOT) &&
        Def.NumInputs == 1) {
      if (Def.Opcode == NdOp::BOOL_NOT)
        TrueWhenZero = !TrueWhenZero;
      Cond = Def.Inputs[0];
      continue;
    }
    if ((Def.Opcode != NdOp::INT_EQUAL && Def.Opcode != NdOp::INT_NOTEQUAL) ||
        Def.NumInputs != 2)
      return std::nullopt;
    if (Def.Opcode == NdOp::INT_NOTEQUAL)
      TrueWhenZero = !TrueWhenZero;
    const NdVar Value = isZero(Def.Inputs[1])   ? Def.Inputs[0]
                        : isZero(Def.Inputs[0]) ? Def.Inputs[1]
                                                : NdVar();
    if (Value.isReg())
      return ZeroTest{Value, TrueWhenZero, End};
    if (!Value.isTemp())
      return std::nullopt;
    // `test x, x` lifts to `t = x & x; zf = t == 0`.
    const std::optional<size_t> Masked =
        lastWrite(Block, End, Value, Instruction);
    if (!Masked)
      return std::nullopt;
    const LowOp &And = Block.Ops[*Masked];
    if (And.Opcode != NdOp::INT_AND || And.NumInputs != 2 ||
        !And.Inputs[0].isReg() || !same(And.Inputs[0], And.Inputs[1]))
      return std::nullopt;
    return ZeroTest{And.Inputs[0], TrueWhenZero, *Masked};
  }
  return std::nullopt;
}

} // namespace

unsigned foldGuardedZeroSelects(LowFunc &F) {
  // Every build gives its body here: most hold no select to fold.
  if (llvm::none_of(F.Blocks, [](const LowBlock &Block) {
        return llvm::any_of(Block.Ops, [](const LowOp &Op) {
          return Op.Opcode == NdOp::SELECT;
        });
      }))
    return 0;
  std::map<int, size_t> IndexOfId;
  for (size_t I = 0; I < F.Blocks.size(); ++I)
    IndexOfId[F.Blocks[I].Id] = I;
  std::vector<std::vector<size_t>> Preds(F.Blocks.size());
  for (size_t I = 0; I < F.Blocks.size(); ++I)
    for (int Succ : F.Blocks[I].Succs)
      if (const auto It = IndexOfId.find(Succ); It != IndexOfId.end())
        Preds[It->second].push_back(I);

  unsigned Folded = 0;
  for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
    LowBlock &Block = F.Blocks[BI];
    if (Preds[BI].size() != 1 || Preds[BI].front() == BI ||
        !Block.ExceptionalPreds.empty())
      continue;
    const LowBlock &Pred = F.Blocks[Preds[BI].front()];
    if (Pred.Ops.empty() || Pred.Succs.size() != 2 ||
        Pred.Succs[0] == Pred.Succs[1])
      continue;
    const LowOp &Branch = Pred.Ops.back();
    if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
        !Branch.Inputs[0].isConst())
      continue;
    const std::optional<ZeroTest> Test =
        zeroTestOf(Pred, Pred.Ops.size() - 1, Branch.Inputs[1]);
    if (!Test)
      continue;
    // The register keeps the value the branch tested.
    bool Kept = true;
    for (size_t K = Test->ReadAt + 1; K < Pred.Ops.size() && Kept; ++K)
      Kept = !mayWrite(Pred.Ops[K], Test->Reg);
    const bool Taken = Block.StartAddr == Branch.Inputs[0].Offset;
    // Only the edge on which the register is nonzero decides the selects.
    if (!Kept || Taken == Test->TrueWhenZero)
      continue;
    size_t FirstWrite = std::numeric_limits<size_t>::max();
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      LowOp &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::SELECT && Op.NumInputs == 3)
        if (const std::optional<ZeroTest> Selects =
                zeroTestOf(Block, I, Op.Inputs[0]);
            Selects && Selects->ReadAt < FirstWrite &&
            Selects->Reg.Space == Test->Reg.Space &&
            Selects->Reg.Offset == Test->Reg.Offset &&
            Selects->Reg.Size >= Test->Reg.Size) {
          // A nonzero register makes a wider view of it nonzero too.
          const NdVar Chosen =
              Selects->TrueWhenZero ? Op.Inputs[2] : Op.Inputs[1];
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 0;
          Op.addInput(Chosen);
          ++Folded;
        }
      if (FirstWrite == std::numeric_limits<size_t>::max() &&
          mayWrite(Op, Test->Reg))
        FirstWrite = I;
    }
  }
  return Folded;
}

} // namespace neverd
