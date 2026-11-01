//===- X64StackProbeAllocations.cpp - Probed x64 PE allocations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A probe removed by fresh source conversion still constrains the allocation
/// after its normal return. Keep the fact at that exact subtraction, never at
/// the incoming size: another path or an earlier use may not have probed it.
///
//===----------------------------------------------------------------------===//

#include "X64StackProbeAllocations.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFStackProbe.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <set>
#include <utility>

namespace neverd::x64_stack_probe {

std::vector<ProvenStackOffsetRange>
probedStackAllocations(const MedFunc &Func, const LowFunc &Source,
                       const BinaryImage &Image, size_t &Remaining) {
  if (Image.Arch != Arch::X64 || Image.Format != BinaryFormat::COFF ||
      Func.Entry != Source.Entry || Func.SkippedSSA || Func.AnalystStackFrame)
    return {};
  const auto Charge = [&](size_t Count) {
    if (Count > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Count;
    return true;
  };
  using Site = std::pair<va_t, int>;
  std::set<Site> Allocations;
  const NdVar SP = NdVar::reg(x86reg::RSP, 8);
  const NdVar AX = NdVar::reg(x86reg::RAX, 8);
  for (const LowBlock &Block : Source.Blocks) {
    if (!Charge(1 + Block.Ops.size() + Block.InstructionBoundaries.size()))
      return {};
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      continue;
    if (auto Error = validateLowInstructionBoundaries(
            Block, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      continue;
    }
    bool Probed = false;
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (hasLowInstructionControlFlag(
              Boundary.ControlFlags,
              LowInstructionControlFlag::InstructionGuard)) {
        Probed = false;
        continue;
      }
      if (Source.ModuleAnalysisRoots.count(Boundary.Address) ||
          Boundary.Control != LowInstructionControl::None)
        Probed = false;
      for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
           ++I) {
        const LowOp &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::CALL) {
          const auto Probe =
              Boundary.Control == LowInstructionControl::Call && Op.NumInputs &&
                      Op.Inputs[0].isConst()
                  ? coff_loader::getCheckedX64StackProbeCall(Image, Op.Addr)
                  : std::nullopt;
          Probed = Probe && Probe->Helper.Begin == Op.Inputs[0].Offset &&
                   libc::stackProbeEffect(Image, Op.Inputs[0].Offset) ==
                       libc::StackProbeEffect::Probe;
          continue;
        }
        if (Probed && Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2 &&
            Op.Output == SP && Op.Inputs[0] == SP && Op.Inputs[1] == AX)
          Allocations.emplace(Op.Addr, Op.Seq);
        // A partial write invalidates the complete probed size too. Calls
        // other than the authenticated probe, and opaque effects, supply no
        // register-preservation evidence here.
        if (Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INTRINSIC ||
            (Op.Output.isReg() && Op.Output.Size &&
             Op.Output.Offset < x86reg::RAX + 8 &&
             Op.Output.Offset + Op.Output.Size > x86reg::RAX))
          Probed = false;
      }
    }
  }
  if (Allocations.empty())
    return {};
  std::vector<const MedOp *> Definitions;
  std::vector<MedVar> Bases;
  const auto IsRegister = [](const MedVar &V, uint64_t Offset) {
    return V.Kind == MedVar::Reg && V.RegOff == Offset && V.Size == 8;
  };
  for (const MedBlock &Block : Func.Blocks) {
    if (!Charge(1 + Block.Ops.size()))
      return {};
    // Fresh SSA conversion may name the probed RAX value with a temporary.
    // Its native identity is proved above; match the exact subtraction and
    // its width, not the temporary's register spelling.
    for (const MedOp &Op : Block.Ops)
      if (!Op.Dead && Allocations.count({Op.Addr, Op.OriginSeq}) &&
          Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2 &&
          IsRegister(Op.Output, x86reg::RSP) &&
          IsRegister(Op.Inputs[0], x86reg::RSP) && Op.Inputs[1].Size == 8) {
        Definitions.push_back(&Op);
        Bases.push_back(Op.Inputs[0]);
      }
  }
  const auto Offsets = entryStackOffsets(Func, Bases, Image.Arch,
                                         Image.abiFormat(), Func.EntryKind);
  std::vector<ProvenStackOffsetRange> Result;
  constexpr int64_t Maximum = coff_loader::X64StackProbeCall::MaximumAllocation;
  for (size_t I = 0; I < Offsets.size(); ++I)
    if (Offsets[I] && *Offsets[I] >= INT64_MIN + Maximum)
      Result.push_back(
          {Definitions[I], nullptr, {*Offsets[I] - Maximum, *Offsets[I]}});
  return Result;
}

} // namespace neverd::x64_stack_probe
