//===- RegistrationStateContracts.cpp - x86 EH object contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::initializeContracts() {
  if (LocalUnwinds) {
    if (LocalUnwinds->size() > 256)
      return false;
    for (const auto &Call : *LocalUnwinds)
      if (!charge(1) || !Call.Target || Call.Target > UINT32_MAX ||
          !LocalUnwindTargets.emplace(Call.Target, Call.Indirect).second) {
        Result.Diagnostics.push_back(
            "registration local-unwind contract is invalid");
        return false;
      }
  }
  if (Stacks) {
    if (Stacks->size() > 256)
      return false;
    for (const auto &Stack : *Stacks)
      if (!charge(1) || !Stack.Target || Stack.Target > UINT32_MAX ||
          Stack.StackPopBytes > UINT16_MAX ||
          !StackPops
               .emplace(std::make_pair(Stack.Target, Stack.Indirect),
                        Stack.StackPopBytes)
               .second) {
        Result.Diagnostics.push_back("registration stack contract is invalid");
        return false;
      }
  }
  if (CheckCalls) {
    if (Callees->size() > 256) {
      Result.Diagnostics.push_back("registration callee count exceeds budget");
      return false;
    }
    for (const auto &Callee : *Callees) {
      const bool Leaf =
          Callee.CalleeKind == RegistrationCalleeFrameContract::Kind::Leaf;
      const bool Throw = Callee.isThrow();
      if (!charge(1) || !Callee.Target || Callee.Target > UINT32_MAX ||
          Callee.StackPopBytes || (!Leaf && !Throw) ||
          Callee.DoesNotReturn != Throw ||
          ((Leaf || Callee.isRethrow() || Callee.isRuntimeThrow()) &&
           (Callee.ThrownTypeVA || Callee.ThrownObjectSize)) ||
          (Throw && (!Callee.ECXReads.empty() || !Callee.ECXWrites.empty())) ||
          (Throw && !Callee.isRethrow() && !Callee.isRuntimeThrow() &&
           (!Callee.ThrownTypeVA || Callee.ThrownTypeVA > UINT32_MAX ||
            !Callee.ThrownObjectSize ||
            Callee.ThrownObjectSize > limits::kMaxRegistrationEHStateWork)) ||
          !validObjects(Callee.ECXReads) || !validObjects(Callee.ECXWrites) ||
          !validImageRanges(Callee.ImageReads) ||
          !validImageRanges(Callee.ImageWrites) ||
          !validImageRanges(Callee.CallerPCWrites) ||
          !CalleeIndices.emplace(Callee.Target, CalleeIndices.size()).second) {
        Result.Diagnostics.push_back("registration callee contract is invalid");
        return false;
      }
      if (!Callee.isRuntimeThrow() && !Callee.RuntimeThrowInfos.empty())
        return false;
      va_t Previous = 0;
      for (const auto &Info : Callee.RuntimeThrowInfos) {
        if (!charge(1) || Info.Address <= Previous ||
            Info.Address > UINT32_MAX || !Info.TypeDescriptorVA ||
            Info.TypeDescriptorVA > UINT32_MAX || !Info.ObjectSize ||
            Info.ObjectSize > UINT16_MAX)
          return false;
        Previous = Info.Address;
      }
    }
    Result.CalleeContracts = *Callees;
  }
  if (CheckRuntimeObjects) {
    std::map<va_t, uint32_t> Widths;
    auto RecordWidth = [&](va_t Type, uint32_t Width) {
      const auto [It, New] = Widths.emplace(Type, Width);
      if (!New && It->second != Width)
        It->second = 0;
    };
    for (const auto &Callee : Result.CalleeContracts) {
      if (Callee.CalleeKind ==
          RegistrationCalleeFrameContract::Kind::PrivateThrow)
        RecordWidth(Callee.ThrownTypeVA, Callee.ThrownObjectSize);
      for (const auto &Info : Callee.RuntimeThrowInfos) {
        if (!charge(1))
          return false;
        RecordWidth(Info.TypeDescriptorVA, Info.ObjectSize);
      }
    }
    for (uint32_t I = 0; I < EH.Cxx->TryBlocks.size(); ++I)
      for (uint32_t J = 0; J < EH.Cxx->TryBlocks[I].Handlers.size(); ++J) {
        if (!charge(1))
          break;
        const auto &Catch = EH.Cxx->TryBlocks[I].Handlers[J];
        if (!Catch.CatchObjectOffset)
          continue;
        auto Width = Widths.find(Catch.TypeDescriptorVA);
        if (!Catch.TypeDescriptorVA || Width == Widths.end() ||
            Width->second == 0 || Width->second > UINT16_MAX ||
            (Catch.Adjectives != 0 && Catch.Adjectives != 8) ||
            Catch.ParentFrameOffset) {
          CompleteCatchObjects = false;
          continue;
        }
        CatchObjects.emplace(
            std::make_pair(I, J),
            RegistrationCxxCatchObject{I, J, Catch.TypeDescriptorVA,
                                       Width->second, Catch.CatchObjectOffset,
                                       Catch.Adjectives == 8});
      }
  }
  if (CheckCleanups) {
    if (Cleanups->size() > limits::kMaxRegistrationEHRecords) {
      Result.Diagnostics.push_back("registration cleanup count exceeds budget");
      return false;
    }
    for (const auto &Cleanup : *Cleanups) {
      if (!charge(1) || Cleanup.ActionState >= EH.Cxx->UnwindMap.size() ||
          EH.Cxx->UnwindMap[Cleanup.ActionState].Kind !=
              CxxUnwindAction::ActionKind::Direct ||
          EH.Cxx->UnwindMap[Cleanup.ActionState].ObjectOffset ||
          Cleanup.RelayTarget !=
              EH.Cxx->UnwindMap[Cleanup.ActionState].ActionVA ||
          !Cleanup.RelayTarget || Cleanup.RelayTarget > UINT32_MAX ||
          Cleanup.Calls.empty() ||
          Cleanup.Calls.size() > limits::kMaxRegistrationEHRecords ||
          !CleanupIndices.emplace(Cleanup.ActionState, CleanupIndices.size())
               .second) {
        Result.Diagnostics.push_back(
            "registration cleanup contract is invalid");
        return false;
      }
      for (const auto &Call : Cleanup.Calls) {
        const auto &Leaf = Call.Leaf;
        if (!charge(1) || !Leaf.Target || Leaf.Target > UINT32_MAX ||
            Leaf.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf ||
            Leaf.StackPopBytes || Leaf.DoesNotReturn || Leaf.ThrownTypeVA ||
            Leaf.ThrownObjectSize || !Leaf.RuntimeThrowInfos.empty() ||
            !Leaf.CallerPCWrites.empty() || !validObjects(Leaf.ECXReads) ||
            !validObjects(Leaf.ECXWrites) ||
            !validImageRanges(Leaf.ImageReads) ||
            !validImageRanges(Leaf.ImageWrites)) {
          Result.Diagnostics.push_back("registration cleanup call is invalid");
          return false;
        }
      }
    }
    Result.CleanupContracts = *Cleanups;
  }
  return true;
}

