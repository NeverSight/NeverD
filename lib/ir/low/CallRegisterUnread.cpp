//===- CallRegisterUnread.cpp - Unobserved callee register inputs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded input-observation proofs independent of inferred source arguments.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace neverd {
namespace {

constexpr GPRFamilyMask AllGPRs =
    (GPRFamilyMask{1} << kX64VectorFamilyBase) - 1;

GPRFamilyMask readFamilies(const GPRReadWidths &Widths) {
  GPRFamilyMask Result = 0;
  for (unsigned I = 0; I < kX64VectorFamilyBase; ++I)
    if (Widths[I])
      Result |= GPRFamilyMask{1} << I;
  return Result;
}

struct CallerInputs {
  size_t Caller;
  GPRFamilyMask Live;
};

struct Observation {
  GPRFamilyMask Reads = 0;
  GPRFamilyMask EntryKills = 0;
  std::vector<CallerInputs> Callers;
};

bool opaqueEntry(const LocalRegisterEffect &Effect) {
  return Effect.Incomplete || Effect.Unknown || Effect.UnknownEntryReads ||
         Effect.UnknownObservers || Effect.Blocks.empty() ||
         Effect.VariadicFrom >= 0;
}

bool charge(size_t &Budget, size_t Work = 1) {
  if (Work > Budget)
    return false;
  Budget -= Work;
  return true;
}

GPRFamilyMask entryKills(const LocalRegisterEffect &Effect, size_t &Budget) {
  if (opaqueEntry(Effect))
    return 0;
  GPRFamilyMask Result = 0;
  for (const auto &Step : Effect.Blocks.front().Steps) {
    if (!Budget)
      return 0;
    --Budget;
    // Only the unconditional entry prefix is authoritative. In particular,
    // CALL's ABI output and may-write summaries are not definite writes.
    if (Step.Callee != InvalidVA || Step.UnknownCall || Step.UnknownTailCall ||
        Step.FixedArguments || Step.FixedTailCall || Step.TailCallee ||
        Step.Exits)
      break;
    Result |= Step.Kills;
  }
  return Result & AllGPRs;
}

// Track the original entry bits that can still reach each observation.
// Kills, including callee entry kills, are fixed before this walk. A direct
// call therefore needs only the intersection of its callee's eventual reads
// with the families reaching this call site.
bool collectObservations(const LocalRegisterEffect &Effect, size_t Caller,
                         const llvm::DenseMap<va_t, size_t> &Indices,
                         std::vector<Observation> &Nodes, size_t &Budget) {
  auto &Local = Nodes[Caller];
  if (opaqueEntry(Effect)) {
    Local.Reads = AllGPRs;
    return true;
  }
  if (!charge(Budget, Effect.Blocks.size()))
    return false;
  for (const auto &Block : Effect.Blocks) {
    if (!charge(Budget, Block.Succs.size()))
      return false;
    for (size_t S : Block.Succs)
      if (S >= Effect.Blocks.size()) {
        Local.Reads = AllGPRs;
        return true;
      }
  }

  struct BlockInputs {
    GPRFamilyMask Reached = 0, Pending = 0;
  };
  std::vector<BlockInputs> Inputs(Effect.Blocks.size());
  Inputs.front() = {AllGPRs, AllGPRs};
  std::deque<size_t> Work{0};
  llvm::SmallDenseMap<size_t, GPRFamilyMask, 8> Calls;
  while (!Work.empty()) {
    if (!charge(Budget))
      return false;
    const size_t B = Work.front();
    Work.pop_front();
    GPRFamilyMask Live = std::exchange(Inputs[B].Pending, 0);
    const auto &Block = Effect.Blocks[B];
    for (const auto &Step : Block.Steps) {
      if (!Live)
        break;
      if (!charge(Budget))
        return false;
      Local.Reads |= readFamilies(Step.Reads) & Live;
      if (Step.UnknownCall || Step.UnknownTailCall || Step.FixedArguments ||
          Step.FixedTailCall) {
        Local.Reads |= Live;
        Live = 0;
      } else if (Step.Callee != InvalidVA) {
        const auto Target = Indices.find(Step.Callee);
        if (Target == Indices.end()) {
          Local.Reads |= Live;
          Live = 0;
        } else {
          Calls[Target->second] |= Live;
          Live &= ~Nodes[Target->second].EntryKills;
        }
      } else {
        Live &= ~Step.Kills;
      }
      if (Step.Exits || Step.TailCallee)
        Live = 0;
    }
    if (!Live)
      continue;
    if (!charge(Budget, Block.Succs.size()))
      return false;
    for (size_t S : Block.Succs) {
      auto &Next = Inputs[S];
      const auto Added = Live & ~Next.Reached;
      if (!Added)
        continue;
      if (!Next.Pending)
        Work.push_back(S);
      Next.Pending |= Added;
      Next.Reached |= Added;
    }
  }
  for (const auto &[Callee, Live] : Calls)
    if (const auto Needed = Live & ~Local.Reads)
      Nodes[Callee].Callers.push_back({Caller, Needed});
  return true;
}

} // namespace

