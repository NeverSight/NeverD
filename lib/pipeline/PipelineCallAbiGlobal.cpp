//===- PipelineCallAbiGlobal.cpp - Image-wide callee signatures -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The image-wide solve of the omitted-callee memo's kept bodies: forwarder
/// propagation over all of them at once, then each one's omitted recovery
/// under the summaries it leaves.  A run then installs what the solve gives
/// a callee instead of probing and recovering it again around its own
/// selection (recoverModuleCallAbi).  Forwarder propagation raises a body's
/// summaries from its own body and its callees' alone, and how far a run
/// reads below a callee leaves what it gives the callee as it was: on the
/// 300-function ntoskrnl 19041.6456 sample, every callee a run read ended
/// with the same summaries and recovery at 32 reads as at 4096, and runs
/// that installed the solve's for 98% of the callees they read decompiled
/// all 600 functions as before.  The solve runs in steps between a
/// session's requests, as the callee prefill does.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"

#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

/// One image-wide solve, kept by the memo between its steps.
struct GlobalCalleeSolve {
  enum class Stage { Probing, Recovering };
  Stage Phase = Stage::Probing;
  /// What the solve read: the memo's adoption and the declared callees.
  uint64_t Adoption = 0;
  std::set<va_t> PrototypeArguments;
  /// The kept bodies, held for the solve, and the index of each entry.
  std::vector<std::shared_ptr<const MedFunc>> Bodies;
  std::map<va_t, size_t> Index;
  /// The bodies that call each body directly, as forwarder propagation
  /// revisits them.
  std::vector<std::vector<size_t>> Callers;
  /// Whether the body takes a signature: not where a probe of it, or of a
  /// body it reads, failed.
  std::vector<char> Closed;
  /// The summaries every body gives, raised to the fixed point.
  CalleeSummaryMaps Maps;
  /// The bodies this round probes, those the next one does, and where the
  /// steps reached.
  std::vector<size_t> Round, Next;
  std::vector<char> Queued;
  size_t Cursor = 0;
  size_t Rounds = 0;
  /// The closed bodies, in the order the recovery steps take them, and what
  /// each recovery gives, by body.
  std::vector<size_t> Recover;
  std::vector<std::optional<OmittedCalleeMemo::GlobalSignature>> Results;
};

