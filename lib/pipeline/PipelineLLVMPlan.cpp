//===- PipelineLLVMPlan.cpp - LLVM emission dependency groups -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable work-budget shards that preserve module-local emission dependencies.
///
//===----------------------------------------------------------------------===//

#include "PipelineLLVMDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedCodeIdentity.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntEqClasses.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::pipeline_detail {
namespace {

void visitAddressInputs(const MedFunc &Func,
                        llvm::function_ref<void(const MedVar &)> Visit) {
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis) {
      for (const auto &[Predecessor, Value] : Phi.Args)
        Visit(Value);
      if (Phi.ExceptionalEntry)
        Visit(*Phi.ExceptionalEntry);
    }
    for (const MedOp &Op : Block.Ops)
      for (size_t I = 0; I < std::min<size_t>(Op.NumInputs, Op.Inputs.size());
           ++I)
        Visit(Op.Inputs[I]);
  }
  for (const MedCallInfo &Call : Func.CallInfos)
    for (const MedVar &Argument : Call.Args)
      Visit(Argument);
  for (const auto &[Key, Plan] : Func.SwitchSelectorPlans) {
    Visit(Plan.Selector);
    Visit(Plan.Condition);
    for (const auto &[Edge, Selector] : Plan.EdgeSelectors)
      Visit(Selector);
  }
  for (const MedScalarAddressModel &Model : Func.ScalarAddressModels)
    Visit(Model.Value);
  for (const MedI386GetPcModel &Model : Func.I386GetPcModels) {
    Visit(Model.Output);
    Visit(Model.Value);
  }
  for (const MedStructReturnCandidate &Candidate : Func.StructReturnCandidates)
    for (const MedVar &Field : Candidate.Fields)
      Visit(Field);
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    Visit(Clobber.PreservedInput);
  for (const MedCxxContinuationExitEvidence &Exit : Func.CxxContinuationExits) {
    Visit(Exit.ReturnValue);
    for (va_t Target : Exit.Targets)
      Visit(
          MedVar::makeConst(Target, 0, ConstantAddressProvenance::CodeAddress));
  }
  for (const auto &[Entry, Registers] : Func.SEHFuncletRegisters)
    for (const auto &[Register, Value] : Registers)
      if (Value.ValueKind == SEHFuncletRegisterValue::Kind::Constant)
        Visit(MedVar::makeConst(Value.Value, 0, Value.Provenance,
                                Value.AddressOwnerVA));
}

struct CodeExtent {
  va_t Begin;
  va_t Last;
  unsigned Function;
};