bool RegistrationStateSolver::validObjects(
    llvm::ArrayRef<RegistrationObjectExtent> Ranges) {
  int32_t PreviousEnd = 0;
  for (const auto &Range : Ranges) {
    if (!charge(1) || Range.Begin < PreviousEnd || Range.End <= Range.Begin ||
        Range.End > int64_t(limits::kMaxRegistrationEHStateWork))
      return false;
    PreviousEnd = Range.End;
  }
  return true;
}

bool RegistrationStateSolver::validImageRanges(
    llvm::ArrayRef<ExceptionAddressRange> Ranges) {
  va_t PreviousEnd = 0;
  for (const auto &Range : Ranges) {
    if (!charge(1) || !Range.isValid() || Range.Begin < PreviousEnd ||
        Range.End > uint64_t(UINT32_MAX) + 1)
      return false;
    PreviousEnd = Range.End;
  }
  return true;
}

bool RegistrationStateSolver::projectFrameObject(
    const Domain &State, int32_t Object, std::optional<int32_t> SP,
    llvm::ArrayRef<RegistrationObjectExtent> Extents,
    std::vector<RegistrationObjectExtent> &Projected, bool Reads) {
  if (!SP || State.Unknown)
    return false;
  for (const auto &Extent : Extents) {
    if (!charge(1))
      return false;
    const int64_t Begin = int64_t(Object) + Extent.Begin;
    const int64_t End = int64_t(Object) + Extent.End;
    // Registration/SavedESP, saved EBP and the caller PC cannot be objects.
    // The realigned frame additionally owns a distinct saved entry EBP.
    const int64_t Administration =
        Chain.RealignedFrame
            ? Chain.RealignedFrame->SavedParentFrameOffset
            : int64_t(*Chain.RegistrationOffset) - (KnownCxx ? 4 : 0);
    if (Begin < *SP || Begin < -int64_t(limits::kMaxRegistrationEHStateWork) ||
        End > (KnownCxx ? *Chain.cxxRuntimeFrameOffset() : 0) ||
        (Begin < int64_t(*Chain.TryLevelOffset) + 4 && Administration < End) ||
        !charge(size_t(End - Begin) + State.Frame.cellCount()))
      return false;
    if (Reads) {
      for (int64_t Byte = Begin; Byte < End; ++Byte)
        if (!State.InitializedFrameBytes.count(int32_t(Byte)))
          return false;
      for (const auto &[Cell, Value] : State.Frame.Cells)
        if (Value.MayBeFrame && int64_t(Cell) < End &&
            Begin < int64_t(Cell) + 4)
          return false;
    }
    Projected.push_back({int32_t(Begin), int32_t(End)});
  }
  return true;
}

} // namespace neverd::registration_state
