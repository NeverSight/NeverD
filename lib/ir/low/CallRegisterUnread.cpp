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

#include "llvm/ADT/STLExtras.h"

#include <deque>
#include <map>
#include <optional>
#include <set>
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

struct Observation {
  GPRFamilyMask Reads = 0;
  GPRFamilyMask EntryKills = 0;
};

GPRFamilyMask entryKills(const LocalRegisterEffect &Effect, size_t &Budget) {
  if (Effect.Incomplete || Effect.Unknown || Effect.UnknownEntryReads ||
      Effect.UnknownObservers || Effect.Blocks.empty() ||
      Effect.VariadicFrom >= 0)
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

std::optional<GPRFamilyMask>
entryReads(const LocalRegisterEffect &Effect,
           const std::map<va_t, Observation> &CalleeReads, size_t &Budget) {
  if (Effect.Incomplete || Effect.Unknown || Effect.UnknownEntryReads ||
      Effect.UnknownObservers || Effect.Blocks.empty() ||
      Effect.VariadicFrom >= 0)
    return AllGPRs;
  if (Effect.Blocks.size() > Budget)
    return std::nullopt;
  Budget -= Effect.Blocks.size();
  std::vector<GPRFamilyMask> Live(Effect.Blocks.size());
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t B = Effect.Blocks.size(); B-- > 0;) {
      if (!Budget)
        return std::nullopt;
      --Budget;
      GPRFamilyMask Next = 0;
      const RegisterBlock &Block = Effect.Blocks[B];
      for (size_t Successor : Block.Succs) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        if (Successor >= Live.size())
          return AllGPRs;
        Next |= Live[Successor];
      }
      for (auto I = Block.Steps.rbegin(); I != Block.Steps.rend(); ++I) {
        if (!Budget)
          return std::nullopt;
        --Budget;
        const RegisterStep &Step = *I;
        if (Step.Exits || Step.TailCallee || Step.UnknownTailCall ||
            Step.FixedTailCall)
          Next = 0;
        const bool Call = Step.Callee != InvalidVA || Step.UnknownCall ||
                          Step.UnknownTailCall || Step.FixedArguments ||
                          Step.FixedTailCall;
        if (Call) {
          const auto Callee = CalleeReads.find(Step.Callee);
          // A prototype fixes source arguments, not implicit machine-state
          // observations. Initial pointer contents also cannot bind a call.
          if (Step.UnknownCall || Step.UnknownTailCall || Step.FixedArguments ||
              Step.FixedTailCall || Callee == CalleeReads.end())
            Next |= AllGPRs;
          else {
            // A complete entry prefix executes before every normal return.
            // Reads within the callee still observe its incoming values;
            // only the caller's later observation is satisfied by that write.
            Next &= ~Callee->second.EntryKills;
            Next |= Callee->second.Reads;
          }
        } else {
          Next &= ~Step.Kills;
          // Partial writes cannot kill a whole family in this bit proof.
        }
        Next |= readFamilies(Step.Reads);
      }
      if (Next != Live[B]) {
        Live[B] = Next;
        Changed = true;
      }
    }
  }
  return Live.front();
}

} // namespace

std::map<va_t, GPRFamilyMask>
proveUnreadCallRegisters(const LocalRegisterEffectRefs &Funcs) {
  size_t Budget = limits::ir::kCallRegisterUnreadWork;
  if (Funcs.size() > Budget)
    return {};
  Budget -= Funcs.size();
  std::map<va_t, Observation> Reads;
  std::map<va_t, std::set<va_t>> Readers;
  std::deque<va_t> Pending;
  std::set<va_t> Queued;
  for (const auto &[Entry, Effect] : Funcs) {
    if (!Effect)
      return {};
    Reads.emplace(Entry, Observation{0, entryKills(*Effect, Budget)});
    Pending.push_back(Entry);
    Queued.insert(Entry);
    if (Effect->Blocks.size() > Budget)
      return {};
    Budget -= Effect->Blocks.size();
    for (const RegisterBlock &Block : Effect->Blocks)
      for (const RegisterStep &Step : Block.Steps) {
        if (!Budget)
          return {};
        --Budget;
        if (Step.Callee != InvalidVA && Funcs.count(Step.Callee))
          Readers[Step.Callee].insert(Entry);
      }
  }
  while (!Pending.empty()) {
    const va_t Entry = Pending.front();
    Pending.pop_front();
    Queued.erase(Entry);
    const auto Next = entryReads(*Funcs.at(Entry), Reads, Budget);
    if (!Next)
      return {};
    GPRFamilyMask &Read = Reads.at(Entry).Reads;
    if ((*Next | Read) == Read)
      continue;
    Read |= *Next;
    if (const auto It = Readers.find(Entry); It != Readers.end())
      for (va_t Reader : It->second) {
        if (!Budget)
          return {};
        --Budget;
        if (Queued.insert(Reader).second)
          Pending.push_back(Reader);
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
  for (const auto &[Entry, Proof] : Reads)
    // Return transport is not represented as an observation. Admit those
    // families only when the complete entry prefix destroys their entry bits.
    if (const auto Mask =
            Candidates & ~(ReturnRegisters & ~Proof.EntryKills) & ~Proof.Reads)
      Unread.emplace(Entry, Mask);
  return Unread;
}

} // namespace neverd
