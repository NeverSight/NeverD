//===- SEHCallStackRanges.cpp - Source handler stack bounds --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep every protected SP alternative in physical return-slot validation.
///
//===----------------------------------------------------------------------===//

#include "SEHCallStackRanges.h"

#include "SEHFrame.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedProvenStackOffsets.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace neverd::x86_seh {

std::map<const MedOp *, EntryStackOffsetRange> sourceHandlerCallStackRanges(
    const MedFunc &Func, const LowFunc &Source, const BinaryImage &Image,
    const std::map<const MedOp *, MedVar> &Stacks,
    const std::map<int, MedVar> &BlockEntries, size_t &Remaining) {
  if (Image.Arch != Arch::X64 || Func.Entry != Source.Entry ||
      !Source.ExceptionMetadata || !Source.ExceptionMetadata->SEH ||
      !Func.HasUnresolvedSEHFrame)
    return {};
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto Charge = [&](size_t Count) {
    if (Count > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Count;
    return true;
  };
  const auto IsStack = [&](const MedVar &Value) {
    return Value.Kind == MedVar::Reg && Value.RegOff == TRI.StackPointer &&
           Value.Size == TRI.PointerSize;
  };
  struct Candidate {
    ProvenStackOffsetRange Seed;
    std::vector<MedVar> Inputs;
    bool Complete = false;
  };
  std::vector<Candidate> Candidates;
  for (const auto &Block : Func.Blocks) {
    if (!Charge(1 + Block.Phis.size() + Block.Ops.size() +
                Block.ExceptionalPreds.size()))
      return {};
    if (Block.ExceptionalPreds.empty() ||
        !llvm::all_of(Block.ExceptionalPreds, [](const auto &Edge) {
          return Edge.Kind == ExceptionalEdgeKind::SEHHandler;
        }))
      continue;
    Candidate Current;
    for (const auto &Phi : Block.Phis)
      if (IsStack(Phi.Output) && Phi.ExceptionalEntry &&
          Phi.ExceptionalEntry->Kind == MedVar::Unspecified) {
        Current.Seed.Join = &Phi;
        if (!Charge(Phi.Args.size()))
          return {};
        for (const auto &[Pred, Value] : Phi.Args)
          Current.Inputs.push_back(Value);
        break;
      }
    if (!Current.Seed.Join)
      for (const auto &Op : Block.Ops) {
        if (Op.Addr != Block.StartAddr)
          break;
        if (!Op.Dead && Op.Opcode == NdOp::COPY && IsStack(Op.Output) &&
            Op.NumInputs == 1 && Op.Inputs[0].Kind == MedVar::Unspecified) {
          Current.Seed.Definition = &Op;
          break;
        }
      }
    if (!Current.Seed.Definition && !Current.Seed.Join)
      continue;
    auto Proof = proveLowSEHEstablisherFrame(
        Source, Block.StartAddr, TRI, !Block.Preds.empty(), Remaining, {},
        false, LowSEHFrameUse::SourceProjection);
    if (!Proof) {
      llvm::consumeError(Proof.takeError());
      if (!Remaining)
        return {};
      continue;
    }
    if (!Proof->HasVaryingEntrySP)
      continue;
    std::vector<int> Protected;
    try {
      projectProtectedSources(Source, Func, *Proof, Remaining, &Protected,
                              nullptr);
    } catch (const LowToMedConversionError &) {
      if (!Remaining)
        return {};
      continue;
    }
    // Every current exceptional predecessor must be represented by the fresh
    // decoded scope. A stale or edited scope cannot discard another entry.
    if (llvm::any_of(Block.ExceptionalPreds, [&](const auto &Edge) {
          return !std::binary_search(Protected.begin(), Protected.end(),
                                     Edge.BlockId);
        }))
      continue;
    bool Covered = true;
    for (int Id : Protected) {
      const auto &From = Func.Blocks[Id];
      if (!Charge(From.Ops.size() + 1))
        return {};
      std::optional<MedVar> Stack;
      if (const auto It = BlockEntries.find(Id); It != BlockEntries.end())
        Stack = It->second;
      // The guard follows the certified prologue. Its entry block may also
      // contain the prologue, whose earlier SPs cannot fault in that guard.
      for (const auto &Op : From.Ops) {
        if (Op.Dead)
          continue;
        const bool AfterPrologue =
            Op.Addr >= Source.Entry + Source.ExceptionMetadata->PrologueSize;
        if (AfterPrologue) {
          if (!Stack) {
            Covered = false;
            break;
          }
          Current.Inputs.push_back(*Stack);
        }
        if (Op.Output.Kind == MedVar::Reg &&
            Op.Output.RegOff == TRI.StackPointer) {
          if (!IsStack(Op.Output)) {
            Covered = false;
            break;
          }
          Stack = Op.Output;
          if (AfterPrologue)
            Current.Inputs.push_back(*Stack);
        }
      }
      if (!Covered)
        break;
    }
    if (Covered && !Current.Inputs.empty())
      Candidates.push_back(std::move(Current));
  }
  // A later handler can be protected by an earlier one. Publish a bound only
  // when all of its sources are already proven; cycles without a seed stay
  // unknown. These bounds never become executable values or memory facts.
  std::vector<ProvenStackOffsetRange> Seeds;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &Candidate : Candidates) {
      if (!Charge(1 + Candidate.Inputs.size()))
        return {};
      if (Candidate.Complete)
        continue;
      const auto Bounds = entryStackOffsetRangesFromProvenDefinitions(
          Func, Candidate.Inputs, Seeds, Image.Arch, Image.abiFormat(),
          Func.EntryKind);
      if (llvm::any_of(Bounds, [](const auto &Bound) { return !Bound; }))
        continue;
      EntryStackOffsetRange Range = *Bounds.front();
      for (const auto &Bound : Bounds) {
        Range.Min = std::min(Range.Min, Bound->Min);
        Range.Max = std::max(Range.Max, Bound->Max);
      }
      Candidate.Seed.Range = Range;
      Seeds.push_back(Candidate.Seed);
      Candidate.Complete = true;
      Changed = true;
    }
  }
  if (Seeds.empty() || !Charge(Stacks.size()))
    return {};
  std::vector<MedVar> Values;
  for (const auto &[Call, Stack] : Stacks)
    Values.push_back(Stack);
  const auto Bounds = entryStackOffsetRangesFromProvenDefinitions(
      Func, Values, Seeds, Image.Arch, Image.abiFormat(), Func.EntryKind);
  std::map<const MedOp *, EntryStackOffsetRange> Result;
  size_t I = 0;
  for (const auto &[Call, Stack] : Stacks) {
    if (Bounds[I])
      Result.emplace(Call, *Bounds[I]);
    ++I;
  }
  return Result;
}

} // namespace neverd::x86_seh
