//===- MedMutableSource.cpp - Bounded mutable source contract ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedMutableSource.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/BitVector.h"

#include <algorithm>
#include <deque>
#include <limits>

namespace neverd {
namespace {
constexpr size_t MaxBlocks = 4096;
constexpr size_t MaxValues = 4096;
constexpr size_t MaxOperations = 8 * 1024 * 1024;
constexpr size_t MaxBlockValues = 4 * 1024 * 1024;
constexpr size_t MaxPropagationWords = 16 * 1024 * 1024;

bool scalarWidth(unsigned Bytes) {
  return Bytes == 1 || Bytes == 2 || Bytes == 4 || Bytes == 8 || Bytes == 16;
}

uint64_t byteMask(unsigned Bytes) {
  return Bytes == 64 ? ~uint64_t(0) : (uint64_t(1) << Bytes) - 1;
}

// Only explicit, unconditional scalar definitions belong to this subset.
// Intrinsics/calls can carry implicit or conditional outputs and need their
// own contracts. NOP/dead outputs are never definitions.
int inputCount(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::LOAD:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
  case NdOp::BOOL_NOT:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
    return 1;
  case NdOp::STORE:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
    return 2;
  case NdOp::SELECT:
    return 3;
  default:
    return -1;
  }
}

bool compatibleWidths(const MedOp &O, unsigned PointerSize) {
  switch (O.Opcode) {
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
    return O.Inputs[0].Size <= 8;
  case NdOp::LOAD:
  case NdOp::STORE:
    return O.Inputs[0].Size == PointerSize;
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return O.Inputs[1].Size <= O.Inputs[0].Size;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return O.Inputs[0].Size == O.Inputs[1].Size;
  case NdOp::SELECT:
    return O.Inputs[1].Size == O.Inputs[2].Size;
  default:
    return true;
  }
}
} // namespace

