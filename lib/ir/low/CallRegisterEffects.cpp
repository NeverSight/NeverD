//===- CallRegisterEffects.cpp - Callee GPR write summaries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CallRegisterEffects.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd {

namespace {
constexpr uint64_t kX64GPRBytes = 16 * 8;
constexpr unsigned kX64StackPointerFamily = 4; // RSP
} // namespace

std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff) {
  if (A != Arch::X64 || RegOff >= kX64GPRBytes)
    return std::nullopt;
  return static_cast<unsigned>(RegOff / 8);
}

LocalRegisterEffect localRegisterEffect(const BinaryImage &Img,
                                        const LowFunc &F) {
  LocalRegisterEffect Effect;
  if (!F.hasCompleteLiftCoverage() ||
      !F.UnsafeIndirectBranchAddresses.empty()) {
    Effect.Unknown = true;
    return Effect;
  }
  std::set<va_t> BlockStarts;
  for (const LowBlock &Block : F.Blocks)
    BlockStarts.insert(Block.StartAddr);
  for (const LowBlock &Block : F.Blocks) {
    // A call that never returns cannot change a register its caller reads.
    auto NoReturnCall = [&](size_t OpIndex) {
      for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
        if (OpIndex >= Boundary.FirstOp &&
            OpIndex - Boundary.FirstOp < Boundary.OpCount)
          return hasLowInstructionControlFlag(
              Boundary.ControlFlags, LowInstructionControlFlag::NoReturn);
      return false;
    };
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      if (Op.Output.isReg())
        if (auto Family = gprFamilyOf(Img.Arch, Op.Output.Offset);
            Family && *Family != kX64StackPointerFamily)
          Effect.Writes |= GPRFamilyMask(1) << *Family;
      switch (Op.Opcode) {
      case NdOp::INDIR_CALL:
        Effect.Unknown = true;
        break;
      case NdOp::INDIR_BR:
        // A resolved jump table has successors; an indirect tail jump leaves
        // for code this summary cannot see.
        if (Block.Succs.empty())
          Effect.Unknown = true;
        break;
      case NdOp::BRANCH:
      case NdOp::COND_BR:
        // A direct branch to another function's entry leaves this body the
        // way a tail call does; the CFG keeps no block for it, so its writes
        // are that function's.
        if (Op.NumInputs > 0 && Op.Inputs[0].isConst() &&
            !BlockStarts.count(Op.Inputs[0].Offset))
          Effect.Callees.insert(Op.Inputs[0].Offset);
        break;
      case NdOp::CALL:
        if (NoReturnCall(I))
          break;
        if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
            Img.findImportAt(Op.Inputs[0].Offset))
          Effect.Unknown = true;
        else
          Effect.Callees.insert(Op.Inputs[0].Offset);
        break;
      default:
        break;
      }
    }
  }
  return Effect;
}

std::map<va_t, GPRFamilyMask>
solveCallRegisterEffects(const std::map<va_t, LocalRegisterEffect> &Funcs) {
  // Unknown is absorbing: a function is unknown if it or any callee is.
  std::set<va_t> Unknown;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect.Unknown)
      Unknown.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (Unknown.count(Entry))
        continue;
      for (va_t Callee : Effect.Callees)
        if (!Funcs.count(Callee) || Unknown.count(Callee)) {
          Unknown.insert(Entry);
          Changed = true;
          break;
        }
    }
  }

  // Least fixed point of Writes(F) = Local(F) | Writes(callees); recursion
  // converges because masks only grow.
  std::map<va_t, GPRFamilyMask> Writes;
  for (const auto &[Entry, Effect] : Funcs)
    if (!Unknown.count(Entry))
      Writes[Entry] = Effect.Writes;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Mask] : Writes) {
      GPRFamilyMask Next = Mask;
      for (va_t Callee : Funcs.at(Entry).Callees)
        Next |= Writes.at(Callee);
      if (Next != Mask) {
        Mask = Next;
        Changed = true;
      }
    }
  }
  return Writes;
}

} // namespace neverd
