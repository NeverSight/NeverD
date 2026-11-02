//===- AArch64SVEState.cpp - Local scalable state admission --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove complete local Z/P definitions before either backend can resolve an
/// unknown register as an ordinary scalar. COPY/PHI transport retains bits;
/// element widths belong to individual operations, not the stored value.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/AArch64/AArch64SVEState.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/ADT/BitVector.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {

using namespace aarch64_sve;

#define NEVERD_A64_SVE_STATE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "neverd/ir/med/AArch64/AArch64SVEState.def"

using Key = std::tuple<MedVar::VarKind, int, int>;

Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }

std::optional<unsigned> registerIndex(const MedVar &Value, uint64_t Base,
                                      unsigned Bytes, unsigned Count) {
  if (Value.Kind != MedVar::Reg || Value.RegOff < Base ||
      Value.RegOff - Base >= uint64_t{Bytes} * Count)
    return std::nullopt;
  return static_cast<unsigned>((Value.RegOff - Base) / Bytes);
}

bool physicalState(const MedVar &Value) {
  return registerIndex(Value, kVectorRegisterBase, kVectorStorageBytes,
                       kVectorRegisterCount) ||
         registerIndex(Value, kPredicateRegisterBase, kPredicateStorageBytes,
                       kPredicateRegisterCount + 1);
}

bool candidate(const MedVar &Value) {
  return Value.Size &&
         (Value.Size == kVectorStorageBytes ||
          Value.Size == kPredicateStorageBytes || physicalState(Value));
}

std::optional<Intrinsic> intrinsic(const MedOp &Op) {
  if (!hasValidMedIntrinsicId(Op) ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count))
    return std::nullopt;
  return static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
}

bool stateIntrinsic(const MedOp &Op) {
  const auto Id = intrinsic(Op);
  return Id && (*Id == Intrinsic::A64_SvePtrue || vectorOperation(*Id));
}

bool scalarStateIntrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_SVE_STATE_SCALAR(Id) case Intrinsic::Id:
#include "neverd/ir/med/AArch64/AArch64SVEState.def"
    return true;
  default:
    return false;
  }
}

struct ValueInfo {
  MedVar Value;
  bool Conflict = false;
  bool Seed = false;
  unsigned Definitions = 0;
  unsigned Index = 0;
  const MedOp *Operation = nullptr;
  const PhiNode *Phi = nullptr;
  std::vector<Key> Copies;
};

bool sameStorage(const MedVar &Left, const MedVar &Right) {
  return Left.Size == Right.Size && Left.TheArch == Right.TheArch &&
         (Left.Kind != MedVar::Reg || Left.RegOff == Right.RegOff);
}

} // namespace