std::optional<MedMutableSourcePlan>
analyzeMedMutableSource(const MedFunc &F, Arch Architecture,
                        std::string *Error) {
  const auto Fail =
      [&](const std::string &Reason) -> std::optional<MedMutableSourcePlan> {
    if (Error)
      *Error = Reason;
    return std::nullopt;
  };
  if (Error)
    Error->clear();
  if (!F.SkippedSSA || F.Blocks.empty() || F.Blocks.size() > MaxBlocks ||
      !F.ReturnType || F.Blocks.front().StartAddr != F.Entry)
    return Fail("mutable source has no bounded explicit entry");
  if (F.DoesNotReturn || F.Params.size() > MaxValues || F.ExceptionMetadata ||
      !F.CallInfos.empty() || !F.CallClobbers.empty() ||
      !F.StructReturnCandidates.empty() || !F.MultiReturn.empty() ||
      !F.JumpTables.empty() || !F.SwitchSelectorPlans.empty() ||
      !F.ScalarAddressModels.empty() || !F.I386GetPcModels.empty() ||
      !F.CxxContinuationExits.empty() || F.FPReturnViaX87 || F.IsVariadic ||
      !F.MutableStackParamHomes.empty() || F.FrameSize || F.FrameHeadroom)
    return Fail("mutable source has unsupported implicit state");
  const auto &TRI = getTargetRegInfo(Architecture);
  if (!TRI.PointerSize || (F.ReturnType->Kind != NdTypeKind::Void &&
                           (F.ReturnType->Kind != NdTypeKind::Int ||
                            !scalarWidth(F.ReturnType->Size) ||
                            F.ReturnType->Size > TRI.PointerSize)))
    return Fail("mutable source requires a standard scalar return");

  MedMutableSourcePlan Plan;
  std::map<int, size_t> Indices;
  bool Valid = true;
  const auto Observe = [&](const MedVar &V) {
    if (!scalarWidth(V.Size)) {
      Valid = false;
      return;
    }
    if (V.isConst()) {
      if (V.Provenance == ConstantAddressProvenance::AddressFragment ||
          (isExactAddressProvenance(V.Provenance) && V.Size < TRI.PointerSize))
        Valid = false;
      return;
    }
    if (V.Id < 0 || V.SSAVer != 0 ||
        (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
         V.Kind != MedVar::Flag && V.Kind != MedVar::Param) ||
        ((V.Kind == MedVar::Reg || V.Kind == MedVar::Param) &&
         V.RegOff > std::numeric_limits<uint64_t>::max() - V.Size)) {
      Valid = false;
      return;
    }
    auto It = Indices.find(V.Id);
    if (It == Indices.end()) {
      if (Plan.Variables.size() >= MaxValues ||
          Plan.Variables.size() + 1 > MaxBlockValues / F.Blocks.size()) {
        Valid = false;
        return;
      }
      Indices.emplace(V.Id, Plan.Variables.size());
      Plan.Variables.push_back(V);
    } else {
      const auto &Old = Plan.Variables[It->second];
      Valid &= Old.Kind == V.Kind && Old.Size == V.Size &&
               ((V.Kind != MedVar::Reg && V.Kind != MedVar::Param) ||
                Old.RegOff == V.RegOff);
    }
  };
  std::map<uint64_t, const MedVar *> Parameters;
  if (F.TypedParams.size() > F.Params.size())
    return Fail("mutable source has excess parameter types");
  for (size_t I = 0; I < F.Params.size(); ++I) {
    const auto &P = F.Params[I];
    if ((P.Kind != MedVar::Param && P.Kind != MedVar::Reg) || P.SSAVer ||
        P.Id < -1 || !scalarWidth(P.Size) ||
        P.RegOff > std::numeric_limits<uint64_t>::max() - P.Size ||
        !Parameters.emplace(P.RegOff, &P).second)
      return Fail("mutable source has ambiguous parameter bindings");
    if (TRI.isFPArgReg(P.RegOff))
      return Fail("mutable source does not support vector-register parameters");
    if (I < F.TypedParams.size()) {
      const auto &T = F.TypedParams[I].Type;
      if (!T || (T->Kind != NdTypeKind::Int && T->Kind != NdTypeKind::Ptr) ||
          T->Size != P.Size ||
          (T->Kind == NdTypeKind::Ptr && T->Size != TRI.PointerSize))
        return Fail("mutable source parameter type disagrees with its carrier");
    }
  }
  std::set<va_t> Addresses;
  size_t RemainingOps = MaxOperations;
  bool HasReturn = false;
  // Bound every adjacency list before any reverse-edge scan can visit it.
  for (const auto &B : F.Blocks) {
    if (B.Ops.size() > RemainingOps)
      return Fail("mutable source operation budget exhausted: block " +
                  std::to_string(B.Id) + " needs " +
                  std::to_string(B.Ops.size()) + " operations with " +
                  std::to_string(RemainingOps) + " remaining");
    if (B.Succs.size() > 2 || B.Preds.size() > MaxBlocks)
      return Fail("mutable source CFG metadata exceeds its budget: block " +
                  std::to_string(B.Id) + " has " +
                  std::to_string(B.Succs.size()) + " successors and " +
                  std::to_string(B.Preds.size()) + " predecessors");
    RemainingOps -= B.Ops.size();
  }
  for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
    const auto &B = F.Blocks[BI];
    if (B.Id != static_cast<int>(BI) || !B.StartAddr ||
        !Addresses.insert(B.StartAddr).second || !B.Phis.empty() ||
        !B.ExceptionalPreds.empty() || !B.ExceptionalSuccs.empty() ||
        B.Ops.empty())
      return Fail("mutable source has unsupported block metadata");
    for (bool Successors : {false, true}) {
      const auto &Edges = Successors ? B.Succs : B.Preds;
      std::set<int> Seen;
      for (int ID : Edges) {
        if (ID < 0 || static_cast<size_t>(ID) >= F.Blocks.size() ||
            !Seen.insert(ID).second)
          return Fail("mutable source has invalid CFG edges");
        const auto &Reverse =
            Successors ? F.Blocks[ID].Preds : F.Blocks[ID].Succs;
        if (std::count(Reverse.begin(), Reverse.end(), B.Id) != 1)
          return Fail("mutable source has asymmetric CFG edges");
      }
    }
    for (size_t OI = 0; OI < B.Ops.size(); ++OI) {
      const auto &O = B.Ops[OI];
      const bool Control = O.Opcode == NdOp::BRANCH ||
                           O.Opcode == NdOp::COND_BR ||
                           O.Opcode == NdOp::RETURN;
      if (O.NumInputs > O.Inputs.size() || O.NumInputs > 6 ||
          !O.IntrinsicOutputs.empty() || O.CallSiteId || O.SourceCallHint ||
          O.DoesNotReturn || O.MemoryOrdering != NdMemoryOrdering::None ||
          O.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return Fail("mutable source has unsupported operation metadata");
      if (Control) {
        if (O.Dead || OI + 1 != B.Ops.size() || O.Output.Size)
          return Fail("mutable source has nonterminal or dead control");
        if (O.Opcode == NdOp::RETURN) {
          if (!B.Succs.empty() || O.NumInputs > 1)
            return Fail("mutable source has an invalid return");
          HasReturn = true;
        } else {
          const unsigned Count = O.Opcode == NdOp::BRANCH ? 1 : 2;
          if (O.NumInputs != Count || B.Succs.size() != Count ||
              !O.Inputs[0].isConst() || !O.Inputs[0].Size ||
              (O.Inputs[0].Size < 8 &&
               O.Inputs[0].ConstVal >> (O.Inputs[0].Size * 8)) ||
              std::none_of(B.Succs.begin(), B.Succs.end(), [&](int ID) {
                return F.Blocks[ID].StartAddr == O.Inputs[0].ConstVal;
              }))
            return Fail("mutable source control disagrees with its CFG");
        }
      } else if (OI + 1 == B.Ops.size()) {
        return Fail("mutable source requires explicit terminal control");
      }
      if (O.Dead || O.Opcode == NdOp::NOP)
        continue;
      if (!Control) {
        const int Count = inputCount(O.Opcode);
        if (Count < 0 || O.NumInputs != Count ||
            (O.Opcode == NdOp::STORE ? O.Output.Size != 0 : O.Output.Size == 0))
          return Fail("mutable source has no explicit operation contract");
        if (!compatibleWidths(O, TRI.PointerSize))
          return Fail("mutable source has unsupported mixed operand widths");
        if (O.Opcode == NdOp::SUBBYTES &&
            (!O.Inputs[1].isConst() ||
             O.Inputs[1].ConstVal > O.Inputs[0].Size ||
             O.Output.Size > O.Inputs[0].Size - O.Inputs[1].ConstVal))
          return Fail("mutable source requires an exact in-range byte slice");
        if (O.Opcode == NdOp::CONCAT &&
            O.Output.Size != O.Inputs[0].Size + O.Inputs[1].Size)
          return Fail("mutable source has inconsistent concatenation widths");
        if (O.Output.Size) {
          if (O.Output.Kind == MedVar::Param || O.Output.isConst())
            return Fail("mutable source writes a readonly value");
          Observe(O.Output);
        }
      }
      for (unsigned I = 0; I < O.NumInputs; ++I)
        Observe(O.Inputs[I]);
      if (!Valid)
        return Fail("mutable source has inconsistent storage identities");
    }
  }
  if (!HasReturn)
    return Fail("mutable source has no explicit return");
  std::map<uint64_t, const MedVar *> Registers;
  for (const auto &V : Plan.Variables) {
    if (V.Kind == MedVar::Param) {
      auto P = Parameters.find(V.RegOff);
      if (P == Parameters.end() || P->second->Size != V.Size)
        return Fail("mutable source reads an unbound explicit parameter");
    }
    if (V.Kind != MedVar::Reg)
      continue;
    auto [It, Added] = Registers.emplace(V.RegOff, &V);
    if (!Added ||
        (It != Registers.begin() &&
         std::prev(It)->first + std::prev(It)->second->Size > V.RegOff) ||
        (std::next(It) != Registers.end() &&
         V.RegOff + V.Size > std::next(It)->first))
      return Fail("mutable source has overlapping independent registers");
    if (V.RegOff == TRI.IntReturnReg &&
        F.ReturnType->Kind != NdTypeKind::Void) {
      if (V.Size < F.ReturnType->Size)
        return Fail("mutable source has a partial return carrier");
      Plan.ReturnValue = V;
    }
  }
  if (F.ReturnType->Kind != NdTypeKind::Void && !Plan.ReturnValue)
    return Fail("mutable source has no unique scalar return carrier");

  struct BlockState {
    llvm::BitVector Gen, Kill, In;
    explicit BlockState(size_t N) : Gen(N), Kill(N), In(N) {}
  };
  std::vector<BlockState> States;
  States.reserve(F.Blocks.size());
  for (const auto &B : F.Blocks) {
    States.emplace_back(Plan.Variables.size());
    auto &S = States.back();
    const auto Read = [&](const MedVar &V) {
      if (!V.isConst()) {
        size_t I = Indices.at(V.Id);
        if (!S.Kill.test(I))
          S.Gen.set(I);
      }
    };
    for (const auto &O : B.Ops) {
      if (O.Dead || O.Opcode == NdOp::NOP)
        continue;
      for (unsigned I = 0; I < O.NumInputs; ++I)
        Read(O.Inputs[I]);
      if (O.Opcode == NdOp::RETURN && Plan.ReturnValue)
        Read(*Plan.ReturnValue);
      if (O.Output.Size)
        S.Kill.set(Indices.at(O.Output.Id));
    }
    S.In = S.Gen;
  }
  std::deque<int> Pending;
  llvm::BitVector Queued(F.Blocks.size(), true);
  for (const auto &B : F.Blocks)
    Pending.push_back(B.Id);
  size_t Remaining = MaxPropagationWords;
  while (!Pending.empty()) {
    const int ID = Pending.front();
    Pending.pop_front();
    Queued.reset(ID);
    const auto &B = F.Blocks[ID];
    auto &S = States[ID];
    const size_t Cost =
        (Plan.Variables.size() / 64 + 1) * (B.Succs.size() + 3) +
        B.Preds.size();
    if (Cost > Remaining)
      return Fail("mutable source dataflow budget exhausted");
    Remaining -= Cost;
    llvm::BitVector Next(Plan.Variables.size());
    for (int Successor : B.Succs)
      Next |= States[Successor].In;
    Next.reset(S.Kill);
    Next |= S.Gen;
    if (Next == S.In)
      continue;
    S.In = std::move(Next);
    for (int Predecessor : B.Preds)
      if (!Queued.test(Predecessor)) {
        Queued.set(Predecessor);
        Pending.push_back(Predecessor);
      }
  }
  for (int I : States.front().In.set_bits()) {
    const auto &V = Plan.Variables[I];
    if (V.Kind != MedVar::Reg && V.Kind != MedVar::Param)
      return Fail("mutable source reads undefined entry storage");
    if (auto P = Parameters.find(V.RegOff);
        P != Parameters.end() && V.Size > P->second->Size)
      return Fail("mutable source reads beyond its entry parameter width");
    Plan.EntryValues.insert(V.Id);
    Plan.EntryBytes[V.RegOff] |= byteMask(V.Size);
  }
  // Absent graph occurrences cannot erase a declared entry parameter.
  for (const auto &[Offset, P] : Parameters)
    if (std::none_of(
            Plan.Variables.begin(), Plan.Variables.end(), [&](const MedVar &V) {
              return (V.Kind == MedVar::Reg || V.Kind == MedVar::Param) &&
                     V.RegOff == Offset;
            }))
      Plan.EntryBytes[Offset] |= byteMask(P->Size);
  return Plan;
}
} // namespace neverd
