//===- LowScalarExpression.cpp - Bounded scalar identities ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse scalar LowIR semantics and the symbolic engine for control targets.

#include "LowScalarExpression.h"

#include "LowScalarSymbolic.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymState.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <utility>

namespace neverd {

symbolic::SymRef symbolicScalarExpression(
    symbolic::SymContext &Ctx, llvm::ArrayRef<LowOp> Operations,
    const NdVar &Value, va_t QueryInstruction, size_t &RemainingWork,
    llvm::function_ref<std::optional<uint64_t>(const LowOp &, uint64_t)>
        ReadImage) {
  const auto Scalar = [](const NdVar &V) {
    return V.Size && V.Size <= sizeof(uint64_t) &&
           (V.isConst() || V.isReg() || V.isTemp()) &&
           (!V.isReg() || V.Offset <= UINT64_MAX - V.Size);
  };
  if (!Scalar(Value) ||
      Operations.size() > limits::kMaxScalarBranchSymbolicOperations)
    return {};
  auto Spend = [&](size_t Work) {
    if (Work > RemainingWork)
      return false;
    RemainingWork -= Work;
    return true;
  };
  if (!Spend(Operations.size()))
    return {};
  symbolic::SymState State(Ctx);
  std::map<uint64_t, std::pair<uint16_t, symbolic::SymRef>> Temps;
  va_t Instruction = InvalidVA;
  auto Read = [&](const NdVar &V) -> symbolic::SymRef {
    if (!Scalar(V))
      return {};
    if (V.isConst())
      return Ctx.mkConst(V.Size * 8, V.Offset);
    if (V.isReg())
      return State.read(symbolic::SymSpace::Register, V.Offset, V.Size);
    auto &Slot = Temps[V.Offset];
    if (Slot.first != V.Size)
      Slot = {V.Size, Ctx.mkFreshVar(V.Size * 8, "temporary")};
    return Slot.second;
  };
  for (const LowOp &Op : Operations) {
    if (Op.NumInputs > std::size(Op.Inputs))
      return {};
    if (Op.Addr != Instruction) {
      Temps.clear();
      Instruction = Op.Addr;
    }
    if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
        Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN)
      return {};
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::ATOMIC_XCHG ||
        Op.Opcode == NdOp::ATOMIC_ADD || Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
      State.clobberRegistersExcept({});
      Temps.clear();
      continue;
    }
    if (!Op.Output.Size || (!Op.Output.isReg() && !Op.Output.isTemp()))
      continue;
    if (!Scalar(Op.Output))
      return {};
    const size_t Before = Ctx.numNodes();
    std::optional<symbolic::SymRef> Result;
    if (ReadImage && Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
        Scalar(Op.Inputs[0]) && Op.MemoryOrdering == NdMemoryOrdering::None &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (const auto Address = Ctx.asConst(Read(Op.Inputs[0])))
        if (const auto Bits = ReadImage(Op, Address->getZExtValue()))
          Result = Ctx.mkConst(Op.Output.Size * 8, *Bits);
    // The scalar evaluator owns the accepted arities and widths. Supplying
    // zero here asks only whether the operation has that total scalar shape;
    // these test values are never inserted into the symbolic state.
    const bool Supported =
        Op.MemoryOrdering == NdMemoryOrdering::None &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        std::all_of(Op.Inputs, Op.Inputs + Op.NumInputs, Scalar) &&
        lowScalarConstantResult(Op, [](const NdVar &) {
          return std::optional<uint64_t>{0};
        }).has_value();
    if (Supported) {
      llvm::SmallVector<symbolic::SymRef, 2> Inputs;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Inputs.push_back(Read(Op.Inputs[I]));
      // Repeated products of two symbolic sums can expand exponentially
      // before a post-construction node check. This fallback needs only
      // linear scalar multiplication; nonlinear results remain fresh inputs.
      if (Op.Opcode != NdOp::INT_MULT || Ctx.isConst(Inputs[0]) ||
          Ctx.isConst(Inputs[1]))
        Result = symbolizeJumpTableIntegerOperation(Ctx, Op.Opcode,
                                                    Op.Output.Size, Inputs);
    }
    // Every unresolved load or scalar definition is a distinct observation.
    // Equal addresses alone cannot make two unproved loads cancel.
    const auto Bits = Result.value_or(symbolic::SymRef{});
    const auto Written =
        Bits ? Bits : Ctx.mkFreshVar(Op.Output.Size * 8, "unknown-write");
    if (Op.Output.isReg())
      State.write(symbolic::SymSpace::Register, Op.Output.Offset, Written);
    else
      Temps[Op.Output.Offset] = {Op.Output.Size, Written};
    if (Ctx.numNodes() > limits::kMaxScalarBranchSymbolicNodes ||
        !Spend(1 + Op.Output.Size + Ctx.numNodes() - Before))
      return {};
  }
  if (Instruction != QueryInstruction)
    Temps.clear();
  const size_t BeforeQuery = Ctx.numNodes();
  auto Expression = Read(Value);
  if (!Expression)
    return {};
  if (!Spend(1 + Value.Size + Ctx.numNodes() - BeforeQuery) ||
      Ctx.numNodes() > limits::kMaxScalarBranchSymbolicNodes)
    return {};
  return Expression;
}

std::optional<uint64_t> constantScalarExpression(
    llvm::ArrayRef<LowOp> Operations, const NdVar &Value, va_t QueryInstruction,
    size_t &RemainingWork,
    llvm::function_ref<std::optional<uint64_t>(const LowOp &, uint64_t)>
        ReadImage) {
  symbolic::SymContext Ctx;
  const auto Expression = symbolicScalarExpression(
      Ctx, Operations, Value, QueryInstruction, RemainingWork, ReadImage);
  if (!Expression)
    return {};
  if (const auto Constant = Ctx.asConst(Expression))
    return Constant->getZExtValue();
  symbolic::MBAOptions Options;
  Options.MaxAtoms = limits::kMaxScalarBranchSymbolicAtoms;
  Options.MaxSynthesisAtoms = limits::kMaxScalarBranchSymbolicAtoms;
  Options.MaxOptimalSynthesisAtoms =
      limits::kMaxScalarBranchOptimalSynthesisAtoms;
  Options.MaxWork =
      std::min(RemainingWork, limits::kMaxScalarBranchSymbolicRewriteWork);
  Options.MaxTableBytes = limits::kMaxScalarBranchSymbolicTableBytes;
  const auto Simplified = symbolic::simplifyMBADeep(Ctx, Expression, Options);
  if (Simplified.Work > RemainingWork)
    return {};
  RemainingWork -= Simplified.Work;
  if (Simplified.Evidence != symbolic::MBAEvidence::Derivation ||
      Simplified.Outcome == symbolic::MBAOutcome::BudgetExhausted)
    return {};
  const auto Constant = Ctx.asConst(Simplified.Expr);
  return Constant ? std::optional<uint64_t>{Constant->getZExtValue()}
                  : std::nullopt;
}

} // namespace neverd