namespace {

/// The value \p Map holds under \p Key, if any.
template <typename Value>
std::optional<Value> heldAt(const std::map<va_t, Value> &Map, va_t Key) {
  const auto Found = Map.find(Key);
  if (Found == Map.end())
    return std::nullopt;
  return Found->second;
}

/// Puts \p Map's value under \p Key back to \p Was.
template <typename Value>
void restoreAt(std::map<va_t, Value> &Map, va_t Key,
               const std::optional<Value> &Was) {
  if (Was)
    Map[Key] = *Was;
  else
    Map.erase(Key);
}

/// The conversion of \p Callee that \p Summaries hold for, as a run reading
/// the callee under them takes it (Callee::conversionFor), without making
/// it the one used last.
const OmittedCalleeMemo::Conversion *
heldFor(const OmittedCalleeMemo::Callee &Callee,
        const PipelineResult &Summaries) {
  for (const OmittedCalleeMemo::Conversion &Each : Callee.Conversions)
    if (Each.readsHold(Summaries))
      return Each.Body && Each.Summary ? &Each : nullptr;
  return nullptr;
}

/// The solve of the bodies \p Memo keeps under \p Summaries.
std::shared_ptr<GlobalCalleeSolve> startSolve(const PipelineResult &Summaries,
                                              OmittedCalleeMemo &Memo) {
  auto Solve = std::make_shared<GlobalCalleeSolve>();
  Solve->Adoption = Memo.Adoptions;
  Solve->PrototypeArguments = Summaries.CallPrototypeArguments;
  std::vector<const OmittedCalleeMemo::Conversion *> Held;
  for (const auto &[Entry, Callee] : Memo.Callees)
    if (const OmittedCalleeMemo::Conversion *Each =
            heldFor(Callee, Summaries)) {
      Solve->Index.emplace(Entry, Solve->Bodies.size());
      Solve->Bodies.push_back(Each->Body);
      Held.push_back(Each);
      Solve->Maps.note(Entry, *Each->Summary);
    }
  const size_t Count = Solve->Bodies.size();
  Solve->Callers.resize(Count);
  Solve->Closed.assign(Count, 1);
  for (size_t I = 0; I < Count; ++I)
    for (const va_t Target : Held[I]->DirectCallTargets)
      if (const auto Found = Solve->Index.find(Target);
          Found != Solve->Index.end())
        Solve->Callers[Found->second].push_back(I);
  for (std::vector<size_t> &Each : Solve->Callers) {
    llvm::sort(Each);
    Each.erase(std::unique(Each.begin(), Each.end()), Each.end());
  }
  Solve->Round.resize(Count);
  std::iota(Solve->Round.begin(), Solve->Round.end(), size_t{0});
  Solve->Queued.assign(Count, 0);
  Solve->Results.resize(Count);
  for (size_t I = 0; I < Count; ++I)
    if (Solve->Closed[I])
      Solve->Recover.push_back(I);
  return Solve;
}

/// The weight of each body \p Order names from \p First, at most \p Count:
/// its operations, as the run's own probes weigh them.
std::vector<uint64_t> weightsOf(const GlobalCalleeSolve &Solve,
                                llvm::ArrayRef<size_t> Order) {
  std::vector<uint64_t> Weights;
  Weights.reserve(Order.size());
  for (const size_t I : Order) {
    uint64_t Weight = 1;
    for (const MedBlock &Block : Solve.Bodies[I]->Blocks)
      Weight += Block.Ops.size();
    Weights.push_back(Weight);
  }
  return Weights;
}

/// Leaves open \p Body and every body that reads it, transitively: what the
/// solve gives them may not be what a run gives.
void leaveOpen(GlobalCalleeSolve &Solve, size_t Body) {
  std::vector<size_t> Work{Body};
  while (!Work.empty()) {
    const size_t I = Work.back();
    Work.pop_back();
    if (!Solve.Closed[I])
      continue;
    Solve.Closed[I] = 0;
    for (const size_t Caller : Solve.Callers[I])
      Work.push_back(Caller);
  }
}

/// Probes the next bodies of the round, at most \p Budget, on the pipeline's
/// workers under the summaries as the step finds them, then raises the
/// summaries as forwarder propagation does, in round order.
void probeStep(const BinaryImage &Img, const std::map<va_t, std::string> &Names,
               const OmittedCalleeMemo &Memo, GlobalCalleeSolve &Solve,
               size_t Budget) {
  const size_t First = Solve.Cursor;
  const size_t Last = std::min(Solve.Round.size(), First + Budget);
  const llvm::ArrayRef<size_t> Order =
      llvm::ArrayRef<size_t>(Solve.Round).slice(First, Last - First);
  std::vector<std::optional<OmittedCalleeMemo::ProbeSignature>> Probes(
      Order.size());
  parallelForEachWeighted(
      weightsOf(Solve, Order),
      [&](auto Claim, size_t N) {
        // A probe's recovery writes these three at its body's entry: each
        // worker probes over its own copies, restored after each probe.
        std::map<va_t, int> RegArity = Solve.Maps.RegArity;
        std::map<va_t, int> TotalArity = Solve.Maps.TotalArity;
        std::map<va_t, bool> IsVariadic = Solve.Maps.IsVariadic;
        const RelocationSymbolIndex Relocations(Img);
        for (size_t W; (W = Claim()) < N;) {
          const MedFunc &Body = *Solve.Bodies[Order[W]];
          const auto Reg = heldAt(RegArity, Body.Entry);
          const auto Total = heldAt(TotalArity, Body.Entry);
          const auto Variadic = heldAt(IsVariadic, Body.Entry);
          try {
            Probes[W] = probeCalleeSignature(
                Body, Img, Names, Solve.Maps, RegArity, TotalArity, IsVariadic,
                Memo.pointerCallees(), Solve.PrototypeArguments, Relocations);
          } catch (...) {
            // The body stays open below; a run probes it in its turn.
          }
          restoreAt(RegArity, Body.Entry, Reg);
          restoreAt(TotalArity, Body.Entry, Total);
          restoreAt(IsVariadic, Body.Entry, Variadic);
        }
      },
      /*MinWorkItems=*/2);
  for (size_t W = 0; W < Order.size(); ++W) {
    const size_t I = Order[W];
    if (!Probes[W]) {
      leaveOpen(Solve, I);
      continue;
    }
    if (!applyCalleeProbe(Solve.Bodies[I]->Entry, *Probes[W], Solve.Maps))
      continue;
    for (const size_t Caller : Solve.Callers[I])
      if (!Solve.Queued[Caller]) {
        Solve.Queued[Caller] = 1;
        Solve.Next.push_back(Caller);
      }
  }
  Solve.Cursor = Last;
  if (Solve.Cursor < Solve.Round.size())
    return;
  // The round is done: the bodies whose callees grew make the next one.
  Solve.Round.swap(Solve.Next);
  Solve.Next.clear();
  for (const size_t I : Solve.Round)
    Solve.Queued[I] = 0;
  Solve.Cursor = 0;
  ++Solve.Rounds;
  if (!Solve.Round.empty() &&
      Solve.Rounds >=
          limits::pipeline::kPipelineCallAbiGlobalSignatureMaxRounds) {
    // Past the bound, leave every body open rather than stop short of the
    // fixed point.
    std::fill(Solve.Closed.begin(), Solve.Closed.end(), 0);
    Solve.Recover.clear();
    Solve.Round.clear();
  }
  if (Solve.Round.empty())
    Solve.Phase = GlobalCalleeSolve::Stage::Recovering;
}

/// Recovers the next closed bodies, at most \p Budget, on the pipeline's
/// workers under the summaries the fixed point left.
void recoverStep(const BinaryImage &Img,
                 const std::map<va_t, std::string> &Names,
                 const OmittedCalleeMemo &Memo, GlobalCalleeSolve &Solve,
                 size_t Budget) {
  const size_t First = Solve.Cursor;
  const size_t Last = std::min(Solve.Recover.size(), First + Budget);
  const llvm::ArrayRef<size_t> Order =
      llvm::ArrayRef<size_t>(Solve.Recover).slice(First, Last - First);
  parallelForEachWeighted(
      weightsOf(Solve, Order),
      [&](auto Claim, size_t N) {
        // A recovery writes these three at its body's entry, as a probe's.
        std::map<va_t, int> RegArity = Solve.Maps.RegArity;
        std::map<va_t, int> TotalArity = Solve.Maps.TotalArity;
        std::map<va_t, bool> IsVariadic = Solve.Maps.IsVariadic;
        const RelocationSymbolIndex Relocations(Img);
        for (size_t W; (W = Claim()) < N;) {
          const size_t I = Order[W];
          const MedFunc &Body = *Solve.Bodies[I];
          const auto Reg = heldAt(RegArity, Body.Entry);
          const auto Total = heldAt(TotalArity, Body.Entry);
          const auto Variadic = heldAt(IsVariadic, Body.Entry);
          try {
            OmittedCalleeMemo::GlobalSignature Gives;
            recoverOmittedCallee(Body, Img, Names, Solve.Maps, RegArity,
                                 TotalArity, IsVariadic, Memo.pointerCallees(),
                                 Solve.PrototypeArguments, Relocations, Gives);
            Solve.Results[I] = std::move(Gives);
          } catch (...) {
            // Without a signature, a run recovers the body itself.
          }
          restoreAt(RegArity, Body.Entry, Reg);
          restoreAt(TotalArity, Body.Entry, Total);
          restoreAt(IsVariadic, Body.Entry, Variadic);
        }
      },
      /*MinWorkItems=*/2);
  Solve.Cursor = Last;
}

/// Gives each closed body the solve recovered its signature, where the memo
/// still keeps its conversion, and stamps the memo with what the solve read.
void publish(OmittedCalleeMemo &Memo, GlobalCalleeSolve &Solve) {
  for (const size_t I : Solve.Recover) {
    if (!Solve.Closed[I] || !Solve.Results[I])
      continue;
    OmittedCalleeMemo::Conversion *Holder = Memo.conversionOf(*Solve.Bodies[I]);
    if (!Holder)
      continue;
    const va_t Entry = Solve.Bodies[I]->Entry;
    auto Signature = std::make_shared<OmittedCalleeMemo::GlobalSignature>(
        std::move(*Solve.Results[I]));
    const CalleeSummaryMaps &Maps = Solve.Maps;
    Signature->RegArity = heldAt(Maps.RegArity, Entry).value_or(0);
    Signature->TotalArity = heldAt(Maps.TotalArity, Entry).value_or(0);
    Signature->IsVariadic = heldAt(Maps.IsVariadic, Entry).value_or(false);
    Signature->IntRegs = heldAt(Maps.IntRegs, Entry);
    Signature->FPArity = heldAt(Maps.FPArity, Entry).value_or(0);
    Signature->FPRegs =
        heldAt(Maps.FPRegs, Entry).value_or(std::vector<uint64_t>());
    Signature->FPScalarBytes = heldAt(Maps.FPScalarBytes, Entry)
                                   .value_or(std::map<uint64_t, uint16_t>());
    Signature->FPReturnSize = heldAt(Maps.FPReturnSize, Entry).value_or(0);
    Memo.keepGlobal(*Holder, std::move(Signature));
  }
  Memo.Globals = OmittedCalleeMemo::GlobalStamp{
      Solve.Adoption, std::move(Solve.PrototypeArguments)};
}

} // namespace

