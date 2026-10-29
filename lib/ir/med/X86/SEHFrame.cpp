//===- SEHFrame.cpp - Windows frame proofs in converted CFGs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recheck decoded establisher frames and project their sources into MedIR.

#include "SEHFrame.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedIR.h"

#include <iterator>

namespace neverd::x86_seh {

// CFG simplification renumbers MedIR and can remove control-only LowIR blocks.
// Native operation addresses, rather than a block's old vector position, own
// the relation. This also covers a source block split into several Med blocks.
void projectProtectedSources(const LowFunc &Low, const MedFunc &Med,
                             const LowSEHEstablisherFrame &Proof,
                             size_t &Remaining, std::vector<int> *Sources,
                             std::map<int, std::set<va_t>> *Calls) {
  auto Fail = [](llvm::StringRef Why) {
    throw LowToMedConversionError("Windows SEH establisher frame: " +
                                  Why.str());
  };
  auto Charge = [&](size_t Amount = 1) {
    if (Amount > Remaining)
      Fail("converted source projection budget exhausted");
    Remaining -= Amount;
  };
  std::map<va_t, const LowBlock *> LowBlocks;
  for (const auto &Block : Low.Blocks) {
    Charge();
    LowBlocks.emplace(Block.StartAddr, &Block);
  }
  std::set<int> Needed(Proof.ProtectedSources.begin(),
                       Proof.ProtectedSources.end());
  std::map<va_t, int> CallSources;
  for (const auto &[Id, Sites] : Proof.ProtectedCalls)
    for (va_t Site : Sites) {
      Charge();
      if (!CallSources.emplace(Site, Id).second)
        Fail("ambiguous decoded throwing-call source");
    }
  std::set<int> Projected, SeenSources;
  std::set<va_t> SeenCalls;
  std::map<int, std::set<va_t>> ProjectedCalls;
  for (const auto &Block : Med.Blocks) {
    Charge();
    if (Block.Id < 0 || size_t(Block.Id) >= Med.Blocks.size() ||
        &Med.Blocks[Block.Id] != &Block)
      Fail("noncanonical converted frame CFG");
    for (const auto &Op : Block.Ops) {
      Charge();
      auto It = LowBlocks.upper_bound(Op.Addr);
      if (It == LowBlocks.begin())
        continue;
      const auto &Source = *std::prev(It)->second;
      if (Op.Addr >= Source.EndAddr)
        continue;
      if (Needed.count(Source.Id)) {
        Projected.insert(Block.Id);
        SeenSources.insert(Source.Id);
      }
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      auto Call = CallSources.find(Op.Addr);
      if (Call == CallSources.end())
        continue;
      if (Call->second != Source.Id || !SeenCalls.insert(Op.Addr).second)
        Fail("ambiguous converted throwing-call source");
      ProjectedCalls[Block.Id].insert(Op.Addr);
    }
  }
  for (int Id : Needed) {
    Charge();
    if (Id < 0 || size_t(Id) >= Low.Blocks.size() || Low.Blocks[Id].Id != Id)
      Fail("invalid decoded protected source");
    if (SeenSources.count(Id))
      continue;
    for (const auto &Op : Low.Blocks[Id].Ops) {
      Charge();
      if (Op.Opcode != NdOp::NOP && Op.Opcode != NdOp::BRANCH)
        Fail("conversion lost a protected source effect");
    }
  }
  if (SeenCalls.size() != CallSources.size())
    Fail("conversion lost a protected throwing call");
  if (Sources)
    Sources->assign(Projected.begin(), Projected.end());
  if (Calls)
    *Calls = std::move(ProjectedCalls);
}

std::optional<uint64_t>
proveSEHEstablisherFrame(const LowFunc &Low, const MedFunc &Med, va_t Handler,
                         const TargetRegInfo &TRI, bool OrdinaryEntry,
                         llvm::StringRef LocalUnwindDigest,
                         bool CxxContinuation,
                         std::vector<int> *ProtectedSources,
                         std::map<int, std::set<va_t>> *ProtectedCalls,
                         bool SourceProjection, bool *UnresolvedEntries) {
  auto Fail = [](const std::string &Why) -> void {
    throw LowToMedConversionError(
        std::string("Windows SEH establisher frame: ") + Why);
  };
  size_t WorkRemaining = limits::kMaxSEHEstablisherFrameWork;
  // The local unwinds converted MedIR turned into jumps enter their targets
  // from their calls.
  std::vector<LowSEHLocalUnwindExitSource> Exits;
  for (const MedSEHLocalUnwindExit &Exit : Med.SEHLocalUnwindExits)
    if (Exit.OwnsTarget)
      Exits.push_back(
          {Exit.CallAddr, Exit.CallSeq, Exit.Target, Exit.TargetFrame});
  auto Proof = proveLowSEHEstablisherFrame(
      Low, Handler, TRI, OrdinaryEntry, WorkRemaining, LocalUnwindDigest,
      CxxContinuation,
      SourceProjection ? LowSEHFrameUse::SourceProjection
                       : LowSEHFrameUse::InvocationIdentity,
      Exits);
  if (!Proof)
    Fail(llvm::toString(Proof.takeError()));
  auto Charge = [&](size_t Amount = 1) {
    if (Amount > WorkRemaining)
      Fail("converted frame propagation budget exhausted");
    WorkRemaining -= Amount;
  };
  const auto &EH = *Low.ExceptionMetadata;
  const bool HasFrameRegister = EH.FrameRegister != 0;
  const va_t PrologueEnd = Low.Entry + EH.PrologueSize;
  const size_t N = Low.Blocks.size();
  const auto &Relevant = Proof->RelevantBlocks;
  const auto &ProtectedBlock = Proof->ProtectedBlocks;
  const auto &CheckedEnd = Proof->CheckedEnd;
  const auto &DecodedAdjustments = Proof->DecodedAdjustments;
  bool DynamicSP = Proof->DynamicSP;
  // Low->Med may add ABI effects (for example a known callee's ret-pop).
  // Such effects are absent from decoded LowOps and must not bypass the SP
  // stability certificate, whether in the prologue or a protected prefix.
  std::map<va_t, uint64_t> ConvertedAdjustments;
  for (const MedBlock &Block : Med.Blocks) {
    Charge();
    for (const MedOp &Op : Block.Ops) {
      Charge();
      if (Op.Output.Kind != MedVar::Reg ||
          Op.Output.RegOff != TRI.StackPointer || !Op.Output.Size)
        continue;
      Charge(N);
      for (size_t B = 0; B < N; ++B) {
        if (!Relevant[B] || Op.Addr >= CheckedEnd[B] ||
            Op.Addr < Low.Blocks[B].StartAddr ||
            Op.Addr >= Low.Blocks[B].EndAddr)
          continue;
        if (HasFrameRegister &&
            (!ProtectedBlock[B] || Proof->HasVaryingEntrySP) &&
            (B != 0 || Op.Addr >= PrologueEnd)) {
          DynamicSP = true;
          continue;
        }
        if (Op.Addr >= PrologueEnd || Op.Opcode != NdOp::INT_SUB ||
            Op.Output.Size != 8 || Op.NumInputs != 2 ||
            Op.Inputs[0].Kind != MedVar::Reg ||
            Op.Inputs[0].RegOff != TRI.StackPointer || Op.Inputs[0].Size != 8 ||
            !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 8 ||
            !ConvertedAdjustments.emplace(Op.Addr, Op.Inputs[1].ConstVal)
                 .second)
          Fail("converted ABI effect changes the protected SP");
      }
    }
  }
  if (ConvertedAdjustments != DecodedAdjustments)
    Fail("converted prologue disagrees with decoded SP effects");
  if (DynamicSP && Proof->HasRelevantLocalUnwind)
    Fail("local-unwind call does not carry the protected frame");
  if (DynamicSP && OrdinaryEntry && !Proof->HasIndependentEntries &&
      !Proof->HasVaryingEntrySP)
    Fail("SP moves before a handler that ordinary flow also enters");
  if (ProtectedSources || ProtectedCalls)
    projectProtectedSources(Low, Med, *Proof, WorkRemaining, ProtectedSources,
                            ProtectedCalls);
  if (UnresolvedEntries)
    *UnresolvedEntries =
        Proof->HasIndependentEntries || Proof->HasVaryingEntrySP;
  if (DynamicSP)
    return std::nullopt;
  return Proof->FrameBytes;
}

} // namespace neverd::x86_seh
