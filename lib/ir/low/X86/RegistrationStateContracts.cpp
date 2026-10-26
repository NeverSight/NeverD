//===- RegistrationStateContracts.cpp - x86 EH object contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate frame-object ownership, initialization and projected call effects.
///
//===----------------------------------------------------------------------===//

#include "RegistrationImportLoad.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::initializeContracts() {
  if (CxxCalls) {
    if (!KnownCxx || !charge(CxxCalls->ImportSlots.size()))
      return false;
    for (va_t Slot : CxxCalls->ImportSlots) {
      if (!Slot || Slot > UINT32_MAX - 3)
        return false;
      StackImportSlots.insert(Slot);
    }
  }
  if (LocalUnwinds) {
    if (LocalUnwinds->size() >
        ::neverd::limits::ir::
            kRegistrationStateContractsRegistrationStateSolverInitializeContractsLocalUnwindsSizeLimit)
      return false;
    for (const auto &Call : *LocalUnwinds) {
      if (!charge(1) || !Call.Target || Call.Target > UINT32_MAX ||
          (Call.Protocol != RegistrationLocalUnwindContract::Kind::EH3 &&
           Call.Protocol != RegistrationLocalUnwindContract::Kind::EH4) ||
          Call.ImportSlot > UINT32_MAX - 3 ||
          (Call.Indirect && Call.ImportSlot &&
           Call.Target != Call.ImportSlot) ||
          !LocalUnwindTargets
               .emplace(std::make_pair(Call.Target, Call.Indirect), Call)
               .second) {
        Result.Diagnostics.push_back(
            "registration local-unwind contract is invalid");
        return false;
      }
      if (Call.ImportSlot)
        LocalUnwindImportSlots.insert(Call.ImportSlot);
    }
  }
  if (Stacks) {
    if (Stacks->size() >
        ::neverd::limits::ir::
            kRegistrationStateContractsRegistrationStateSolverInitializeContractsStacksSizeLimit)
      return false;
    std::map<std::pair<va_t, int>, va_t> CurrentLoads;
    const bool HasImportLoads =
        std::any_of(Stacks->begin(), Stacks->end(),
                    [](const auto &Stack) { return !Stack.Loads.empty(); });
    if (HasImportLoads) {
      for (const auto &Block : Function.Blocks)
        for (const auto &Boundary : Block.InstructionBoundaries) {
          if (!charge(16))
            return false;
          if (const auto Load =
                  registration_abi::registrationImportLoad(Block, Boundary))
            if (!CurrentLoads
                     .emplace(std::make_pair(Boundary.Address, Load->OpSeq),
                              Load->Slot)
                     .second)
              return false;
        }
    }
    for (const auto &Stack : *Stacks) {
      if (!charge(1) || !Stack.Target || Stack.Target > UINT32_MAX ||
          Stack.StackPopBytes > UINT16_MAX ||
          !StackPops
               .emplace(std::make_pair(Stack.Target, Stack.Indirect),
                        Stack.StackPopBytes)
               .second) {
        Result.Diagnostics.push_back("registration stack contract is invalid");
        return false;
      }
      if (!Stack.Loads.empty() && !Stack.Indirect)
        return false;
      if (Stack.ImportVeneer) {
        if (Stack.Indirect || Stack.RequiredImports.size() != 1)
          return false;
        StackImportVeneers.insert(Stack.Target);
      }
      if (Stack.PreservesImportBindings) {
        if (!Stack.Indirect && !Stack.ImportVeneer)
          return false;
        StackBindingPreservers.emplace(Stack.Target, Stack.Indirect);
      }
      if (Stack.Indirect) {
        if (!Stack.RequiredImports.empty())
          return false;
        StackImportSlots.insert(Stack.Target);
      }
      va_t PreviousImport = 0;
      for (va_t Slot : Stack.RequiredImports) {
        if (!charge(1) || Slot <= PreviousImport || Slot > UINT32_MAX - 3)
          return false;
        StackImportSlots.insert(Slot);
        PreviousImport = Slot;
      }
      if (!Stack.RequiredImports.empty())
        StackBindingCallees.insert(Stack.Target);
      for (const auto &Load : Stack.Loads) {
        if (!charge(1))
          return false;
        const auto Current = CurrentLoads.find(Load);
        if (Current == CurrentLoads.end() || Current->second != Stack.Target ||
            !StackImportLoads.emplace(Load, Stack.Target).second)
          return false;
      }
    }
  }
  if (CheckCalls) {
    if (Callees->size() >
        ::neverd::limits::ir::
            kRegistrationStateContractsRegistrationStateSolverInitializeContractsCalleesSizeLimit) {
      Result.Diagnostics.push_back("registration callee count exceeds budget");
      return false;
    }
    for (const auto &Callee : *Callees) {
      const bool Leaf =
          Callee.CalleeKind == RegistrationCalleeFrameContract::Kind::Leaf;
      const bool Throw = Callee.isThrow();
      const bool Initializer = Callee.isInitializer();
      const bool CxxParent = Callee.isCxxParent();
      const bool Member =
          Leaf && (!Callee.ECXReads.empty() || !Callee.ECXWrites.empty()) &&
          Callee.CallerPCWrites.empty();
      if (!charge(1) || !Callee.Target || Callee.Target > UINT32_MAX ||
          (Initializer || Member
               ? Callee.StackPopBytes > UINT16_MAX ||
                     Callee.StackPopBytes % 4 ||
                     (Leaf && Callee.StackPopBytes / 4 >= limits::kMaxCallArgs)
               : Callee.StackPopBytes != 0) ||
          (!Leaf && !Throw && !Initializer && !CxxParent) ||
          (Throw ? !Callee.DoesNotReturn
                 : !CxxParent && Callee.DoesNotReturn) ||
          (Callee.DoesNotReturn && Callee.HasIndependentScalarReturn) ||
          (Callee.HasIndependentScalarReturn && !Leaf && !CxxParent) ||
          ((Leaf || Initializer || CxxParent || Callee.isRethrow() ||
            Callee.isRuntimeThrow()) &&
           (Callee.ThrownTypeVA || Callee.ThrownObjectSize)) ||
          (Throw && (!Callee.ECXReads.empty() || !Callee.ECXWrites.empty())) ||
          (CxxParent &&
           (!Callee.ECXReads.empty() || !Callee.ECXWrites.empty() ||
            !Callee.CallerPCWrites.empty() ||
            Callee.RequiredImports.empty())) ||
          (Throw && !Callee.isRethrow() && !Callee.isRuntimeThrow() &&
           (!Callee.ThrownTypeVA || Callee.ThrownTypeVA > UINT32_MAX ||
            !Callee.ThrownObjectSize ||
            Callee.ThrownObjectSize > limits::kMaxSourceCallBorrowedBytes)) ||
          !validObjects(Callee.ECXReads) || !validObjects(Callee.ECXWrites) ||
          !validObjects(Callee.ECXInitialized) ||
          (Initializer != !Callee.ECXInitialized.empty()) ||
          (Initializer && !Callee.CallerPCWrites.empty()) ||
          !validImageRanges(Callee.ImageReads) ||
          !validImageRanges(Callee.ImageWrites) ||
          !validImageRanges(Callee.CallerPCWrites) ||
          !CalleeIndices.emplace(Callee.Target, CalleeIndices.size()).second) {
        Result.Diagnostics.push_back("registration callee contract is invalid");
        return false;
      }
      for (const auto &Initialized : Callee.ECXInitialized) {
        bool Covered = false;
        for (const auto &Write : Callee.ECXWrites) {
          if (!charge(1))
            return false;
          Covered |=
              Write.Begin <= Initialized.Begin && Initialized.End <= Write.End;
        }
        if (!Covered)
          return false;
      }
      va_t PreviousImport = 0;
      for (va_t Slot : Callee.RequiredImports) {
        if (!charge(1) || Slot <= PreviousImport || Slot > UINT32_MAX - 3)
          return false;
        StackImportSlots.insert(Slot);
        PreviousImport = Slot;
      }
      if (!Callee.isRuntimeThrow() && !CxxParent &&
          !Callee.RuntimeThrowInfos.empty())
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
            !Leaf.ECXInitialized.empty() || !Leaf.CallerPCWrites.empty() ||
            !validObjects(Leaf.ECXReads) || !validObjects(Leaf.ECXWrites) ||
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
        Range.End > int64_t(limits::kMaxSourceCallBorrowedBytes))
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

bool RegistrationStateSolver::frameObjectIsDisjointFromAdministration(
    int64_t Begin, int64_t End) const {
  if (End <= Begin)
    return false;
  auto Overlaps = [&](int64_t Slot, int64_t Width) {
    return Begin < Slot + Width && Slot < End;
  };
  // C++ owns SavedESP below its three-word node. SEH additionally owns the
  // exception-pointer slot between SavedESP and its four-word node. These
  // are runtime administration even when a source store left scalar contents.
  int64_t Administration =
      int64_t(*Chain.RegistrationOffset) - (KnownCxx ? 4 : 8);
  if (Chain.RealignedFrame && Chain.RealignedFrame->hasSavedParentFrame())
    Administration = std::min(
        Administration, int64_t(Chain.RealignedFrame->SavedParentFrameOffset));
  if (Overlaps(Administration,
               int64_t(*Chain.TryLevelOffset) + 4 - Administration))
    return false;
  if (EH4) {
    // Cookie slots can lie below the contiguous registration area. Incomplete
    // placement evidence cannot authorize an object at an invented offset.
    if (!EHCookieSlot || (Chain.GSCookieOffset != -2 && !GSCookieSlot) ||
        Overlaps(*EHCookieSlot, 4) ||
        (GSCookieSlot && Overlaps(*GSCookieSlot, 4)))
      return false;
  }
  for (const auto &Condition : CxxCookieConditions)
    if (Overlaps(Condition.FrameOffset, 4))
      return false;
  return true;
}

std::optional<RegistrationObjectExtent>
RegistrationStateSolver::cxxBorrowedStorage(const Domain &State,
                                            int32_t Begin) {
  const auto SP = parentStackOffset(State);
  if (!KnownCxx || !SP || State.Unknown || Begin < *SP ||
      Begin < -int64_t(limits::kMaxSourceCallBorrowedBytes))
    return std::nullopt;
  int64_t End = *Chain.cxxRuntimeFrameOffset();
  if (Begin >= 8 && Conditional) {
    if (!charge(Conditional->Objects.size() + 1))
      return std::nullopt;
    for (const auto &Object : Conditional->Objects)
      if (Object.Begin <= Begin && int64_t(Begin) + 4 <= Object.End)
        End = std::max(End, int64_t(Object.End));
  }
  End = std::min(End, int64_t(Begin) + limits::kMaxSourceCallBorrowedBytes);
  if (End < int64_t(Begin) + 4)
    return std::nullopt;

  // The prefix predicate is monotone: once a range touches administration,
  // extending it cannot regain ownership. Reuse that authority, including
  // noncontiguous cookie slots, without scanning a large frame byte by byte.
  uint32_t Words = 0, Limit = uint32_t((End - Begin) / 4);
  while (Words < Limit) {
    if (!charge(CxxCookieConditions.size() + 1))
      return std::nullopt;
    const uint32_t Middle = Words + (Limit - Words + 1) / 2;
    if (frameObjectIsDisjointFromAdministration(Begin,
                                                int64_t(Begin) + 4 * Middle))
      Words = Middle;
    else
      Limit = Middle - 1;
  }
  if (!Words)
    return std::nullopt;
  const RegistrationObjectExtent Extent{Begin,
                                        int32_t(int64_t(Begin) + 4 * Words)};
  std::vector<RegistrationObjectExtent> Projected;
  if (!projectFrameObject(State, 0, SP, {Extent}, Projected, false, false))
    return std::nullopt;
  return Extent;
}

bool RegistrationStateSolver::projectFrameObject(
    const Domain &State, int32_t Object, std::optional<int32_t> SP,
    llvm::ArrayRef<RegistrationObjectExtent> Extents,
    std::vector<RegistrationObjectExtent> &Projected, bool Reads,
    bool RecordBorrowedEffects, bool RequireInitializedReads) {
  if (!SP || State.Unknown)
    return false;
  for (const auto &Extent : Extents) {
    if (!charge(1))
      return false;
    const int64_t Begin = int64_t(Object) + Extent.Begin;
    const int64_t End = int64_t(Object) + Extent.End;
    bool Borrowed = false;
    if (KnownCxx && Conditional && Begin >= 8) {
      if (End > INT32_MAX || !charge(Conditional->Objects.size()))
        return false;
      Borrowed = llvm::any_of(Conditional->Objects, [&](const auto &Range) {
        return Range.Begin <= Begin && End <= Range.End;
      });
    }
    // Saved EBP and the caller PC lie above the source allocation. The common
    // administration proof also protects runtime slots within that allocation.
    if (Begin < *SP || Begin < -int64_t(limits::kMaxSourceCallBorrowedBytes) ||
        (!Borrowed && End > (KnownCxx ? *Chain.cxxRuntimeFrameOffset() : 0)) ||
        !frameObjectIsDisjointFromAdministration(Begin, End) ||
        !charge(size_t(End - Begin) + State.Frame.cellCount()))
      return false;
    if (Reads && RequireInitializedReads) {
      for (int64_t Byte = Begin; Byte < End; ++Byte)
        if (!State.InitializedFrameBytes.count(int32_t(Byte)))
          return false;
      for (const auto &[Cell, Value] : State.Frame.Cells)
        if (Value.MayBeFrame && int64_t(Cell) < End &&
            Begin < int64_t(Cell) + 4)
          return false;
    }
    Projected.push_back({int32_t(Begin), int32_t(End)});
    if (Borrowed && RecordBorrowedEffects) {
      auto &Effects = Reads ? Conditional->Reads : Conditional->Writes;
      Effects.emplace(int32_t(Begin), int32_t(End));
    }
  }
  return true;
}

} // namespace neverd::registration_state
