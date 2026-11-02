//===- SEHFrameProof.cpp - Local-unwind frame arguments -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Tracks entry-relative stack identities through the complete LowIR CFG.

#include "neverd/ir/low/SEHFrameProof.h"

#include "SEHFrameProofDetail.h"
#include "SEHFrameRuntimeMemory.h"
#include "X86/FrameEffects.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/LowUndefinedEffects.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

using seh_frame::Key;
using seh_frame::key;
using seh_frame::State;
using seh_frame::Value;

bool consume(size_t &Remaining, size_t Amount = 1) {
  if (Amount > Remaining) {
    Remaining = 0;
    return false;
  }
  Remaining -= Amount;
  return true;
}

bool chargeFrameMetadata(const LowFunc &F, size_t &Remaining) {
  if (!consume(Remaining, F.CxxContinuationEntries.size() + 1))
    return false;
  if (!F.ExceptionMetadata)
    return true;
  const auto &EH = *F.ExceptionMetadata;
  if (!consume(Remaining, EH.FragmentRanges.size() +
                              EH.UnwindOperations.size() +
                              (EH.SEH ? EH.SEH->Scopes.size() : 0) + 1))
    return false;
  if (!EH.Cxx)
    return true;
  const auto &Cxx = *EH.Cxx;
  if (!consume(Remaining, Cxx.UnwindMap.size() + Cxx.IPMap.size() +
                              Cxx.TryBlocks.size() + 1))
    return false;
  for (const auto &Try : Cxx.TryBlocks)
    if (!consume(Remaining, Try.Handlers.size() + 1))
      return false;
  return true;
}

// Both sides of a call need the complete decoded paths. A symmetric graph
// alone can omit a branch arm that overwrites the saved frame pointer.
} // namespace