std::map<va_t, GPRFamilyMask>
proveUnreadCallRegisters(const LocalRegisterEffectRefs &Funcs) {
  size_t Budget = limits::ir::kCallRegisterUnreadWork;
  if (!charge(Budget, Funcs.size()))
    return {};
  llvm::DenseMap<va_t, size_t> Indices;
  Indices.reserve(Funcs.size());
  std::vector<Observation> Nodes(Funcs.size());
  size_t I = 0;
  for (const auto &[Entry, Effect] : Funcs) {
    if (!Effect)
      return {};
    Indices.try_emplace(Entry, I);
    Nodes[I++].EntryKills = entryKills(*Effect, Budget);
  }
  I = 0;
  for (const auto &[Entry, Effect] : Funcs)
    if (!collectObservations(*Effect, I++, Indices, Nodes, Budget))
      return {};

  // Grow the least fixed point, including recursive call components. Only
  // newly observed bits propagate. Callee changes never rescan a caller CFG.
  std::deque<size_t> Work;
  std::vector<GPRFamilyMask> Pending(Nodes.size());
  for (I = 0; I < Nodes.size(); ++I)
    if (Nodes[I].Reads && !Nodes[I].Callers.empty()) {
      Pending[I] = Nodes[I].Reads;
      Work.push_back(I);
    }
  while (!Work.empty()) {
    const size_t Callee = Work.front();
    Work.pop_front();
    const auto Reads = std::exchange(Pending[Callee], 0);
    if (!charge(Budget, Nodes[Callee].Callers.size()))
      return {};
    for (const auto &[Caller, Live] : Nodes[Callee].Callers) {
      const auto Added = Live & Reads & ~Nodes[Caller].Reads;
      if (!Added)
        continue;
      Nodes[Caller].Reads |= Added;
      if (Nodes[Caller].Callers.empty())
        continue;
      if (!Pending[Caller])
        Work.push_back(Caller);
      Pending[Caller] |= Added;
    }
  }
  std::map<va_t, GPRFamilyMask> Unread;
  // The register-flow body deliberately omits implicit return transport and
  // ordinary SP accesses. Never publish their absence as an observation proof.
  const auto &TRI = getTargetRegInfo(Arch::X64);
  GPRFamilyMask Candidates = AllGPRs;
  GPRFamilyMask ReturnRegisters = 0;
  for (unsigned Family = 0; Family < kX64VectorFamilyBase; ++Family) {
    const uint64_t Register = Family * sizeof(uint64_t);
    if (TRI.isStackPointer(Register) || TRI.isReturnControlReg(Register))
      Candidates &= ~(GPRFamilyMask{1} << Family);
    else if (TRI.isReturnReg(Register) ||
             llvm::is_contained(TRI.IntReturnRegs, Register))
      ReturnRegisters |= GPRFamilyMask{1} << Family;
  }
  I = 0;
  for (const auto &[Entry, Effect] : Funcs) {
    const auto &Proof = Nodes[I++];
    // Return transport is not represented as an observation. Admit those
    // families only when the complete entry prefix destroys their entry bits.
    if (const auto Mask =
            Candidates & ~(ReturnRegisters & ~Proof.EntryKills) & ~Proof.Reads)
      Unread.emplace_hint(Unread.end(), Entry, Mask);
  }
  return Unread;
}

} // namespace neverd
