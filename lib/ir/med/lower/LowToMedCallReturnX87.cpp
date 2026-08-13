//===- LowToMedCallReturnX87.cpp - x87 call returns ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Models call returns carried on the x87 register stack.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>

namespace neverd {

void LowToMedConverter::modelCallX87Return(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);
  // Only x86/x86-64 have an x87 stack.  i386 cdecl returns floating point in
  // st0; x86-64 returns it in XMM0 (handled by modelCallFPReturn), so this pass
  // only fires when a callee actually leaves its result on the x87 stack — the
  // caller reads it back with `fstp [mem]`, a post-call read of an st register
  // the lifter never modeled the call as defining (so it folds to the stale
  // entry value, storing 0).
  if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
    return;

  int MaxVer = 0;
  for (const auto &B : Func.Blocks) {
    for (const auto &Op : B.Ops)
      MaxVer = std::max(MaxVer, Op.Output.SSAVer);
    for (const auto &Phi : B.Phis)
      MaxVer = std::max(MaxVer, Phi.Output.SSAVer);
  }
  int NextVer = MaxVer + 1;

  for (auto &Blk : Func.Blocks) {
    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;

      // The first post-call x87 read (the `fstp` of the FP return), before any
      // op redefines an x87 register.
      MedVar StRead;
      bool Found = false;
      for (size_t J = OI + 1; J < Blk.Ops.size() && !Found; ++J) {
        auto &Nx = Blk.Ops[J];
        for (uint8_t I = 0; I < Nx.NumInputs; ++I)
          if (Nx.Inputs[I].Kind == MedVar::Reg &&
              TRI.isX87StackReg(Nx.Inputs[I].RegOff)) {
            StRead = Nx.Inputs[I];
            Found = true;
            break;
          }
        if (Found)
          break;
        if (Nx.Output.Kind == MedVar::Reg &&
            TRI.isX87StackReg(Nx.Output.RegOff))
          break; // x87 redefined before any read
      }
      if (!Found)
        continue;

      int NewVer = NextVer++;
      MedVar Out;
      Out.Kind = MedVar::Reg;
      Out.Id = StRead.Id;
      Out.Size = StRead.Size;
      Out.RegOff = StRead.RegOff;
      Out.SSAVer = NewVer;
      Out.TheArch = TargetArch;
      Op.Output = Out;

      for (size_t J = OI + 1; J < Blk.Ops.size(); ++J) {
        auto &Nx = Blk.Ops[J];
        for (uint8_t I = 0; I < Nx.NumInputs; ++I)
          if (Nx.Inputs[I].Kind == MedVar::Reg &&
              Nx.Inputs[I].RegOff == StRead.RegOff &&
              Nx.Inputs[I].Id == StRead.Id)
            Nx.Inputs[I].SSAVer = NewVer;
        if (Nx.Output.Kind == MedVar::Reg && Nx.Output.RegOff == StRead.RegOff)
          break;
      }
    }
  }
}

} // namespace neverd