bool completeLowSEHFrameCFG(const LowFunc &F,
                            const std::map<int, size_t> &Positions,
                            size_t &Remaining, bool SourceProjection) {
  std::map<va_t, va_t> Ranges;
  for (const auto &B : F.Blocks) {
    if (!consume(Remaining, B.Preds.size() + B.Succs.size() + B.Ops.size() +
                                B.InstructionBoundaries.size() + 1))
      return false;
    if (auto Error = validateLowInstructionBoundaries(
            B, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return false;
    }
    if (!Ranges.emplace(B.StartAddr, B.EndAddr).second)
      return false;
    for (const auto &Insn : B.InstructionBoundaries) {
      if (Insn.Mode != InstructionMode::Default ||
          Insn.TargetMode != LowInstructionTargetMode::Preserve ||
          Insn.Control == LowInstructionControl::ConditionalCall ||
          Insn.Control == LowInstructionControl::ConditionalReturn ||
          (&Insn != &B.InstructionBoundaries.back() &&
           (isUnconditionalNoReturn(Insn) ||
            Insn.Control == LowInstructionControl::Branch ||
            Insn.Control == LowInstructionControl::Return ||
            Insn.Control == LowInstructionControl::TailCall ||
            (Insn.Control == LowInstructionControl::Terminator &&
             !hasLowInstructionControlFlag(
                 Insn.ControlFlags, LowInstructionControlFlag::Resumable)))))
        return false;
    }
    for (int Id : B.Preds)
      if (!Positions.count(Id) || !F.Blocks[Positions.at(Id)].hasSucc(B.Id))
        return false;
    std::set<va_t> Successors;
    for (int Id : B.Succs)
      if (!Positions.count(Id) ||
          !llvm::is_contained(F.Blocks[Positions.at(Id)].Preds, B.Id))
        return false;
      else
        Successors.insert(F.Blocks[Positions.at(Id)].StartAddr);
    const auto &Insn = B.InstructionBoundaries.back();
    std::set<va_t> Expected;
    if (Insn.Control == LowInstructionControl::Branch) {
      if (B.Ops.empty())
        return false;
      const auto &Last = B.Ops.back();
      if (Last.Opcode == NdOp::INDIR_BR) {
        if (!SourceProjection || Last.NumInputs != 1 || Last.Output.Size ||
            !hasLowInstructionControlFlag(
                Insn.ControlFlags, LowInstructionControlFlag::Indirect) ||
            F.UnsafeIndirectBranchAddresses.count(Last.Addr) ||
            !consume(Remaining, F.JumpTables.size() + 1))
          return false;
        const JumpTable *Table = nullptr;
        for (const auto &Candidate : F.JumpTables)
          if (Candidate.InsnAddr == Last.Addr) {
            if (Table || Candidate.MutatedUnsafe || Candidate.Targets.empty())
              return false;
            Table = &Candidate;
          }
        if (!Table || !consume(Remaining, Table->Targets.size()))
          return false;
        // The resolver owns the dispatch. The frame projection must visit
        // every published target, including duplicate case destinations.
        Expected.insert(Table->Targets.begin(), Table->Targets.end());
      } else {
        const bool Conditional = Last.Opcode == NdOp::COND_BR;
        if ((!Conditional && Last.Opcode != NdOp::BRANCH) ||
            Last.NumInputs != (Conditional ? 2 : 1) ||
            !Last.Inputs[0].isConst() || Last.Inputs[0].Size != 8)
          return false;
        Expected.insert(Last.Inputs[0].Offset);
        if (Conditional)
          Expected.insert(B.EndAddr);
      }
    } else if (Insn.Control != LowInstructionControl::Return &&
               Insn.Control != LowInstructionControl::TailCall &&
               !isUnconditionalNoReturn(Insn) &&
               (Insn.Control != LowInstructionControl::Terminator ||
                hasLowInstructionControlFlag(
                    Insn.ControlFlags, LowInstructionControlFlag::Resumable)))
      Expected.insert(B.EndAddr);
    if (Successors != Expected)
      return false;
  }
  va_t End = 0;
  for (const auto &[Start, Limit] : Ranges) {
    if (Start < End)
      return false;
    End = Limit;
  }
  return true;
}

namespace {

struct CallContext {
  const std::map<va_t, const LowFunc *> *Callees = nullptr;
  std::set<va_t> Dependencies;
  std::set<va_t> ActiveFunctions;
  std::set<int64_t> ReturnSlots;
  const LowSEHRuntimeMemory *RuntimeMemory = nullptr;
  bool UsesRuntimeMemory = false;
  bool HasExceptionCallbacks = false;

  bool overlapsReturnPC(int64_t Begin, int64_t End) const {
    return llvm::any_of(ReturnSlots, [&](int64_t Slot) {
      return Begin < Slot + 8 && Slot < End;
    });
  }
};

std::optional<State> transferCallee(const LowFunc &Callee, const State &Caller,
                                    const TargetRegInfo &TRI, size_t &Remaining,
                                    CallContext &Calls);

bool transfer(const LowBlock &Block, State &S, const TargetRegInfo &TRI,
              size_t &Remaining, LowSEHFrameProof *Record,
              CallContext *Calls = nullptr,
              std::optional<int64_t> ReturnSlot = std::nullopt) {
  const NdVar SP = NdVar::reg(TRI.StackPointer, 8);
  const NdVar Frame = NdVar::reg(TRI.Win64ParamRegs.front(), 8);
  for (const auto &Boundary : Block.InstructionBoundaries) {
    if (!consume(Remaining))
      return false;
    if (hasLowInstructionControlFlag(
            Boundary.ControlFlags,
            LowInstructionControlFlag::InstructionGuard) ||
        Boundary.Control == LowInstructionControl::ConditionalCall ||
        Boundary.Control == LowInstructionControl::ConditionalReturn) {
      S = {};
      return true;
    }
  }
  va_t Instruction = InvalidVA;
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    if (!consume(Remaining, S.Values.size() + S.Spills.size() +
                                S.FrameBytes.size() + S.ImageBytes.size() +
                                S.InvalidatedImportSlots.size() + 1))
      return false;
    const LowOp &Op = Block.Ops[I];
    if (Op.Opcode >= NdOp::_COUNT || Op.NumInputs > std::size(Op.Inputs) ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Op.Output.Size && ((!Op.Output.isReg() && !Op.Output.isTemp()) ||
                            Op.Output.Offset > UINT64_MAX - Op.Output.Size))) {
      S = {};
      return true;
    }
    if (Op.Addr != Instruction) {
      S.dropTemporaries();
      Instruction = Op.Addr;
    }
    if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
        Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN) {
      // An instruction-local branch cannot be flattened into unconditional
      // copies. The ordinary terminal branch's alternatives already join in
      // the CFG. A malformed trailing effect supplies no outgoing facts.
      if (I + 1 != Block.Ops.size() || Op.Output.Size)
        S = {};
      break;
    }
    // Ordinary FP completion preserves integer storage, but a local filter
    // may change that storage before continuing a faulting instruction. The
    // x64 proof has no callback-effect receipt yet. Reading MXCSR cannot trap;
    // other FP effects need a callback-free invocation here.
    const bool PreservesFrame =
        isX86FramePreservingIntrinsic(Op, TRI.TheArch) && Calls &&
        (!Calls->HasExceptionCallbacks ||
         Op.Inputs[0].Offset == static_cast<unsigned>(Intrinsic::X86ReadMXCSR));
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        (Op.Opcode == NdOp::INTRINSIC && !PreservesFrame)) {
      const auto CurrentSP = S.get(SP);
      if (Record && CurrentSP && S.get(Frame) == CurrentSP &&
          Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8) {
        if (!Record->Calls.emplace(std::make_pair(Op.Addr, Op.Seq), *CurrentSP)
                 .second)
          return false;
      }
      std::optional<State> Returned;
      if (Calls && Calls->Callees && CurrentSP &&
          (ReturnSlot || !S.Spills.empty() || !S.FrameBytes.empty() ||
           !S.ImageBytes.empty()) &&
          Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 8) {
        const auto It = Calls->Callees->find(Op.Inputs[0].Offset);
        if (It != Calls->Callees->end() && It->second &&
            It->second->Entry == It->first) {
          Returned = transferCallee(*It->second, S, TRI, Remaining, *Calls);
          if (Returned)
            Calls->Dependencies.insert(It->first);
        }
      }
      if (!Returned && Calls && Calls->RuntimeMemory) {
        if (!consume(Remaining, Calls->ReturnSlots.size() + 1))
          return false;
        Returned = seh_frame::transferRuntimeMemory(
            Op, S, TRI, *Calls->RuntimeMemory, Calls->ReturnSlots, Remaining);
        Calls->UsesRuntimeMemory |= Returned.has_value();
      }
      if (ReturnSlot && !Returned)
        return false;
      // Inspected stores or an authenticated runtime invocation own saved
      // cells. A neighbouring pointer passed to an opaque callee has no extent.
      State Preserved;
      Preserved.InvalidatedImportSlots = S.InvalidatedImportSlots;
      Preserved.EscapedFrame = S.EscapedFrame || !Returned;
      if (Returned)
        Preserved.EscapedFrame |= Returned->EscapedFrame;
      if (Returned) {
        Preserved.InvalidatedImportSlots.insert(
            Returned->InvalidatedImportSlots.begin(),
            Returned->InvalidatedImportSlots.end());
        Preserved.Spills = std::move(Returned->Spills);
        Preserved.FrameBytes = std::move(Returned->FrameBytes);
        Preserved.ImageBytes = std::move(Returned->ImageBytes);
        const auto ReturnKey = key(NdVar::reg(TRI.IntReturnReg, 8));
        if (auto It = Returned->Values.find(ReturnKey);
            It != Returned->Values.end())
          Preserved.Values.emplace(*It);
        for (const auto &[K, Value] : S.Values) {
          const auto &[Space, Offset, Size] = K;
          const auto AtReturn = Returned->Values.find(K);
          // The actual body must preserve the value as well as its ABI
          // carrier. A declared callee-save register alone grants no fact.
          if (Space == VnodeSpace::REG &&
              TRI.isCallPreserved(Offset, Size, BinaryFormat::COFF) &&
              AtReturn != Returned->Values.end() && AtReturn->second == Value)
            Preserved.Values.emplace(K, Value);
        }
      }
      S = std::move(Preserved);
      const bool DefinesSP = Op.Output.isReg() && Op.Output.Size &&
                             Op.Output.Offset < SP.Offset + SP.Size &&
                             SP.Offset < Op.Output.Offset + Op.Output.Size;
      if (CurrentSP && Op.Opcode != NdOp::INTRINSIC && !DefinesSP)
        S.Values[key(SP)] = Value::frame(*CurrentSP);
      continue;
    }
    if (Op.Opcode == NdOp::STORE) {
      if (Op.NumInputs != 2 || Op.Output.Size) {
        S = {};
        return true;
      }
      const auto Address = S.get(Op.Inputs[0]);
      const auto StoredValue = S.value(Op.Inputs[1]);
      int64_t End = 0;
      if (!Address || !Op.Inputs[1].Size ||
          llvm::AddOverflow(*Address, int64_t{Op.Inputs[1].Size}, End)) {
        const auto ImageAddress = S.constant(Op.Inputs[0]);
        if (Calls && Calls->RuntimeMemory && ImageAddress &&
            seh_frame::imageStoreIsDisjoint(*Calls->RuntimeMemory,
                                            *ImageAddress, Op.Inputs[1].Size)) {
          Calls->UsesRuntimeMemory = true;
          if (!consume(Remaining, Calls->RuntimeMemory->calls().size() + 1))
            return false;
          for (const auto &[Identity, Binding] : Calls->RuntimeMemory->calls())
            if (*ImageAddress < Binding.IATAddress + 8 &&
                Binding.IATAddress < *ImageAddress + Op.Inputs[1].Size)
              S.InvalidatedImportSlots.insert(Binding.IATAddress);
          if (!consume(Remaining, S.ImageBytes.size() + Op.Inputs[1].Size))
            return false;
          std::erase_if(S.ImageBytes, [&](const auto &Byte) {
            return *ImageAddress <= Byte.first &&
                   Byte.first < *ImageAddress + Op.Inputs[1].Size;
          });
          if (StoredValue && StoredValue->Identity == Value::Kind::Constant)
            for (unsigned Byte = 0; Byte != Op.Inputs[1].Size; ++Byte)
              S.ImageBytes[*ImageAddress + Byte] =
                  uint8_t(StoredValue->Bits >> (Byte * 8));
          // Unsupported arithmetic or a partial pointer carrier loses the
          // exact value, not its possible alias. Exporting unknown bits must
          // not restore privacy merely because no Frame fact remains.
          S.EscapedFrame |=
              !StoredValue || StoredValue->Identity == Value::Kind::Frame;
          continue;
        }
        if (ReturnSlot)
          return false; // It may overwrite this invocation's return PC.
        S.clearMemory();
        S.EscapedFrame = true;
        continue;
      }
      if (ReturnSlot &&
          (!Calls || !consume(Remaining, Calls->ReturnSlots.size()) ||
           Calls->overlapsReturnPC(*Address, End)))
        return false;
      S.invalidateMemory(*Address, End);
      if (StoredValue && Op.Inputs[1].Size == 8)
        S.Spills[*Address] = *StoredValue;
      if (StoredValue && StoredValue->Identity == Value::Kind::Constant)
        for (unsigned Byte = 0; Byte != Op.Inputs[1].Size; ++Byte)
          S.FrameBytes[*Address + Byte] =
              uint8_t(StoredValue->Bits >> (Byte * 8));
      continue;
    }
    if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
        Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
      if (ReturnSlot)
        return false;
      S.clearMemory();
      S.EscapedFrame = true;
    }
    std::optional<Value> Fact;
    if (auto Bits = lowScalarConstantResult(
            Op, [&](const NdVar &V) { return S.constant(V); }))
      Fact = Value::constant(*Bits);
    else if (Op.NumInputs == 1 && Op.Opcode == NdOp::COPY &&
             Op.Output.Size == Op.Inputs[0].Size)
      Fact = S.value(Op.Inputs[0]);
    else if (Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB)) {
      const auto DeltaBits = S.constant(Op.Inputs[1]);
      if (Op.Output.Size == 8 && Op.Inputs[1].Size == 8 && DeltaBits) {
        if (auto Base = S.get(Op.Inputs[0])) {
          const int64_t Delta = static_cast<int64_t>(*DeltaBits);
          int64_t Sum = 0;
          const bool Overflow = Op.Opcode == NdOp::INT_ADD
                                    ? llvm::AddOverflow(*Base, Delta, Sum)
                                    : llvm::SubOverflow(*Base, Delta, Sum);
          if (!Overflow)
            Fact = Value::frame(Sum);
        }
      }
    } else if (Op.Output.Size == 8 && Op.NumInputs == 1 &&
               Op.Opcode == NdOp::LOAD) {
      if (auto Address = S.get(Op.Inputs[0])) {
        if (auto It = S.Spills.find(*Address); It != S.Spills.end())
          Fact = It->second;
      } else if (Calls && Calls->RuntimeMemory) {
        const auto Address = S.constant(Op.Inputs[0]);
        if (Address && !S.InvalidatedImportSlots.count(*Address) &&
            Calls->RuntimeMemory->calls().count({*Address, true})) {
          Fact = Value{Value::Kind::ImportTarget, *Address};
          Calls->UsesRuntimeMemory = true;
        }
      }
    }
    if (Fact && (!Op.Output.Size || Op.Output.Size > 8))
      Fact.reset();
    if (Fact && Fact->Identity == Value::Kind::Constant)
      Fact->Bits &= UINT64_MAX >> ((8 - Op.Output.Size) * 8);
    if (!Op.Output.Size)
      continue;
    if (Op.Output.Space == SP.Space && Op.Output.Offset < SP.Offset + SP.Size &&
        SP.Offset < Op.Output.Offset + Op.Output.Size) {
      const auto Before = S.get(SP);
      const auto Offset = Fact && Fact->Identity == Value::Kind::Frame
                              ? std::optional<int64_t>{int64_t(Fact->Bits)}
                              : std::nullopt;
      if (!Offset || !Before || Op.Output != SP)
        S.clearMemory();
      else if (*Before < *Offset)
        S.invalidateMemory(*Before, *Offset);
    }
    std::erase_if(S.Values, [&](const auto &V) {
      const auto &[Space, Offset, Size] = V.first;
      return Space == Op.Output.Space &&
             Offset < Op.Output.Offset + Op.Output.Size &&
             Op.Output.Offset < Offset + Size;
    });
    if (Fact) {
      S.Values[key(Op.Output)] = *Fact;
      if (Fact->Identity == Value::Kind::Constant && Op.Output.isReg() &&
          Op.Output.Size == 4 &&
          TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size))
        S.Values[key(NdVar::reg(Op.Output.Offset, 8))] = *Fact;
    }
  }
  S.dropTemporaries();
  return true;
}