llvm::IntEqClasses emissionGroups(const std::vector<MedFunc> &Funcs,
                                  const BinaryImage *Image) {
  llvm::IntEqClasses Groups(static_cast<unsigned>(Funcs.size()));
  // The tag keeps every virtual address available, including DenseMap's
  // integer sentinel values. Lookup order never controls the shard order.
  using AddressKey = std::pair<va_t, unsigned>;
  llvm::DenseMap<AddressKey, unsigned> Entries;
  llvm::DenseMap<AddressKey, unsigned> NativeFuncInfos;
  Entries.reserve(Funcs.size());
  NativeFuncInfos.reserve(Funcs.size());
  std::vector<CodeExtent> Extents;
  for (unsigned I = 0; I < Funcs.size(); ++I) {
    const MedFunc &Func = Funcs[I];
    const auto [Entry, Unique] =
        Entries.try_emplace(AddressKey{Func.Entry, 0}, I);
    if (!Unique)
      Groups.join(Entry->second, I);
    if (Func.ExceptionMetadata && Func.ExceptionMetadata->Cxx) {
      const va_t Identity = Func.ExceptionMetadata->Cxx->NativeFuncInfoVA;
      if (Identity) {
        const auto [Owner, Inserted] =
            NativeFuncInfos.try_emplace(AddressKey{Identity, 0}, I);
        if (!Inserted)
          Groups.join(Owner->second, I);
      }
    }
    if (Func.Name.empty() || Func.Blocks.empty())
      continue;
    for (const auto &[Begin, End] : medFunctionCodeRanges(Func))
      Extents.push_back({Begin, End - 1, I});
    for (const MedBlock &Block : Func.Blocks) {
      const va_t Address = medBlockCodeAddress(Block);
      Extents.push_back({Address, Address, I});
    }
  }

  // Coalesce overlapping owners once, including ambiguous exact labels.
  // Every later constant query is logarithmic in the disjoint extent index,
  // never a scan over every function. Scheduling grants no address identity:
  // the emitter still rejects conflicting, stale or unsupported occurrences.
  std::sort(Extents.begin(), Extents.end(), [](const auto &A, const auto &B) {
    return std::tie(A.Begin, A.Last, A.Function) <
           std::tie(B.Begin, B.Last, B.Function);
  });
  std::vector<CodeExtent> Index;
  for (const CodeExtent &Extent : Extents) {
    if (!Index.empty() && Extent.Begin <= Index.back().Last) {
      Groups.join(Index.back().Function, Extent.Function);
      Index.back().Last = std::max(Index.back().Last, Extent.Last);
    } else {
      Index.push_back(Extent);
    }
  }

  for (unsigned I = 0; I < Funcs.size(); ++I) {
    const MedFunc &Func = Funcs[I];
    // SEH lowering commits noinline/optnone to its callbacks only after the
    // parent passes preflight. Their definitions must be present in that same
    // transaction, before any shard runs promotion or optimization.
    if (Func.ExceptionMetadata && Func.ExceptionMetadata->SEH)
      for (const SEHScopeRecord &Scope : Func.ExceptionMetadata->SEH->Scopes) {
        if (Scope.Kind != SEHScopeKind::Filter &&
            Scope.Kind != SEHScopeKind::Finally)
          continue;
        const auto Callback = Entries.find({Scope.FilterOrFinallyVA, 0});
        if (Scope.FilterOrFinallyVA && Callback != Entries.end())
          Groups.join(I, Callback->second);
      }

    visitAddressInputs(Func, [&](const MedVar &Value) {
      if (!Value.isConst() ||
          Value.Provenance == ConstantAddressProvenance::Scalar ||
          Value.Provenance == ConstantAddressProvenance::DataAddress ||
          Value.Provenance == ConstantAddressProvenance::AddressFragment ||
          (!Value.ConstVal && !isExactAddressProvenance(Value.Provenance)))
        return;
      const va_t Address =
          Image ? normalizeCodeAddress(Value.ConstVal, Image->Arch, Image->Mode)
                : Value.ConstVal;
      // Entry references link through ordinary declarations. Interior block
      // addresses and offset identities need their owner's retained body.
      if (Entries.count({Address, 0}))
        return;
      const auto End =
          std::upper_bound(Index.begin(), Index.end(), Address,
                           [](va_t Address, const CodeExtent &Extent) {
                             return Address < Extent.Begin;
                           });
      if (End != Index.begin()) {
        const CodeExtent &Owner = *std::prev(End);
        if (Address <= Owner.Last)
          Groups.join(I, Owner.Function);
      }
    });
  }
  Groups.compress();
  return Groups;
}

} // namespace

LLVMShardPlan planLLVMEmissionShards(const std::vector<MedFunc> &Funcs,
                                     const BinaryImage *Image) {
  LLVMShardPlan Plan;
  Plan.ShardOf.assign(Funcs.size(), 0);
  if (Funcs.empty())
    return Plan;
  const auto Groups = emissionGroups(Funcs, Image);
  struct WorkUnit {
    uint64_t Weight = 0;
    size_t FirstFunction = 0;
    std::vector<size_t> Functions;
  };
  std::vector<WorkUnit> Units(Groups.getNumClasses());
  uint64_t TotalWeight = 0;
  for (size_t I = 0; I < Funcs.size(); ++I) {
    uint64_t Weight = 1;
    for (const MedBlock &Block : Funcs[I].Blocks)
      Weight += Block.Ops.size() + Block.Phis.size();
    WorkUnit &Unit = Units[Groups[I]];
    if (Unit.Functions.empty())
      Unit.FirstFunction = I;
    Unit.Functions.push_back(I);
    Unit.Weight += Weight;
    TotalWeight += Weight;
  }

  // Worker count controls only execution, never a function's optimization
  // neighbors or exception contract. Indivisible native dependency groups
  // may exceed the soft memory budget.
  Plan.NumShards = static_cast<unsigned>(std::min<uint64_t>(
      Units.size(), 1 + (TotalWeight - 1) / limits::kMaxShardOps));
  std::vector<size_t> Order(Units.size());
  for (size_t I = 0; I < Units.size(); ++I)
    Order[I] = I;
  std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
    return Units[A].Weight != Units[B].Weight
               ? Units[A].Weight > Units[B].Weight
               : Units[A].FirstFunction < Units[B].FirstFunction;
  });
  using Bin = std::pair<uint64_t, unsigned>;
  std::priority_queue<Bin, std::vector<Bin>, std::greater<Bin>> Load;
  for (unsigned S = 0; S < Plan.NumShards; ++S)
    Load.emplace(0, S);
  for (size_t UnitIndex : Order) {
    auto [Weight, Shard] = Load.top();
    Load.pop();
    for (size_t Function : Units[UnitIndex].Functions)
      Plan.ShardOf[Function] = Shard;
    Load.emplace(Weight + Units[UnitIndex].Weight, Shard);
  }
  return Plan;
}

} // namespace neverd::pipeline_detail