bool solvesGlobalCalleeSignatures(const BinaryImage &Img,
                                  const PipelineResult &Summaries) {
  return limits::pipeline::kPipelineCallAbiGlobalSignatures &&
         Summaries.CalleeSignatureMemo && Img.Arch == Arch::X64 &&
         Img.abiFormat() == BinaryFormat::COFF && !Img.IsRelocatable;
}

bool stepGlobalCalleeSignatures(const BinaryImage &Img,
                                const PipelineResult &Summaries,
                                const std::map<va_t, std::string> &Names,
                                size_t Bodies) {
  if (!solvesGlobalCalleeSignatures(Img, Summaries))
    return true;
  OmittedCalleeMemo *Memo = Summaries.CalleeSignatureMemo.get();
  Memo->adopt(Img, Summaries);
  if (Memo->Globals && Memo->Globals->Adoption == Memo->Adoptions &&
      Memo->Globals->PrototypeArguments == Summaries.CallPrototypeArguments)
    return true;
  GlobalCalleeSolve *Solve = Memo->Solve.get();
  if (!Solve || Solve->Adoption != Memo->Adoptions ||
      Solve->PrototypeArguments != Summaries.CallPrototypeArguments) {
    // A step gathers the bodies; the next ones probe and recover them.
    Memo->Globals.reset();
    Memo->Solve = startSolve(Summaries, *Memo);
    return false;
  }
  const size_t Budget = std::max<size_t>(
      Bodies, limits::pipeline::kPipelineCallAbiGlobalSignatureStepBodies);
  if (Solve->Phase == GlobalCalleeSolve::Stage::Probing) {
    probeStep(Img, Names, *Memo, *Solve, Budget);
    return false;
  }
  if (Solve->Cursor < Solve->Recover.size()) {
    recoverStep(Img, Names, *Memo, *Solve, Budget);
    return false;
  }
  publish(*Memo, *Solve);
  Memo->Solve.reset();
  return true;
}