// Inspect the concrete argument/frame values at this call, preserving only
// saved cells common to all returns. The architectural call owns one new PC
// slot; LowIR's RETURN does not otherwise expose its load. A store through an
// unknown address, a modified active PC, a non-returning exit or an unproved
// nested call cannot establish this contract. No parameter width or ABI
// declaration bounds writes. Every nested body shares the caller's budget.
std::optional<State> transferCallee(const LowFunc &F, const State &Caller,
                                    const TargetRegInfo &TRI, size_t &Remaining,
                                    CallContext &Calls) {
  const auto SP = NdVar::reg(TRI.StackPointer, 8);
  const auto CallerSP = Caller.get(SP);
  int64_t Slot = 0;
  if (!CallerSP || llvm::SubOverflow(*CallerSP, int64_t{8}, Slot) ||
      Calls.ActiveFunctions.size() >=
          ::neverd::limits::ir::kSEHFrameProofMaxCalleeDepth ||
      Calls.ActiveFunctions.count(F.Entry) ||
      !consume(Remaining, Calls.ReturnSlots.size() + 1) ||
      Calls.overlapsReturnPC(Slot, *CallerSP) || !F.hasCompleteLiftCoverage() ||
      !F.FunctionTemporaries.empty() || F.Blocks.empty() || F.CalleePopBytes ||
      (F.ExceptionMetadata &&
       (F.ExceptionMetadata->SEH || F.ExceptionMetadata->Cxx ||
        F.ExceptionMetadata->Registration)) ||
      !consume(Remaining, Caller.Values.size() + Caller.Spills.size() +
                              Caller.FrameBytes.size() +
                              Caller.ImageBytes.size() + 1))
    return std::nullopt;
  Calls.ActiveFunctions.insert(F.Entry);
  Calls.ReturnSlots.insert(Slot);
  llvm::scope_exit RestoreInvocation([&] {
    Calls.ActiveFunctions.erase(F.Entry);
    Calls.ReturnSlots.erase(Slot);
  });
  std::map<int, size_t> Positions;
  std::set<va_t> Starts;
  std::optional<size_t> Entry;
  for (size_t I = 0; I != F.Blocks.size(); ++I) {
    const auto &B = F.Blocks[I];
    if (!consume(Remaining) || B.Id < 0 || !Positions.emplace(B.Id, I).second ||
        !Starts.insert(B.StartAddr).second || !B.ExceptionalPreds.empty() ||
        !B.ExceptionalSuccs.empty())
      return std::nullopt;
    if (B.StartAddr == F.Entry)
      Entry = I;
  }
  if (!Entry || !F.Blocks[*Entry].Preds.empty() ||
      !completeLowSEHFrameCFG(F, Positions, Remaining))
    return std::nullopt;
  for (const auto *Roots :
       {&F.ModuleAnalysisRoots, &F.OrdinaryModuleAnalysisRoots})
    for (va_t Root : *Roots)
      if (!consume(Remaining) || Root != F.Entry)
        return std::nullopt;
  State Initial = Caller;
  Initial.dropTemporaries();
  Initial.Values[key(SP)] = Value::frame(Slot);
  // The callee owns new storage below the caller SP, including the pushed PC.
  // Reusing stale values from a previous invocation would prove false loads.
  std::erase_if(Initial.Spills,
                [&](const auto &Cell) { return Cell.first < *CallerSP; });
  std::erase_if(Initial.FrameBytes,
                [&](const auto &Byte) { return Byte.first < *CallerSP; });
  std::vector<std::optional<State>> Inputs(F.Blocks.size());
  Inputs[*Entry] = std::move(Initial);
  std::deque<size_t> Queue{*Entry};
  std::vector<bool> Queued(F.Blocks.size());
  Queued[*Entry] = true;
  std::optional<State> Returned;
  while (!Queue.empty()) {
    const size_t I = Queue.front();
    Queue.pop_front();
    Queued[I] = false;
    const auto &B = F.Blocks[I];
    if (!consume(Remaining, Inputs[I]->Values.size() +
                                Inputs[I]->Spills.size() +
                                Inputs[I]->FrameBytes.size() +
                                Inputs[I]->ImageBytes.size() + 1) ||
        B.Ops.empty())
      return std::nullopt;
    State Out = *Inputs[I];
    if (!transfer(B, Out, TRI, Remaining, nullptr, &Calls, Slot))
      return std::nullopt;
    const auto &Last = B.Ops.back();
    if (Last.Opcode == NdOp::RETURN) {
      const auto Boundary =
          llvm::find_if(B.InstructionBoundaries, [&](const auto &Insn) {
            return Insn.Address == Last.Addr;
          });
      if (!B.Succs.empty() || Out.get(SP) != Slot || Last.Seq < 0 ||
          Boundary == B.InstructionBoundaries.end() ||
          Boundary->Control != LowInstructionControl::Return ||
          !hasLowInstructionControlFlag(Boundary->ControlFlags,
                                        LowInstructionControlFlag::Return) ||
          (static_cast<uint16_t>(Boundary->ControlFlags) &
           ~static_cast<uint16_t>(LowInstructionControlFlag::Return |
                                  LowInstructionControlFlag::Indirect)) ||
          Boundary->Immediate.value_or(0) != 0 || !Boundary->OpCount ||
          Boundary->FirstOp >= B.Ops.size() ||
          Boundary->OpCount != B.Ops.size() - Boundary->FirstOp ||
          Last.Addr >= B.EndAddr || Boundary->Size != B.EndAddr - Last.Addr)
        return std::nullopt;
      if (!Returned)
        Returned = Out;
      else {
        if (!consume(Remaining, Returned->Values.size() +
                                    Returned->Spills.size() +
                                    Returned->FrameBytes.size() +
                                    Returned->ImageBytes.size()))
          return std::nullopt;
        Returned->intersect(Out);
      }
      continue;
    }
    if (B.Succs.empty() || Last.Opcode == NdOp::INDIR_BR)
      return std::nullopt;
    for (int Id : B.Succs) {
      const size_t Next = Positions.at(Id);
      const size_t OldFacts = Inputs[Next]
                                  ? Inputs[Next]->Values.size() +
                                        Inputs[Next]->Spills.size() +
                                        Inputs[Next]->FrameBytes.size() +
                                        Inputs[Next]->ImageBytes.size()
                                  : 0;
      if (!consume(Remaining, Out.Values.size() + Out.Spills.size() +
                                  Out.FrameBytes.size() +
                                  Out.ImageBytes.size() + OldFacts + 1))
        return std::nullopt;
      bool Changed = !Inputs[Next];
      if (Changed)
        Inputs[Next] = Out;
      else {
        State Joined = *Inputs[Next];
        Joined.intersect(Out);
        Changed = Joined != *Inputs[Next];
        Inputs[Next] = std::move(Joined);
      }
      if (Changed && !Queued[Next]) {
        Queued[Next] = true;
        Queue.push_back(Next);
      }
    }
  }
  if (Returned) {
    std::erase_if(Returned->Spills, [&](const auto &Cell) {
      const auto Before = Caller.Spills.find(Cell.first);
      return Cell.first < *CallerSP || Before == Caller.Spills.end() ||
             Before->second != Cell.second;
    });
    std::erase_if(Returned->FrameBytes,
                  [&](const auto &Byte) { return Byte.first < *CallerSP; });
  }
  return Returned;
}

} // namespace

