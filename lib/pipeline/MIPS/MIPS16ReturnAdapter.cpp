//===- MIPS16ReturnAdapter.cpp - Source FP return helpers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Expand proven runtime result transfers before scalar FP admission and SSA.
//===----------------------------------------------------------------------===//

#include "MIPS16ReturnAdapter.h"

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/support/Parallel.h"

#include <algorithm>
#include <set>

namespace neverd::pipeline_detail {

std::map<va_t, MIPS16ReturnTransport>
mips16ReturnTransports(const BinaryImage &Image,
                       llvm::ArrayRef<LowFunc> Functions) {
  std::map<va_t, MIPS16ReturnTransport> Result;
  if (!isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16 || !Image.MIPS->hasScalarFPABI(false))
    return Result;
  std::set<va_t> Entries;
  for (const auto &Function : Functions) {
    if (Image.instructionModeAt(Function.Entry) != InstructionMode::MIPS16)
      continue;
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst())
          Entries.insert(Op.Inputs[0].Offset);
  }
  if (Entries.empty())
    return Result;
  const ExecutableCodeOwnerIndex Owners(Image);
  const std::vector<va_t> Targets(Entries.begin(), Entries.end());
  std::vector<std::optional<MIPS16ReturnTransport>> Proven(Targets.size());
  parallelForEach(Targets.size(), [&](auto Claim, size_t Count) {
    for (size_t I; (I = Claim()) < Count;)
      Proven[I] = proveMIPS16ReturnTransport(Image, Targets[I], &Owners);
  });
  for (size_t I = 0; I < Targets.size(); ++I)
    if (Proven[I])
      Result.emplace(Targets[I], std::move(*Proven[I]));
  return Result;
}

std::optional<LowFunc> projectMIPS16ReturnCalls(
    const BinaryImage &Image, const LowFunc &Source,
    const std::map<va_t, MIPS16ReturnTransport> &Transports) {
  if (Transports.empty() ||
      Image.instructionModeAt(Source.Entry) != InstructionMode::MIPS16)
    return std::nullopt;
  struct Site {
    size_t Block, Boundary;
    const MIPS16ReturnTransport *Transport;
  };
  std::vector<Site> Sites;
  for (size_t B = 0; B < Source.Blocks.size(); ++B) {
    const auto &Block = Source.Blocks[B];
    for (size_t I = 0; I < Block.InstructionBoundaries.size(); ++I) {
      const auto &Boundary = Block.InstructionBoundaries[I];
      if (Boundary.Control != LowInstructionControl::Call || !Boundary.OpCount)
        continue;
      const auto &Call = Block.Ops[Boundary.FirstOp + Boundary.OpCount - 1];
      if (Call.Opcode != NdOp::CALL || !Call.NumInputs ||
          !Call.Inputs[0].isConst())
        continue;
      const auto Found = Transports.find(Call.Inputs[0].Offset);
      if (Found != Transports.end())
        Sites.push_back({B, I, &Found->second});
    }
  }
  if (Sites.empty())
    return std::nullopt;
  uint64_t NextTemporary = TmpBase;
  const auto Reserve = [&](const NdVar &Value) {
    if (Value.isTemp())
      NextTemporary = std::max(NextTemporary, Value.Offset + Value.Size);
  };
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      Reserve(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Reserve(Op.Inputs[I]);
    }
  LowFunc Projected = Source;
  // Later slices first: each replacement preserves all original indexes
  // still needed by earlier sites in this block.
  for (auto I = Sites.rbegin(); I != Sites.rend(); ++I) {
    auto &Block = Projected.Blocks[I->Block];
    auto &Boundary = Block.InstructionBoundaries[I->Boundary];
    const size_t At = Boundary.FirstOp + Boundary.OpCount - 1;
    const LowOp Call = Block.Ops[At];
    std::vector<LowOp> Effects = I->Transport->Effects;
    std::map<std::pair<va_t, uint64_t>, uint64_t> Temporaries;
    int Seq = Call.Seq;
    for (auto &Op : Effects) {
      const auto Remap = [&](NdVar &Value) {
        if (!Value.isTemp())
          return;
        const auto [It, Added] = Temporaries.try_emplace(
            std::pair{Op.Addr, Value.Offset}, NextTemporary);
        if (Added)
          NextTemporary += mipsreg::VectorBytes;
        Value.Offset = It->second;
      };
      Remap(Op.Output);
      for (unsigned K = 0; K < Op.NumInputs; ++K)
        Remap(Op.Inputs[K]);
      Op.Addr = Call.Addr;
      Op.Seq = Seq++;
    }
    Block.Ops.erase(Block.Ops.begin() + At);
    Block.Ops.insert(Block.Ops.begin() + At, Effects.begin(), Effects.end());
    const size_t Added = Effects.size() - 1;
    Boundary.OpCount += Added;
    Boundary.Control = LowInstructionControl::None;
    Boundary.ControlFlags = LowInstructionControlFlag::None;
    Boundary.Immediate.reset();
    Boundary.TargetMode = LowInstructionTargetMode::Preserve;
    for (size_t J = I->Boundary + 1; J < Block.InstructionBoundaries.size();
         ++J)
      Block.InstructionBoundaries[J].FirstOp += Added;
  }
  return Projected;
}

} // namespace neverd::pipeline_detail