const char *validateAArch64SVEState(const MedFunc &Func, Arch Architecture) {
  bool HasState = false;
  for (const MedBlock &Block : Func.Blocks) {
    if (HasState)
      break;
    for (const PhiNode &Phi : Block.Phis) {
      if (HasState)
        break;
      HasState |= Architecture == Arch::AArch64 && physicalState(Phi.Output);
      for (const auto &[Pred, Value] : Phi.Args) {
        HasState |= Architecture == Arch::AArch64 && physicalState(Value);
        if (HasState)
          break;
      }
    }
    for (const MedOp &Op : Block.Ops) {
      if (HasState)
        break;
      HasState |= stateIntrinsic(Op);
      if (Architecture != Arch::AArch64)
        continue;
      HasState |= physicalState(Op.Output);
      for (const MedVar &Value : Op.IntrinsicOutputs)
        HasState |= physicalState(Value);
      for (unsigned I = 0; I < std::min<size_t>(Op.NumInputs, Op.Inputs.size());
           ++I)
        HasState |= physicalState(Op.Inputs[I]);
    }
  }
  if (!HasState)
    return nullptr;
  if (Architecture != Arch::AArch64 || Func.SkippedSSA || Func.Blocks.empty())
    return kStateMalformed;
  using namespace limits::ir;
  if (Func.Blocks.size() > kSVEStateMaxBlocks)
    return kStateLimit;

  std::map<Key, ValueInfo> Values;
  std::set<unsigned> VectorRegisters, NeonRegisters;
  size_t Work = 0;
  bool LimitExceeded = false;
  auto touch = [&](const MedVar &Value) -> ValueInfo * {
    if (const auto Index =
            registerIndex(Value, kVectorRegisterBase, kVectorStorageBytes,
                          kVectorRegisterCount))
      VectorRegisters.insert(*Index);
    if (const auto Index = registerIndex(
            Value, kNeonRegisterBase, kNeonStorageBytes, kVectorRegisterCount))
      NeonRegisters.insert(*Index);
    if (!Value.Size)
      return nullptr;
    if (++Work > kSVEStateMaxWork || Values.size() >= kSVEStateMaxValues) {
      LimitExceeded = true;
      return nullptr;
    }
    auto [It, Inserted] = Values.try_emplace(key(Value));
    ValueInfo &Info = It->second;
    if (Inserted)
      Info.Value = Value;
    else
      Info.Conflict |= !sameStorage(Info.Value, Value);
    Info.Seed |= physicalState(Value);
    return &Info;
  };
  auto connect = [&](const MedVar &Left, const MedVar &Right) {
    auto *L = touch(Left);
    auto *R = touch(Right);
    if (L && R) {
      L->Copies.push_back(key(Right));
      R->Copies.push_back(key(Left));
    }
  };
  for (const MedVar &Parameter : Func.Params) {
    if (++Work > kSVEStateMaxWork)
      return kStateLimit;
    if (candidate(Parameter))
      return kStateABI;
  }
  if (Func.ReturnType && candidate(MedVar::makeConst(0, Func.ReturnType->Size)))
    return kStateABI;
  for (const MedCallClobber &Clobber : Func.CallClobbers) {
    if (++Work > kSVEStateMaxWork)
      return kStateLimit;
    if (candidate(Clobber.Value) || candidate(Clobber.PreservedInput))
      return kStateCall;
  }

  for (size_t B = 0; B != Func.Blocks.size(); ++B) {
    const MedBlock &Block = Func.Blocks[B];
    if (Block.Id != static_cast<int>(B) || !Block.ExceptionalPreds.empty() ||
        !Block.ExceptionalSuccs.empty())
      return kStateMalformed;
    for (const PhiNode &Phi : Block.Phis) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      if (auto *Info = touch(Phi.Output)) {
        ++Info->Definitions;
        Info->Phi = &Phi;
      }
      for (const auto &[Pred, Value] : Phi.Args) {
        connect(Phi.Output, Value);
        if (LimitExceeded)
          return kStateLimit;
      }
    }
    for (const MedOp &Op : Block.Ops) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      if (Op.NumInputs > Op.Inputs.size())
        return kStateMalformed;
      const bool Branch = Op.Opcode == NdOp::BRANCH;
      const bool Conditional = Op.Opcode == NdOp::COND_BR;
      const bool Return = Op.Opcode == NdOp::RETURN;
      if ((Branch || Conditional || Return) &&
          (&Op != &Block.Ops.back() || Op.Dead))
        return kStateMalformed;
      if ((Branch && (Op.NumInputs != 1 || Block.Succs.size() != 1)) ||
          (Conditional && (Op.NumInputs != 2 || Block.Succs.empty() ||
                           Block.Succs.size() > 2)) ||
          (Return && !Block.Succs.empty()))
        return kStateMalformed;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INDIR_BR)
        return kStateCall;
      const auto Id = intrinsic(Op);
      const auto Vector = Id ? vectorOperation(*Id) : std::nullopt;
      const bool Predicate = Id && *Id == Intrinsic::A64_SvePtrue;
      // Valid PAuth operations only write their explicit scalar result. Reuse
      // that contract without admitting opaque calls or unresolved PACM.
      const bool PAuth = aarch64PAuthMedShapeIsValid(Op);
      const bool OriginalMemory = aarch64AddressMemoryMedShapeIsValid(Op);
      if (Op.Opcode == NdOp::INTRINSIC &&
          (!Id || (!Vector && !Predicate && !PAuth && !OriginalMemory &&
                   !scalarStateIntrinsic(*Id))))
        return kStateCall;
      if ((Vector && !aarch64VectorMedShapeIsValid(Op)) ||
          (Predicate && !aarch64PtrueMedShapeIsValid(Op)))
        return kStateMalformed;
      if (auto *Info = touch(Op.Output)) {
        ++Info->Definitions;
        Info->Operation = &Op;
        Info->Seed |= Predicate || (Vector && Vector->ProducesVector);
      }
      for (const MedVar &Output : Op.IntrinsicOutputs)
        if (auto *Info = touch(Output))
          ++Info->Definitions;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        if (auto *Info = touch(Op.Inputs[I]);
            Info && Vector && I && Vector->consumesState(I - 1))
          Info->Seed = true;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        connect(Op.Output, Op.Inputs[0]);
    }
    if (LimitExceeded)
      return kStateLimit;
    if ((Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::COND_BR) &&
        Block.Succs.size() > 1)
      return kStateMalformed;
  }
  for (unsigned Register : VectorRegisters)
    if (NeonRegisters.count(Register))
      return kStateAlias;

  std::set<Key> Tracked;
  std::vector<Key> Pending;
  for (const auto &[K, Info] : Values)
    if (Info.Seed)
      Pending.push_back(K);
  while (!Pending.empty()) {
    if (++Work > kSVEStateMaxWork)
      return kStateLimit;
    const auto K = Pending.back();
    Pending.pop_back();
    if (!Tracked.insert(K).second)
      continue;
    auto &Info = Values.at(K);
    const MedVar &Value = Info.Value;
    if (Info.Conflict || Value.Id < 0 || Value.SSAVer < 0 ||
        (Value.TheArch != Arch::Unknown && Value.TheArch != Architecture))
      return kStateMalformed;
    if (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp)
      return kStateABI;
    if (Info.Definitions != 1)
      return Info.Definitions ? kStateMalformed : kStateUnknown;
    if (Value.Size != kVectorStorageBytes &&
        Value.Size != kPredicateStorageBytes)
      return kStateTransport;
    if (const auto Index =
            registerIndex(Value, kVectorRegisterBase, kVectorStorageBytes,
                          kVectorRegisterCount)) {
      if (Value.Size != kVectorStorageBytes ||
          Value.RegOff != kVectorRegisterBase + *Index * kVectorStorageBytes)
        return kStateMalformed;
    }
    if (const auto Index =
            registerIndex(Value, kPredicateRegisterBase, kPredicateStorageBytes,
                          kPredicateRegisterCount + 1)) {
      if (*Index == kPredicateRegisterCount)
        return kStateTransport;
      if (Value.Size != kPredicateStorageBytes ||
          Value.RegOff !=
              kPredicateRegisterBase + *Index * kPredicateStorageBytes)
        return kStateMalformed;
    }
    if (Info.Operation) {
      const MedOp &Op = *Info.Operation;
      if (Op.Dead)
        return kStateUnknown;
      const auto Id = intrinsic(Op);
      const auto Vector = Id ? vectorOperation(*Id) : std::nullopt;
      const bool Producer = (Id && *Id == Intrinsic::A64_SvePtrue) ||
                            (Vector && Vector->ProducesVector);
      const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Inputs[0].Size == Value.Size &&
                        Op.IntrinsicOutputs.empty() &&
                        Op.MemoryOrdering == NdMemoryOrdering::None &&
                        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      if (!Producer && !Copy)
        return kStateTransport;
    } else if (!Info.Phi) {
      return kStateTransport;
    } else if (Info.Phi->ExceptionalEntry) {
      return kStateUnknown;
    }
    Pending.insert(Pending.end(), Info.Copies.begin(), Info.Copies.end());
  }
  unsigned Count = 0;
  for (const auto &K : Tracked)
    Values.at(K).Index = Count++;
  auto tracked = [&](const MedVar &Value) -> const ValueInfo * {
    if (!Value.Size)
      return nullptr;
    const auto It = Values.find(key(Value));
    return It != Values.end() && Tracked.count(It->first) ? &It->second
                                                          : nullptr;
  };

  // Require a single ordinary CFG entry. Exceptional roots need their own
  // scalable state contract; they cannot inherit this entry's local values.
  if (!Func.Blocks.front().Preds.empty())
    return kStateMalformed;
  std::vector<std::set<int>> Successors(Func.Blocks.size());
  std::vector<std::set<int>> Predecessors(Func.Blocks.size());
  for (const auto &Block : Func.Blocks) {
    auto collect = [&](const std::vector<int> &Edges,
                       std::set<int> &Set) -> const char * {
      for (int Edge : Edges) {
        if (++Work > kSVEStateMaxWork)
          return kStateLimit;
        if (Edge < 0 || static_cast<size_t>(Edge) >= Func.Blocks.size() ||
            !Set.insert(Edge).second)
          return kStateMalformed;
      }
      return nullptr;
    };
    if (const char *Error = collect(Block.Succs, Successors[Block.Id]))
      return Error;
    if (const char *Error = collect(Block.Preds, Predecessors[Block.Id]))
      return Error;
  }
  for (const auto &Block : Func.Blocks) {
    for (int Next : Successors[Block.Id]) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      if (!Predecessors[Next].count(Block.Id))
        return kStateMalformed;
    }
    for (int Prev : Predecessors[Block.Id]) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      if (!Successors[Prev].count(Block.Id))
        return kStateMalformed;
    }
  }
  // Both native lowering and source structuring name an ordinary block by
  // StartAddr, falling back to its first operation's address. Require unique
  // identities and an exact target witness rather than either emitter's
  // heuristic fallback for a contradictory branch target.
  std::map<va_t, int> AddressBlocks;
  std::vector<va_t> Addresses;
  for (const auto &Block : Func.Blocks) {
    if (++Work > kSVEStateMaxWork)
      return kStateLimit;
    const va_t Address = Block.StartAddr || Block.Ops.empty()
                             ? Block.StartAddr
                             : Block.Ops.front().Addr;
    if (!AddressBlocks.emplace(Address, Block.Id).second)
      return kStateMalformed;
    Addresses.push_back(Address);
  }
  for (const auto &Block : Func.Blocks) {
    if (++Work > kSVEStateMaxWork)
      return kStateLimit;
    if (Block.Ops.empty())
      continue;
    const MedOp &Last = Block.Ops.back();
    if (Last.Opcode != NdOp::BRANCH && Last.Opcode != NdOp::COND_BR)
      continue;
    if (!Last.Inputs.front().isConst() ||
        (Last.Opcode == NdOp::COND_BR && Block.Succs.size() == 2 &&
         Last.Inputs.front().ConstVal == 0))
      return kStateMalformed;
    const auto Target = AddressBlocks.find(Last.Inputs.front().ConstVal);
    if (Target == AddressBlocks.end() ||
        !Successors[Block.Id].count(Target->second))
      return kStateMalformed;
    if (Last.Opcode == NdOp::COND_BR && Block.Succs.size() == 1 &&
        Block.EndAddr != Addresses[Target->second])
      return kStateMalformed;
  }
  std::vector<bool> Reachable(Func.Blocks.size());
  std::vector<unsigned> Blocks{0};
  while (!Blocks.empty()) {
    if (++Work > kSVEStateMaxWork)
      return kStateLimit;
    const unsigned B = Blocks.back();
    Blocks.pop_back();
    if (Reachable[B])
      continue;
    Reachable[B] = true;
    for (int Next : Successors[B])
      Blocks.push_back(Next);
  }
  if (std::find(Reachable.begin(), Reachable.end(), false) != Reachable.end())
    return kStateMalformed;
  // An N-bit must-defined set per block is sufficient: the unique SSA
  // definition is either on every incoming path or cannot be consumed here.
  if (Count && Func.Blocks.size() > kSVEStateMaxBits / Count)
    return kStateLimit;
  std::vector<llvm::BitVector> Generated(Func.Blocks.size(),
                                         llvm::BitVector(Count));
  std::vector<llvm::BitVector> Incoming = Generated;
  std::vector<llvm::BitVector> Outgoing(Func.Blocks.size(),
                                        llvm::BitVector(Count, true));
  for (const auto &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      if (const auto *Info = tracked(Phi.Output))
        Generated[Block.Id].set(Info->Index);
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      if (const auto *Info = tracked(Op.Output))
        Generated[Block.Id].set(Info->Index);
    }
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const auto &Block : Func.Blocks) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      auto &In = Incoming[Block.Id];
      In.reset();
      if (Block.Id != 0) {
        In.set();
        for (int Pred : Block.Preds) {
          if ((Work += Count) > kSVEStateMaxWork)
            return kStateLimit;
          In &= Outgoing[Pred];
        }
      }
      auto Next = In;
      Next |= Generated[Block.Id];
      if (Next != Outgoing[Block.Id]) {
        Outgoing[Block.Id] = std::move(Next);
        Changed = true;
      }
    }
  }
  for (const auto &Block : Func.Blocks) {
    auto Available = Incoming[Block.Id];
    for (const auto &Phi : Block.Phis) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      const auto *Output = tracked(Phi.Output);
      if (!Output)
        continue;
      std::set<int> Seen;
      if (Phi.Args.size() != Block.Preds.size() || Phi.Args.empty())
        return kStateMalformed;
      for (const auto &[Pred, Value] : Phi.Args) {
        if (++Work > kSVEStateMaxWork)
          return kStateLimit;
        const auto *Input = tracked(Value);
        if (!Seen.insert(Pred).second || !Predecessors[Block.Id].count(Pred) ||
            !Input || Value.Size != Phi.Output.Size)
          return kStateMalformed;
        if (!Outgoing[Pred].test(Input->Index))
          return kStateUnknown;
      }
      Available.set(Output->Index);
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > kSVEStateMaxWork)
        return kStateLimit;
      const auto Id = intrinsic(Op);
      const auto Vector = Id ? vectorOperation(*Id) : std::nullopt;
      const auto *Output = tracked(Op.Output);
      for (unsigned I = 0; I != Op.NumInputs; ++I) {
        if (++Work > kSVEStateMaxWork)
          return kStateLimit;
        const auto *Input = tracked(Op.Inputs[I]);
        if (!Input)
          continue;
        const bool Copy = Output && Op.Opcode == NdOp::COPY && I == 0;
        const bool Consume = Vector && I && Vector->consumesState(I - 1);
        if (!Copy && !Consume)
          return Op.Opcode == NdOp::RETURN ? kStateABI : kStateTransport;
        if (!Available.test(Input->Index))
          return kStateUnknown;
      }
      if (Output)
        Available.set(Output->Index);
    }
  }
  return nullptr;
}

} // namespace neverd