std::string lowSEHFrameDependencyDigest(const LowFunc &F) {
  llvm::SHA256 Hash;
  Hash.update("neverd-low-seh-frame-cfg-v5");
  const auto Word = [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Value >> (8 * I));
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  };
  const auto Words = [&](const auto &Items) {
    Word(Items.size());
    for (const auto &Item : Items)
      Word(Item);
  };
  Word(F.Entry);
  Word(F.CalleePopBytes);
  Word(F.ExceptionMetadata &&
       (F.ExceptionMetadata->SEH || F.ExceptionMetadata->Cxx ||
        F.ExceptionMetadata->Registration));
  Word(F.ExceptionMetadata.has_value());
  if (F.ExceptionMetadata) {
    const auto &EH = *F.ExceptionMetadata;
    Word(EH.CodeRange.Begin);
    Word(EH.CodeRange.End);
    Word(static_cast<unsigned>(EH.Kind));
    Word(static_cast<unsigned>(EH.Encoding));
    Word(static_cast<unsigned>(EH.ParseStatus));
    Word(EH.UnwindVersion);
    Word(EH.UnwindFlags);
    Word(EH.PrologueSize);
    Word(EH.FrameRegister);
    Word(EH.FrameOffset);
    Word(EH.PrimaryFunctionIndex.has_value());
    if (EH.PrimaryFunctionIndex)
      Word(*EH.PrimaryFunctionIndex);
    Word(EH.ChainedPrimaryRange.has_value());
    if (EH.ChainedPrimaryRange) {
      Word(EH.ChainedPrimaryRange->Begin);
      Word(EH.ChainedPrimaryRange->End);
    }
    Word(EH.ChainedUnwindInfoRVA);
    Word(EH.FragmentRanges.size());
    for (const auto &Range : EH.FragmentRanges) {
      Word(Range.Begin);
      Word(Range.End);
    }
    Word(EH.UnwindOperations.size());
    for (const auto &Op : EH.UnwindOperations) {
      Word(static_cast<unsigned>(Op.Kind));
      Word(Op.CodeOffset);
      Word(Op.StackOffset);
    }
    Word(EH.SEH.has_value());
    if (EH.SEH) {
      Word(EH.SEH->Scopes.size());
      for (const auto &Scope : EH.SEH->Scopes) {
        Word(Scope.GuardedRange.Begin);
        Word(Scope.GuardedRange.End);
        Word(static_cast<unsigned>(Scope.Kind));
        Word(static_cast<unsigned>(Scope.ParseStatus));
        Word(Scope.HandlerVA);
        Word(Scope.FilterOrFinallyVA);
        Word(Scope.ContinuationVA);
      }
    }
    Word(EH.Cxx.has_value());
    if (EH.Cxx) {
      const auto &Cxx = *EH.Cxx;
      Word(Cxx.MaxState);
      Word(Cxx.HasDynamicStackAlignment);
      Word(Cxx.IsSynchronous);
      Word(Cxx.UnwindMap.size());
      for (const auto &Action : Cxx.UnwindMap)
        Word(Action.ToState);
      Word(Cxx.IPMap.size());
      for (const auto &State : Cxx.IPMap) {
        Word(State.IP);
        Word(State.State);
      }
      Word(Cxx.TryBlocks.size());
      for (const auto &Try : Cxx.TryBlocks) {
        Word(Try.TryLow);
        Word(Try.TryHigh);
        Word(Try.CatchHigh);
        Word(Try.Handlers.size());
        for (const auto &Handler : Try.Handlers)
          Word(Handler.HandlerVA);
      }
    }
  }
  Word(F.CxxContinuationEntries.size());
  for (const auto &Entry : F.CxxContinuationEntries) {
    Word(Entry.Target);
    Word(Entry.SourceEntry);
    Word(Entry.ReturnAddr);
    Word(Entry.ReturnSeq);
  }
  Word(F.hasCompleteLiftCoverage());
  Words(F.ModuleAnalysisRoots);
  Words(F.OrdinaryModuleAnalysisRoots);
  Word(F.FunctionTemporaries.size());
  for (const auto &Temporary : F.FunctionTemporaries) {
    Word(Temporary.Offset);
    Word(Temporary.Bytes);
  }
  Word(F.Blocks.size());
  for (const auto &Block : F.Blocks) {
    Word(Block.Id);
    Word(Block.StartAddr);
    Word(Block.EndAddr);
    Hash.update(lowUndefinedOperationDigest(Block.Ops));
    Words(Block.Preds);
    Words(Block.Succs);
    Word(Block.ExceptionalPreds.size());
    const auto EdgeWords = [&](const ExceptionalEdge &Edge) {
      Word(Edge.BlockId);
      Word(Edge.TargetVA);
      Word(static_cast<unsigned>(Edge.Kind));
      if (const auto *Context = Edge.singleContext()) {
        Word(Context->RegionIndex);
        Word(Context->State);
      } else {
        Word(UINT64_MAX);
        Word(Edge.contexts().size());
        Hash.update(Edge.SharedContexts->digest());
      }
      Word(Edge.SourceVA);
    };
    for (const auto &Edge : Block.ExceptionalPreds)
      EdgeWords(Edge);
    Word(Block.ExceptionalSuccs.size());
    for (const auto &Edge : Block.ExceptionalSuccs)
      EdgeWords(Edge);
    Word(Block.InstructionBoundaries.size());
    for (const auto &Boundary : Block.InstructionBoundaries) {
      Word(Boundary.Address);
      Word(Boundary.Size);
      Word(Boundary.FirstOp);
      Word(Boundary.OpCount);
      Word(static_cast<unsigned>(Boundary.Mode));
      Word(static_cast<unsigned>(Boundary.Control));
      Word(static_cast<unsigned>(Boundary.ControlFlags));
      Word(static_cast<unsigned>(Boundary.TargetMode));
      Word(Boundary.Immediate.has_value());
      if (Boundary.Immediate)
        Word(*Boundary.Immediate);
    }
  }
  return llvm::toHex(Hash.final(), true);
}

