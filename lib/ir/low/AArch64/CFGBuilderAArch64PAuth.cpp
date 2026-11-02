//===- CFGBuilderAArch64PAuth.cpp - PACM execution state ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve PACM using executed instruction boundaries and incoming CFG edges.
/// A byte preceding a branch target is not evidence that PACM executed. Mixed
/// ordinary edges carry a function temporary into SSA; unrelated instructions
/// consume the prefix even when their lifted body is empty.
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace neverd {
namespace {

bool isPACMMarker(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 &&
         Op.Inputs[0].isConst() &&
         Op.Inputs[0].Offset ==
             static_cast<uint64_t>(Intrinsic::A64_PACMPending);
}

struct PrefixConsumer {
  Intrinsic Id;
  NdVar SecondModifier;
};

std::optional<PrefixConsumer> consumer(uint32_t Word, va_t Address) {
  const auto PC = NdVar::cst(Address, kAArch64PAuthPointerBytes);
  const auto X16 = NdVar::reg(a64reg::X16, kAArch64PAuthPointerBytes);
  const auto X15 = NdVar::reg(a64reg::X15, kAArch64PAuthPointerBytes);
  switch (Word) {
#define NEVERD_A64_PAUTH_M_CONSUMER(Word, Id, Mod2)                            \
  case Word:                                                                   \
    return PrefixConsumer{Intrinsic::Id, Mod2};
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthM.def"
  default:
    return std::nullopt;
  }
}

bool endsInPACM(const LowBlock &Block) {
  if (Block.InstructionBoundaries.empty())
    return false;
  const auto &Last = Block.InstructionBoundaries.back();
  return Last.OpCount == 1 && isPACMMarker(Block.Ops[Last.FirstOp]);
}

} // namespace

void CFGBuilder::resolveAArch64PAuthPrefixes(LowFunc &Func) {
  if (!CurrentImg || CurrentImg->Arch != Arch::AArch64)
    return;
  // rebuildBlocks reconstructs the raw lifter's operations. Retire temporary
  // ranges belonging to the previous normalized graph at that same boundary.
  Func.FunctionTemporaries.clear();
  bool HasPrefix = false;
  for (const auto &Block : Func.Blocks)
    HasPrefix |= std::any_of(Block.Ops.begin(), Block.Ops.end(), isPACMMarker);
  if (!HasPrefix)
    return;
  if (auto Error = validateLowInstructionBoundaries(
          Func, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    UnsupportedInstructionAddresses.insert(Func.Entry);
    return;
  }

  struct Rewrite {
    int Block;
    size_t Index;
    PrefixConsumer Consumer;
    bool Mixed;
  };
  std::vector<Rewrite> Rewrites;
  bool NeedsMode = false;
  // Validate the complete transaction before removing a pending marker.
  for (const auto &Block : Func.Blocks) {
    bool Prefix = false;
    for (size_t I = 0; I < Block.InstructionBoundaries.size(); ++I) {
      const auto &Boundary = Block.InstructionBoundaries[I];
      const auto *Bytes = CurrentImg->readVA(Boundary.Address, Boundary.Size);
      if (!Bytes || Boundary.Size != branch::A64InstructionBytes) {
        UnsupportedInstructionAddresses.insert(Boundary.Address);
        return;
      }
      const auto Consumer = consumer(readLE<uint32_t>(Bytes), Boundary.Address);
      bool Mixed = false;
      if (!I && Consumer) {
        bool WithoutPrefix = Block.Preds.empty();
        for (int Pred : Block.Preds) {
          const auto &Previous = Func.Blocks[Pred];
          const bool HasPrefix =
              Previous.EndAddr == Block.StartAddr && endsInPACM(Previous);
          Prefix |= HasPrefix;
          WithoutPrefix |= !HasPrefix;
        }
        // An independently enterable body needs an implicit-entry SSA value.
        // Until that edge is explicit, never specialize it from predecessors.
        if (Prefix && (Block.StartAddr == Func.Entry ||
                       Func.ModuleAnalysisRoots.count(Block.StartAddr) ||
                       !Block.ExceptionalPreds.empty())) {
          UnsupportedInstructionAddresses.insert(Boundary.Address);
          return;
        }
        Mixed = Prefix && WithoutPrefix;
      }
      if (Prefix && Consumer) {
        if (!Boundary.OpCount ||
            !lowAArch64PAuthShapeIsValid(Block.Ops[Boundary.FirstOp])) {
          UnsupportedInstructionAddresses.insert(Boundary.Address);
          return;
        }
        Rewrites.push_back({Block.Id, static_cast<size_t>(Boundary.FirstOp),
                            *Consumer, Mixed});
        NeedsMode |= Mixed;
      }
      Prefix =
          Boundary.OpCount == 1 && isPACMMarker(Block.Ops[Boundary.FirstOp]);
    }
    if (Prefix &&
        (Block.Succs.size() != 1 ||
         Func.Blocks[Block.Succs.front()].StartAddr != Block.EndAddr)) {
      UnsupportedInstructionAddresses.insert(
          Block.InstructionBoundaries.back().Address);
      return;
    }
  }

  NdVar Mode;
  if (NeedsMode) {
    uint64_t Next = 0;
    const auto Reserve = [&](const NdVar &Value) {
      if (!Value.isTemp())
        return true;
      if (Value.Offset > UINT64_MAX - Value.Size)
        return false;
      Next = std::max(Next, Value.Offset + Value.Size);
      return true;
    };
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops) {
        bool Valid =
            Reserve(Op.Output) && Op.Seq < std::numeric_limits<int>::max();
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          Valid &= Reserve(Op.Inputs[I]);
        if (!Valid) {
          UnsupportedInstructionAddresses.insert(Op.Addr);
          return;
        }
      }
    if (Next > UINT64_MAX - kAArch64PAuthPointerBytes) {
      UnsupportedInstructionAddresses.insert(Func.Entry);
      return;
    }
    Mode = NdVar::tmp(Next, kAArch64PAuthPointerBytes);
    Func.FunctionTemporaries.push_back({Next, kAArch64PAuthPointerBytes});
  }
  for (const auto &Rewrite : Rewrites) {
    auto &Op = Func.Blocks[Rewrite.Block].Ops[Rewrite.Index];
    const auto Pointer = Op.Inputs[1];
    const auto Modifier = Op.Inputs[Op.NumInputs - 1];
    Op.Inputs[0].Offset = static_cast<uint64_t>(Rewrite.Consumer.Id);
    Op.Inputs[1] = Pointer;
    Op.Inputs[2] = Modifier;
    Op.Inputs[3] = Rewrite.Consumer.SecondModifier;
    Op.Inputs[kAArch64PACMOperandCount] =
        Rewrite.Mixed ? Mode : NdVar::cst(1, kAArch64PAuthPointerBytes);
    Op.NumInputs = kAArch64PACMOperandCount + 1;
  }

  for (auto &Block : Func.Blocks) {
    const bool OutgoingPrefix = endsInPACM(Block);
    std::vector<LowOp> Ops;
    for (auto &Boundary : Block.InstructionBoundaries) {
      const size_t Begin = Ops.size();
      int NextSeq = 0;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I) {
        const auto &Op = Block.Ops[I];
        if (NeedsMode)
          NextSeq = std::max(NextSeq, Op.Seq + 1);
        if (!isPACMMarker(Op))
          Ops.push_back(Op);
      }
      if (NeedsMode && &Boundary == &Block.InstructionBoundaries.back()) {
        LowOp Publish;
        Publish.Opcode = NdOp::COPY;
        Publish.Addr = Boundary.Address;
        Publish.Seq = NextSeq;
        Publish.Output = Mode;
        Publish.addInput(NdVar::cst(OutgoingPrefix, kAArch64PAuthPointerBytes));
        auto Position =
            std::find_if(Ops.begin() + Begin, Ops.end(), [](const LowOp &Op) {
              return Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
                     Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN;
            });
        Ops.insert(Position, Publish);
      }
      Boundary.FirstOp = Begin;
      Boundary.OpCount = Ops.size() - Begin;
    }
    Block.Ops = std::move(Ops);
  }
}

} // namespace neverd
