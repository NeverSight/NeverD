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
#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/support/Parallel.h"

#include <algorithm>
#include <set>

namespace neverd::pipeline_detail {

std::map<va_t, MIPS16RuntimeTransport>
mips16RuntimeTransports(const BinaryImage &Image,
                        llvm::ArrayRef<LowFunc> Functions) {
  std::map<va_t, MIPS16RuntimeTransport> Result;
  if (!isMIPS(Image.Arch) || isMIPS64(Image.Arch) || !Image.MIPS ||
      !Image.MIPS->HasMIPS16 || !Image.MIPS->hasScalarFPABI(false))
    return Result;
  std::set<va_t> Entries;
  for (const auto &Function : Functions) {
    if (Image.instructionModeAt(Function.Entry) != InstructionMode::MIPS16) {
      // A selected helper needs the same byte proof even without a selected
      // caller. Its incoming v0/v1 words are not ordinary C parameters.
      // All qualified helpers start with a transfer to an FPR or save ra in s2.
      if (!Function.Blocks.empty() &&
          !Function.Blocks.front().InstructionBoundaries.empty()) {
        const auto &Block = Function.Blocks.front();
        const auto &First = Block.InstructionBoundaries.front();
        for (size_t I = First.FirstOp; I < First.FirstOp + First.OpCount; ++I)
          if (Block.Ops[I].Output.isReg() &&
              (mipsreg::isVector(Block.Ops[I].Output.Offset) ||
               Block.Ops[I].Output.Offset == mipsreg::r(18))) {
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
  std::vector<std::optional<MIPS16RuntimeTransport>> Proven(Targets.size());
  parallelForEach(Targets.size(), [&](auto Claim, size_t Count) {
    for (size_t I; (I = Claim()) < Count;) {
      if (auto Stub =
              proveMIPS16IndirectCallTransport(Image, Targets[I], &Owners))
        Proven[I] = MIPS16RuntimeTransport{
            std::move(Stub->Effects), Stub->FloatingPrefix, Stub->ResultBytes,
            Stub->ResultElements};
      else if (auto Return =
                   proveMIPS16ReturnTransport(Image, Targets[I], &Owners))
        Proven[I] = MIPS16RuntimeTransport{std::move(Return->Effects),
                                           {},
                                           Return->ElementBytes,
                                           Return->Elements};
    }
  });
  for (size_t I = 0; I < Targets.size(); ++I)
    if (Proven[I])
      Result.emplace(Targets[I], std::move(*Proven[I]));
  return Result;
}

std::optional<LowFunc> projectMIPS16RuntimeCalls(
    const BinaryImage &Image, const LowFunc &Source,
    const std::map<va_t, MIPS16RuntimeTransport> &Transports,
    const MIPS16CallSites *InputSites, MIPS16CallSites *ProjectedSites) {
  if (Transports.empty() ||
      Image.instructionModeAt(Source.Entry) != InstructionMode::MIPS16)
    return std::nullopt;
  struct Site {
    size_t Block, Boundary;
    const MIPS16RuntimeTransport *Transport;
    const SourceCallTypeHint *IndirectType = nullptr;
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
      if (Found == Transports.end())
        continue;
      const auto &Stub = Found->second;
      const SourceCallTypeHint *Hint = nullptr;
      if (Stub.IndirectFloatingPrefix) {
        const auto Physical = sourceCallOccurrenceKey(Call, Image.Arch);
        if (Physical && InputSites && ProjectedSites)
          if (auto It = InputSites->find(*Physical); It != InputSites->end())
            Hint = &It->second;
        std::string Diagnostic;
        if (!Hint || Hint->CallKind != SourceCallTypeHint::Kind::Analyst ||
            Hint->AnalystSite != Physical || !Hint->Signature.MIPSScalars ||
            !validateSourceABI(Hint->Signature, Diagnostic))
          throw LowToMedConversionError("MIPS16 indirect runtime call requires "
                                        "its function-pointer prototype");
        const auto &ABI = *Hint->Signature.MIPSScalars;
        std::array<unsigned, 2> Prefix{};
        unsigned Index = 0;
        for (const auto &P : ABI.Scalars) {
          if (P.Kind != SourceABICarrierKind::FloatingRegister)
            continue;
          if (Index == Prefix.size() ||
              P.Register != mipsreg::w(12 + Index * 2))
            throw LowToMedConversionError(
                "Unexpected o32 floating register prefix");
          Prefix[Index++] = P.Bytes;
        }
        const unsigned Elements = ABI.ReturnKind == NdTypeKind::Float
                                      ? (ABI.ReturnComplex ? 2 : 1)
                                      : 0;
        if (ABI.MIPS16Parameters || Prefix != *Stub.IndirectFloatingPrefix ||
            Stub.ResultElements != Elements ||
            (Elements && ABI.ReturnBytes != Stub.ResultBytes * Elements))
          throw LowToMedConversionError("Function-pointer prototype disagrees "
                                        "with the MIPS16 runtime transfers");
      }
      Sites.push_back({B, I, &Stub, Hint});
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
  if (ProjectedSites && InputSites &&
      std::any_of(Sites.begin(), Sites.end(),
                  [](const Site &S) { return S.IndirectType; }))
    *ProjectedSites = *InputSites;
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
      const auto &Matched = Sites[Site++];
      for (LowOp Op : Matched.Transport->Effects) {
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
        if (Op.Opcode == NdOp::INDIR_CALL && Matched.IndirectType) {
          auto Hint = *Matched.IndirectType;
          Hint.MIPS16RuntimeOrigin = Hint.AnalystSite;
          ProjectedSites->erase(*Hint.AnalystSite);
          Hint.AnalystSite = sourceCallOccurrenceKey(Op, Image.Arch);
          if (!Hint.AnalystSite)
            throw LowToMedConversionError(
                "Invalid projected MIPS16 indirect call");
          const auto Key = *Hint.AnalystSite;
          Hint.TargetAddress = 0;
          Hint.TargetName.clear();
          ProjectedSites->emplace(Key, std::move(Hint));
        }
        Block.Ops.push_back(std::move(Op));
      }
      Boundary.OpCount = Block.Ops.size() - Boundary.FirstOp;
      Boundary.Immediate.reset();
      if (Matched.IndirectType) {
        Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
        Boundary.TargetMode = LowInstructionTargetMode::FromMIPS16TargetBit0;
      } else {
        Boundary.Control = LowInstructionControl::None;
        Boundary.ControlFlags = LowInstructionControlFlag::None;
        Boundary.TargetMode = LowInstructionTargetMode::Preserve;
      }
    }
  }
  return Projected;
}

} // namespace neverd::pipeline_detail