LowSEHFrameProof
proveLowSEHFrames(const LowFunc &F, const TargetRegInfo &TRI, size_t &Remaining,
                  const std::map<va_t, const LowFunc *> *Callees,
                  const LowSEHRuntimeMemory *RuntimeMemory) {
  if (TRI.TheArch != Arch::X64 || TRI.PointerSize != 8 ||
      TRI.Win64ParamRegs.empty() || F.Blocks.empty() ||
      !F.hasCompleteLiftCoverage() || !F.FunctionTemporaries.empty())
    return {};
  const size_t N = F.Blocks.size();
  CallContext Calls;
  Calls.Callees = Callees;
  Calls.RuntimeMemory = RuntimeMemory;
  Calls.HasExceptionCallbacks =
      F.ExceptionMetadata &&
      (F.ExceptionMetadata->SEH || F.ExceptionMetadata->Cxx ||
       F.ExceptionMetadata->Registration);
  // The entry frame is anchored at its caller's pushed return address. Runtime
  // home areas must not overwrite that live slot, just as nested invocations
  // must preserve each return address introduced by transferCallee.
  if (TRI.CallPushesReturnAddress)
    Calls.ReturnSlots.insert(0);
  if (RuntimeMemory && lowSEHRuntimeMemoryDigest(*RuntimeMemory).empty())
    return {};
  std::map<int, size_t> Positions;
  std::set<va_t> Starts;
  for (size_t I = 0; I < N; ++I)
    if (!consume(Remaining) || F.Blocks[I].Id < 0 ||
        !Positions.emplace(F.Blocks[I].Id, I).second ||
        !Starts.insert(F.Blocks[I].StartAddr).second)
      return {};
  if (!Starts.count(F.Entry) ||
      !completeLowSEHFrameCFG(F, Positions, Remaining))
    return {};
  for (const auto *Roots :
       {&F.ModuleAnalysisRoots, &F.OrdinaryModuleAnalysisRoots})
    for (va_t Root : *Roots)
      if (!consume(Remaining) || !Starts.count(Root))
        return {};
  std::vector<State> Inputs(N), Outputs(N);
  // An in-function SEH body enters with the protected parent's established
  // SP. Reuse the same decoded frame proof as SSA; storage sizing or the mere
  // presence of an exceptional edge cannot supply this coordinate. Only SP
  // is restored here: no saved memory cell receives a runtime-preservation
  // contract from frame establishment.
  std::map<va_t, int64_t> HandlerSP;
  if (F.ExceptionMetadata && F.ExceptionMetadata->SEH) {
    std::map<va_t, std::set<uint32_t>> DeclaredHandlers;
    for (size_t I = 0; I < F.ExceptionMetadata->SEH->Scopes.size(); ++I) {
      const auto &Scope = F.ExceptionMetadata->SEH->Scopes[I];
      if (!consume(Remaining))
        return {};
      if (Scope.Kind != SEHScopeKind::Finally)
        DeclaredHandlers[Scope.HandlerVA].insert(static_cast<uint32_t>(I));
    }
    std::set<va_t> Dependencies;
    for (const auto &Entry : F.SEHLocalUnwindContinuations)
      Dependencies.insert(Entry.CalleeDependencies.begin(),
                          Entry.CalleeDependencies.end());
    const std::vector<va_t> FrameDependencies(Dependencies.begin(),
                                              Dependencies.end());
    std::string FrameDigest;
    if (!F.SEHLocalUnwindContinuations.empty()) {
      if (!chargeFrameMetadata(F, Remaining))
        return {};
      FrameDigest = lowSEHFrameDependencyDigest(
          F, FrameDependencies, Callees,
          F.SEHLocalUnwindContinuations.front().UsesRuntimeMemory
              ? RuntimeMemory
              : nullptr);
    }
    std::vector<va_t> CheckedHandlers;
    for (const auto &Block : F.Blocks) {
      if (!consume(Remaining))
        return {};
      if (Block.ExceptionalPreds.empty() ||
          F.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) ||
          !DeclaredHandlers.count(Block.StartAddr))
        continue;
      bool CheckedEdges = true;
      for (const auto &Edge : Block.ExceptionalPreds) {
        if (!consume(Remaining))
          return {};
        const auto Position = Positions.find(Edge.BlockId);
        const auto *Context = Edge.singleContext();
        if (Edge.Kind != ExceptionalEdgeKind::SEHHandler ||
            Edge.TargetVA != Block.StartAddr || Edge.SourceVA != InvalidVA ||
            !Context ||
            !DeclaredHandlers.at(Block.StartAddr).count(Context->RegionIndex) ||
            Position == Positions.end()) {
          CheckedEdges = false;
          break;
        }
        const auto &Scope =
            F.ExceptionMetadata->SEH->Scopes[Context->RegionIndex];
        const auto Range =
            getSemanticSEHGuardedRange(Scope, Arch::X64, *F.ExceptionMetadata);
        const auto &Source = F.Blocks[Position->second];
        auto Forward = Edge;
        Forward.BlockId = Block.Id;
        if (!consume(Remaining, Source.ExceptionalSuccs.size() + 1))
          return {};
        if (!Range || Scope.ParseStatus != ExceptionParseStatus::Complete ||
            Source.StartAddr >= Range->End || Source.EndAddr <= Range->Begin ||
            !llvm::is_contained(Source.ExceptionalSuccs, Forward)) {
          CheckedEdges = false;
          break;
        }
      }
      if (!CheckedEdges)
        continue;
      CheckedHandlers.push_back(Block.StartAddr);
    }
    auto Frames = proveLowSEHHandlerFrames(F, CheckedHandlers, TRI, Remaining,
                                           FrameDigest);
    if (!Remaining)
      return {};
    for (const auto &[Handler, Frame] : Frames)
      if (!Frame.DynamicSP && Frame.FrameBytes <= uint64_t(INT64_MAX))
        HandlerSP.emplace(Handler, -int64_t(Frame.FrameBytes));
  }
  std::vector<bool> Roots(N), Visited(N), Queued(N);
  std::deque<size_t> Queue;
  const auto Enqueue = [&](size_t I) {
    if (!Queued[I]) {
      Queued[I] = true;
      Queue.push_back(I);
    }
  };
  for (size_t I = 0; I < N; ++I) {
    const auto &Block = F.Blocks[I];
    Roots[I] = Block.StartAddr == F.Entry || Block.Preds.empty() ||
               F.ModuleAnalysisRoots.count(Block.StartAddr) ||
               F.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) ||
               !Block.ExceptionalPreds.empty();
    if (Roots[I])
      Enqueue(I);
  }
  while (!Queue.empty()) {
    if (!consume(Remaining))
      return {};
    const size_t I = Queue.front();
    Queue.pop_front();
    Queued[I] = false;
    const auto &Block = F.Blocks[I];
    std::optional<State> Input;
    if (Roots[I]) {
      Input.emplace();
      if (Block.StartAddr == F.Entry && Block.ExceptionalPreds.empty())
        Input->Values[key(NdVar::reg(TRI.StackPointer, 8))] = Value::frame(0);
      else {
        Input->EscapedFrame = true;
        if (auto It = HandlerSP.find(Block.StartAddr); It != HandlerSP.end())
          Input->Values[key(NdVar::reg(TRI.StackPointer, 8))] =
              Value::frame(It->second);
      }
    }
    for (int Id : Block.Preds) {
      const size_t P = Positions.at(Id);
      if (!consume(Remaining, Outputs[P].Values.size() +
                                  Outputs[P].Spills.size() +
                                  Outputs[P].FrameBytes.size() +
                                  Outputs[P].ImageBytes.size() + 1))
        return {};
      if (!Visited[P])
        continue;
      if (Input)
        Input->intersect(Outputs[P]);
      else
        Input = Outputs[P];
    }
    if (!Input)
      continue;
    State Output = *Input;
    if (!transfer(Block, Output, TRI, Remaining, nullptr, &Calls))
      return {};
    const bool Changed = !Visited[I] || Output != Outputs[I];
    Visited[I] = true;
    Inputs[I] = std::move(*Input);
    Outputs[I] = std::move(Output);
    if (Changed)
      for (int Id : Block.Succs)
        Enqueue(Positions.at(Id));
  }
  LowSEHFrameProof Result;
  for (size_t I = 0; I < N; ++I)
    if (Visited[I] &&
        !transfer(F.Blocks[I], Inputs[I], TRI, Remaining, &Result, &Calls))
      return {};
  if (!Result.Calls.empty()) {
    if (!chargeFrameMetadata(F, Remaining))
      return {};
    for (const auto &Block : F.Blocks)
      if (!consume(Remaining, Block.Ops.size() +
                                  Block.InstructionBoundaries.size() +
                                  Block.Preds.size() + Block.Succs.size() + 1))
        return {};
    for (va_t Dependency : Calls.Dependencies) {
      if (!chargeFrameMetadata(*Callees->at(Dependency), Remaining))
        return {};
      for (const auto &Block : Callees->at(Dependency)->Blocks)
        if (!consume(Remaining,
                     Block.Ops.size() + Block.InstructionBoundaries.size() +
                         Block.Preds.size() + Block.Succs.size() + 1))
          return {};
    }
    Result.CalleeDependencies.assign(Calls.Dependencies.begin(),
                                     Calls.Dependencies.end());
    Result.UsesRuntimeMemory = Calls.UsesRuntimeMemory;
    Result.DependencyDigest = lowSEHFrameDependencyDigest(
        F, Result.CalleeDependencies, Callees,
        Result.UsesRuntimeMemory ? RuntimeMemory : nullptr);
    if (Result.DependencyDigest.empty())
      return {};
  }
  return Result;
}

