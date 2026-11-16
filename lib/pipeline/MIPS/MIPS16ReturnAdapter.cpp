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
    if (Image.instructionModeAt(Function.Entry) != InstructionMode::MIPS16) {
      // A selected helper needs the same byte proof even without a selected
      // caller. Its incoming v0/v1 words are not ordinary C parameters.
      // Only a first instruction unit writing an FPR can match this transport;
      // avoid decoding every classic body again during source admission.
      if (!Function.Blocks.empty() &&
          !Function.Blocks.front().InstructionBoundaries.empty()) {
        const auto &Block = Function.Blocks.front();
        const auto &First = Block.InstructionBoundaries.front();
        for (size_t I = First.FirstOp; I < First.FirstOp + First.OpCount; ++I)
          if (Block.Ops[I].Output.isReg() &&
              mipsreg::isVector(Block.Ops[I].Output.Offset)) {
            Entries.insert(Function.Entry);
            break;
          }
      }
      continue;
    }
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
  for (const auto &Temporary : Source.FunctionTemporaries)
    NextTemporary = std::max(NextTemporary, Temporary.Offset + Temporary.Bytes);
  LowFunc Projected = Source;
  size_t NextSite = 0;
  while (NextSite < Sites.size()) {
    const size_t FirstSite = NextSite;
    const size_t B = Sites[NextSite].Block;
    size_t Extra = 0;
    while (NextSite < Sites.size() && Sites[NextSite].Block == B) {
      Extra += Sites[NextSite].Transport->Effects.size() - 1;
      ++NextSite;
    }
    const auto &Original = Source.Blocks[B];
    auto &Block = Projected.Blocks[B];
    Block.Ops.clear();
    Block.Ops.reserve(Original.Ops.size() + Extra);
    size_t Site = FirstSite;
    for (size_t I = 0; I < Block.InstructionBoundaries.size(); ++I) {
      const auto &Before = Original.InstructionBoundaries[I];
      auto &Boundary = Block.InstructionBoundaries[I];
      Boundary.FirstOp = Block.Ops.size();
      const size_t End = Before.FirstOp + Before.OpCount;
      if (Site == NextSite || Sites[Site].Boundary != I) {
        Block.Ops.insert(Block.Ops.end(), Original.Ops.begin() + Before.FirstOp,
                         Original.Ops.begin() + End);
        continue;
      }
      Block.Ops.insert(Block.Ops.end(), Original.Ops.begin() + Before.FirstOp,
                       Original.Ops.begin() + End - 1);
      const LowOp &Call = Original.Ops[End - 1];
      std::map<std::pair<va_t, uint64_t>, uint64_t> Temporaries;
      int Seq = Call.Seq;
      for (LowOp Op : Sites[Site++].Transport->Effects) {
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
        Block.Ops.push_back(std::move(Op));
      }
      Boundary.OpCount = Block.Ops.size() - Boundary.FirstOp;
      Boundary.Control = LowInstructionControl::None;
      Boundary.ControlFlags = LowInstructionControlFlag::None;
      Boundary.Immediate.reset();
      Boundary.TargetMode = LowInstructionTargetMode::Preserve;
    }
  }
  return Projected;
}

} // namespace neverd::pipeline_detail