std::vector<std::shared_ptr<const OmittedCalleeMemo::GlobalSignature>>
installGlobalCalleeSignatures(
    const OmittedCalleeMemo &Memo, const PipelineResult &Result,
    llvm::ArrayRef<std::shared_ptr<const MedFunc>> Omitted,
    llvm::ArrayRef<OmittedCalleeMemo::Conversion *> Holders,
    const std::set<va_t> &Skip, CalleeSummaryMaps &Maps) {
  std::vector<std::shared_ptr<const OmittedCalleeMemo::GlobalSignature>>
      Installed(Omitted.size());
  if (!limits::pipeline::kPipelineCallAbiGlobalSignatures || !Memo.Globals ||
      Memo.Globals->Adoption != Memo.Adoptions ||
      Memo.Globals->PrototypeArguments != Result.CallPrototypeArguments)
    return Installed;
  for (size_t I = 0; I < Omitted.size() && I < Holders.size(); ++I) {
    const OmittedCalleeMemo::Conversion *Holder = Holders[I];
    const va_t Entry = Omitted[I]->Entry;
    if (!Holder || !Holder->Global || Skip.count(Entry))
      continue;
    const OmittedCalleeMemo::GlobalSignature &Global = *Holder->Global;
    Maps.RegArity[Entry] = Global.RegArity;
    Maps.TotalArity[Entry] = Global.TotalArity;
    Maps.IsVariadic[Entry] = Global.IsVariadic;
    if (Global.IntRegs)
      Maps.IntRegs[Entry] = *Global.IntRegs;
    else
      Maps.IntRegs.erase(Entry);
    Maps.FPArity[Entry] = Global.FPArity;
    Maps.FPRegs[Entry] = Global.FPRegs;
    Maps.FPScalarBytes[Entry] = Global.FPScalarBytes;
    Maps.FPReturnSize[Entry] = Global.FPReturnSize;
    Installed[I] = Holder->Global;
  }
  return Installed;
}

} // namespace neverd