std::string
lowSEHFrameDependencyDigest(const LowFunc &F, llvm::ArrayRef<va_t> Dependencies,
                            const std::map<va_t, const LowFunc *> *Callees,
                            const LowSEHRuntimeMemory *RuntimeMemory) {
  const auto CallerDigest = lowSEHFrameDependencyDigest(F);
  if (Dependencies.empty() && !RuntimeMemory)
    return CallerDigest;
  if (!Dependencies.empty() && !Callees)
    return {};
  llvm::SHA256 Hash;
  Hash.update("neverd-low-seh-frame-callees-v1");
  Hash.update(CallerDigest);
  if (RuntimeMemory) {
    const auto Digest = lowSEHRuntimeMemoryDigest(*RuntimeMemory);
    if (Digest.empty())
      return {};
    Hash.update("runtime-memory");
    Hash.update(Digest);
  }
  std::set<va_t> Seen;
  for (va_t Entry : Dependencies) {
    const auto It = Callees->find(Entry);
    if (Entry == F.Entry || !Seen.insert(Entry).second ||
        It == Callees->end() || !It->second || It->second->Entry != Entry)
      return {};
    Hash.update(lowSEHFrameDependencyDigest(*It->second));
  }
  return llvm::toHex(Hash.final(), true);
}

} // namespace neverd
