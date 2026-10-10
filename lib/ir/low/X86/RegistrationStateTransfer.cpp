//===- RegistrationStateTransfer.cpp - x86 EH ordinary transfers ----------===//
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

namespace {
bool overlaps(int32_t Offset, uint16_t Width, int32_t Slot,
              uint16_t SlotWidth) {
  return int64_t(Offset) < int64_t(Slot) + SlotWidth &&
         int64_t(Slot) < int64_t(Offset) + Width;
}

} // namespace

bool RegistrationStateSolver::transferBlock(size_t I) {
  const LowBlock &Block = Function.Blocks[I];
  const Domain Before = Incoming[I];
  if (!charge(Before.CxxCatchStacks.size() + 1))
    return true;
  Domain After = Before;
  FrameTransfer Transfer(
      After.Frame, *Chain.RegistrationOffset, EH4 ? SecurityCookieVA : 0,
      Chain.RealignedFrame ? &*Chain.RealignedFrame : nullptr);
  FrameTransfer RuntimeTransfer(After.RuntimeObject, *Chain.RegistrationOffset);
  std::set<va_t> ProvenStores;
  std::optional<RegistrationCxxContinuation> CatchReturn;
  bool NoReturnAtExit = false;
  va_t NoReturnEnd = InvalidVA;
  auto Invalidate = [&] {
    Facts[I].Invalid = true;
    After.Unknown = true;
    After.Levels.clear();
  };
  for (const LowOp &Op : Block.Ops) {
    // Ambiguous frame loads inspect every tracked cell. Charge that work,
    // not just the number of lifted operations, before entering the scan.
    size_t RegisterBytes = Op.Output.Size;
    for (unsigned Input = 0; Input != Op.NumInputs; ++Input)
      RegisterBytes += Op.Inputs[Input].Size;
    if (!charge(
            1 + RegisterBytes +
            ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)
                 ? After.Frame.cellCount() + 2 * After.RuntimeObject.cellCount()
                 : 0)))
      break;
    Transfer.beginInstruction(Op.Addr);
    RuntimeTransfer.beginInstruction(Op.Addr);
    const FrameValue Value = Transfer.evaluate(Op, After.Installed);
    FrameValue RuntimeValue = RuntimeTransfer.evaluate(Op, After.Installed);
    if (CheckRuntimeObjects &&
        (Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::COND_BR ||
         Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::INDIR_CALL))
      for (unsigned Input = 0; Input != Op.NumInputs; ++Input)
        if (RuntimeTransfer.read(Op.Inputs[Input]).MayBeFrame)
          CompleteRuntimeObjects = false;
    if (CheckRuntimeObjects && Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
      const auto Address = Transfer.read(Op.Inputs[0]);
      RuntimeValue = After.RuntimeObject.load(Address.Offset, Op.Output.Size);
      if (!Address.Offset)
        RuntimeValue = {};
    }
    recordCatchReturn(I, After, Op, Transfer, CatchReturn);
    if (Op.Seq >= 0 && Op.Output.Size != 0 && Value.MayBeFrame) {
      if (!charge(1))
        break;
      auto [It, New] =
          FrameValues.emplace(std::make_pair(Op.Addr, Op.Seq), Value);
      if (!New)
        It->second = registration_state::join(It->second, Value);
    }
    const auto Memory = lowMemoryOperands(Op);
    if (Memory.Address &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto Address = Transfer.read(*Memory.Address);
      if (!realignedMemoryIsDisjoint(Address, Memory.AccessSize) ||
          (Address.CallbackAddress &&
           (!Memory.Complete || !After.Frame.callbackMemoryIsPrivate(
                                    *Address.CallbackAddress, Memory.AccessSize,
                                    Op.Opcode != NdOp::STORE)))) {
        // Alignment leaves the distance between the two frames unknown.
        // Reject accesses that could cross into the other coordinate space.
        Invalidate();
        CompleteImageReads = ConsistentIncomingAccesses = false;
      }
    }
    bool RuntimeMemory = false;
    RuntimeMemory =
        recordRuntimeMemory(I, After, Op, Transfer, RuntimeTransfer);
    if (Exhausted)
      break;

    if (EH4 && Op.Opcode == NdOp::INTRINSIC)
      CompleteCookies = false;
    if (EH4 && Memory.Address &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto Address = Transfer.read(*Memory.Address);
      if (!Memory.Complete)
        CompleteCookies = false;
      if (After.Installed && (!Address.Offset && Address.MayBeFrame))
        CompleteCookies = false;
      if (After.Installed && Address.Offset) {
        auto Touches = [&](std::optional<int32_t> Slot) {
          return Slot && overlaps(*Address.Offset, Memory.AccessSize, *Slot, 4);
        };
        // Source cookie storage is owned only by its checked initialization
        // and the native runtime. Reads or writes after installation would
        // observe or change a synthetic cookie instead of the physical one.
        const bool CheckedGSRead = CookieCheckVA && Op.Opcode == NdOp::LOAD &&
                                   Address.Offset == GSCookieSlot &&
                                   Memory.AccessSize == 4;
        ReadsGSCookie |= CheckedGSRead;
        if (Touches(EHCookieSlot) ||
            (Touches(GSCookieSlot) && !CheckedGSRead) ||
            overlaps(*Address.Offset, Memory.AccessSize,
                     *Chain.RegistrationOffset + 8, 4))
          CompleteCookies = false;
      }
      if (Memory.StoredValue) {
        const uint64_t TableEnd =
            Chain.ScopeTableVA + 16 + uint64_t(Chain.Scopes.size()) * 12;
        if (!Address.Offset &&
            (!Address.Constant ||
             (uint64_t(*Address.Constant) < SecurityCookieVA + 4 &&
              SecurityCookieVA <
                  uint64_t(*Address.Constant) + Memory.AccessSize) ||
             (uint64_t(*Address.Constant) < TableEnd &&
              Chain.ScopeTableVA <
                  uint64_t(*Address.Constant) + Memory.AccessSize)))
          CompleteCookies = false;
      }
    }
    if (CookieCheckVA && Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
        Op.Inputs[0].isConst() && Op.Inputs[0].Offset == CookieCheckVA) {
      const auto Boundary = Boundaries.find(Op.Addr);
      if (!Transfer.isCookieCheck(Op, CookieCheckVA) || Op.Seq < 0 ||
          Boundary == Boundaries.end() || Boundary->second.first != Block.Id)
        CompleteCookies = false;
      else
        CookieChecks.emplace(
            std::make_pair(Op.Addr, Op.Seq),
            RegistrationCookieCheck{
                Op.Addr, Op.Addr + Boundary->second.second.Size, Op.Seq});
    }
    if (Op.Opcode == NdOp::INTRINSIC) {
      CompleteImageReads = false;
      if (CheckCalls) {
        CompleteCalls = false;
        After.InitializedFrameBytes.clear();
        if (!charge(After.Frame.cellCount()))
          break;
        After.Frame.forgetCellValues();
      }
    }
    if (Memory.Address && Op.Opcode != NdOp::STORE &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (!charge(1))
        break;
      const auto Address = Transfer.read(*Memory.Address);
      if (!Memory.Complete)
        CompleteImageReads = false;
      else if (Address.Offset || Address.EntryOffset ||
               Address.CallbackAddress || RuntimeMemory) {
        // The established private frame cannot alias an image allocation.
      } else if (Address.Constant && !Address.MayBeFrame)
        ImageReads.emplace(*Address.Constant,
                           va_t(*Address.Constant) + Memory.AccessSize);
      else
        CompleteImageReads = false;
    }
    if (Memory.Complete &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)) {
      const auto Address = Transfer.read(*Memory.Address);
      const uint16_t Width =
          Op.Opcode == NdOp::LOAD ? Op.Output.Size : Memory.StoredValue->Size;
      std::optional<RegistrationIncomingFrameAccess> Access;
      if (Address.MayBeFrame && !Address.Offset && !Address.EntryOffset &&
          !Address.CallbackAddress)
        ConsistentIncomingAccesses = false;
      const auto IncomingOffset =
          Chain.RealignedFrame ? Address.EntryOffset : Address.Offset;
      if (IncomingOffset && int64_t(*IncomingOffset) + Width > 4)
        Access = RegistrationIncomingFrameAccess{
            Op.Addr, Op.Seq, *IncomingOffset, Width, Op.Opcode == NdOp::STORE};
      auto [It, Inserted] =
          IncomingAccesses.emplace(std::make_pair(Op.Addr, Op.Seq), Access);
      ConsistentIncomingAccesses &= Inserted || It->second == Access;
    }
    if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
        Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
      // An RMW is a write even when its scalar result is dead. This domain
      // does not guess a conditional registration state or frame-cell value.
      if (!Memory.Complete ||
          Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS ||
          Transfer.read(*Memory.Address).MayBeFrame ||
          Transfer.read(*Memory.StoredValue).MayBeFrame)
        Invalidate();
    }
    if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
      const FrameValue Address = Transfer.read(Op.Inputs[0]);
      if (!Address.Constant ||
          overlaps(int32_t(*Address.Constant), Op.Output.Size, 0, 4)) {
        if (Address.Constant == uint32_t{0} && Op.Output.Size == 4 &&
            After.Installed != After.Uninstalled)
          recordChainAccess(
              Op, Block,
              After.Installed
                  ? RegistrationChainAccess::Kind::ReadInstalledHead
                  : RegistrationChainAccess::Kind::ReadPreviousHead);
        else
          CompleteChainOperations = false;
      }
    }
    if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
      const FrameValue Address = Transfer.read(Op.Inputs[0]);
      const FrameValue Stored = Transfer.read(Op.Inputs[1]);
      const uint16_t Width = Op.Inputs[1].Size;
      if (Stored.MayBeFrame && !charge((size_t(Width) + 3) / 4))
        break;
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
          (!Address.Constant ||
           overlaps(int32_t(*Address.Constant), Width, 0, 4))) {
        auto Boundary =
            std::find_if(Block.InstructionBoundaries.begin(),
                         Block.InstructionBoundaries.end(),
                         [&](const LowInstructionBoundary &Instruction) {
                           return Instruction.Address == Op.Addr;
                         });
        const bool AtEnd = Boundary != Block.InstructionBoundaries.end() &&
                           Boundary->Address + Boundary->Size == Block.EndAddr;
        if (Address.Constant == uint32_t{0} && Width == 4 && AtEnd &&
            Op.Addr == Chain.ChainInstallVA &&
            Stored.Offset == Chain.RegistrationOffset && After.Uninstalled &&
            !After.Installed && realignedInstallationReady(After.Frame)) {
          After.Levels = {*Chain.SeededTryLevel};
          After.Unknown = false;
          After.Uninstalled = false;
          After.Installed = true;
          ProvenInstallation = true;
          if (EH4 && !cookiesReady(After.Frame))
            CompleteCookies = false;
          recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Install);
        } else if (Address.Constant == uint32_t{0} && Width == 4 && AtEnd &&
                   Stored.PreviousChain && After.Installed &&
                   !After.Uninstalled) {
          After.Levels.clear();
          After.Unknown = false;
          After.Uninstalled = true;
          After.Installed = false;
          recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Remove);
        } else {
          CompleteChainOperations = false;
          Invalidate();
          After.Installed = After.Uninstalled = true;
        }
      } else if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (Chain.RealignedFrame && !After.Installed && After.Uninstalled &&
            Address.Offset == Chain.TryLevelOffset && Width == 4 &&
            Stored.Constant == uint32_t(*Chain.SeededTryLevel)) {
          const auto Observation = Stores.find(Op.Addr);
          if (Observation != Stores.end() && Observation->second.Width == 4 &&
              Observation->second.Level == *Chain.SeededTryLevel)
            ProvenStores.insert(Op.Addr);
        }
        if (After.Installed && Address.MayBeFrame && !Address.EntryOffset &&
            !Address.CallbackAddress &&
            (!Address.Offset ||
             overlaps(*Address.Offset, Width, *Chain.TryLevelOffset, 4))) {
          auto Observation = Stores.find(Op.Addr);
          std::set<int32_t> WrittenLevels;
          bool Valid =
              Address.Offset == Chain.TryLevelOffset &&
              (Width == 1 || Width == 2 || Width == 4) && Stored.Constant &&
              Observation != Stores.end() &&
              Observation->second.Width == Width &&
              uint32_t(Observation->second.Level) == *Stored.Constant &&
              !After.Uninstalled;
          if (Valid && Width == 4) {
            Valid = validLevel(int32_t(*Stored.Constant));
            WrittenLevels.insert(int32_t(*Stored.Constant));
          } else if (Valid) {
            // A byte/word state write preserves the other bytes. Never
            // sign-extend its immediate or infer zero high bytes. Replay
            // every reaching whole level and retain distinct results.
            const uint32_t Mask = Width == 1 ? UINT8_MAX : UINT16_MAX;
            Valid = !After.Unknown && !After.Levels.empty() &&
                    *Stored.Constant <= Mask;
            for (int32_t Level : After.Levels) {
              if (!charge(1)) {
                Valid = false;
                break;
              }
              const int32_t Written = int32_t((uint32_t(Level) & ~Mask) |
                                              (*Stored.Constant & Mask));
              Valid &= validLevel(Written);
              WrittenLevels.insert(Written);
            }
          }
          if (Valid && ProvenStores.insert(Op.Addr).second) {
            After.Levels = std::move(WrittenLevels);
            After.Unknown = false;
          } else
            Invalidate();
        }
        if (After.Installed && Address.Offset &&
            overlaps(
                *Address.Offset, Width, *Chain.RegistrationOffset,
                uint16_t(*Chain.TryLevelOffset - *Chain.RegistrationOffset)))
          Invalidate();
        if (Stored.CallbackAddress && !Address.CallbackAddress &&
            (Address.Offset != int64_t(*Chain.RegistrationOffset) - 4 ||
             Width != 4))
          Invalidate();
        if (After.Installed && Chain.RealignedFrame && Address.Offset &&
            overlaps(*Address.Offset, Width,
                     Chain.RealignedFrame->SavedParentFrameOffset, 4))
          Invalidate();
        if (Address.Offset) {
          if (!charge(After.Frame.cellCount() + 1))
            break;
          After.Frame.store(*Address.Offset, Width, Stored);
          if (CheckCalls) {
            const auto SP = parentStackOffset(After);
            const int64_t End = int64_t(*Address.Offset) + Width;
            if (SP && *Address.Offset >= *SP && End <= 0) {
              if (!charge(Width))
                break;
              for (int64_t Byte = *Address.Offset; Byte < End; ++Byte)
                After.InitializedFrameBytes.insert(int32_t(Byte));
            }
          }
          if (Address.Offset == Chain.TryLevelOffset &&
              ProvenStores.count(Op.Addr) && After.Levels.size() == 1)
            After.Frame.Cells[*Chain.TryLevelOffset] =
                FrameValue::constant(uint32_t(*After.Levels.begin()));
        } else if (Address.CallbackAddress) {
          if (!After.Frame.callbackMemoryIsPrivate(*Address.CallbackAddress,
                                                   Width) ||
              !charge(After.Frame.cellCount() + Width + 1))
            Invalidate();
          else
            After.Frame.storeCallback(Address.CallbackAddress->Offset, Width,
                                      Stored);
        } else if (Address.EntryOffset) {
          // Only the checked entry's saved-register area is disjoint from
          // the newly allocated frame. Do not guess aliases below it.
          if (*Address.EntryOffset < -12 ||
              int64_t(*Address.EntryOffset) + Width > 4 || After.Installed ||
              !charge(After.Frame.cellCount() + 1))
            Invalidate();
          else
            After.Frame.storeEntry(*Address.EntryOffset, Width, Stored);
        } else if (Stored.MayBeFrame) {
          // A frame pointer escaped to storage whose future aliases cannot
          // be bounded by this frame-value domain.
          Invalidate();
        }
        if (CheckCalls && !RuntimeMemory && !Address.Offset &&
            !Address.EntryOffset && !Address.CallbackAddress &&
            !Address.Constant) {
          After.InitializedFrameBytes.clear();
          if (!charge(After.Frame.cellCount()))
            break;
          After.Frame.forgetCellValues();
        }
      }
    }
    std::optional<FrameValue> CallSP;
    bool CheckedCall = false;
    if (auto Call = transferCall(I, After, Op)) {
      CheckedCall = true;
      CallSP = Call->StackPointer;
      NoReturnAtExit = Call->DoesNotReturn;
      if (NoReturnAtExit)
        NoReturnEnd = Call->EndAddress;
    }
    // Stack balance alone does not discharge transferCall's memory, object
    // initialization or no-return obligations. Retain their refusal while
    // recovering the independent register coordinate for ordinary flow.
    if (!CallSP && (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
        Op.NumInputs == 1 && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4) {
      const auto It =
          StackPops.find({Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL});
      const auto Boundary = Boundaries.find(Op.Addr);
      if (It != StackPops.end() && Op.Seq >= 0 &&
          Boundary != Boundaries.end() && Boundary->second.first == Block.Id &&
          Boundary->second.second.Control == LowInstructionControl::Call) {
        LowOp Adjust;
        Adjust.Opcode = NdOp::INT_ADD;
        Adjust.Output = NdVar::reg(x86reg::RSP, 4);
        Adjust.NumInputs = 2;
        Adjust.Inputs[0] = Adjust.Output;
        Adjust.Inputs[1] = NdVar::cst(It->second, 4);
        CallSP = Transfer.evaluate(Adjust, After.Installed);
      }
    }
    Transfer.write(Op, Value, CookieCheckVA);
    if (CheckRuntimeObjects) {
      RuntimeTransfer.write(Op, RuntimeValue);
      if (CheckedCall) {
        After.RuntimeObject
            .Registers[x86reg::RAX / x86reg::GeneralRegStride] = {};
        After.RuntimeObject
            .Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
        After.RuntimeObject
            .Registers[x86reg::RDX / x86reg::GeneralRegStride] = {};
        After.RuntimeObject
            .Registers[x86reg::RSP / x86reg::GeneralRegStride] = {};
        After.RuntimeObject.OtherRegisterBytes.clear();
        After.RuntimeObject.OtherRegistersMayBeFrame = false;
      }
    }
    if (Op.Output.isReg() && Op.Output.Offset == x86reg::RSP &&
        After.Frame.CallbackEntry) {
      if (!charge(After.Frame.CallbackCells.size() +
                  After.Frame.InitializedCallbackBytes.size()))
        break;
      After.Frame.trimCallbackCells();
    }
    if (CallSP)
      After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] = *CallSP;
    if (CheckCalls && Op.Output.isReg() &&
        Op.Output.Offset / x86reg::GeneralRegStride ==
            x86reg::RSP / x86reg::GeneralRegStride) {
      const auto SP = parentStackOffset(After);
      if (!SP)
        After.InitializedFrameBytes.clear();
      else {
        if (!charge(After.InitializedFrameBytes.size()))
          break;
        After.InitializedFrameBytes.erase(
            After.InitializedFrameBytes.begin(),
            After.InitializedFrameBytes.lower_bound(*SP));
      }
    }
    if (NoReturnAtExit)
      break;
  }
  for (auto It = Stores.lower_bound(Block.StartAddr);
       It != Stores.end() && It->first < Block.EndAddr; ++It)
    if ((!NoReturnAtExit || It->first < NoReturnEnd) &&
        !ProvenStores.count(It->first))
      Invalidate();
  if (Facts[I].Invalid)
    After.Unknown = true;
  Facts[I].InstalledAtExit = After.Installed;
  Facts[I].CxxContinuationAtExit = CatchReturn.has_value();
  Facts[I].NoReturnAtExit = NoReturnAtExit;
  if (!NoReturnAtExit) {
    for (int Successor : Block.Succs) {
      auto It = Index.find(Successor);
      if (It == Index.end()) {
        Result.Diagnostics.push_back(
            "registration-state CFG has a missing edge");
        return false;
      }
      merge(It->second, After);
    }
  }
  if (CatchReturn) {
    const auto Resume = Entries.find(CatchReturn->TargetVA);
    if (Resume != Entries.end()) {
      if (!charge(After.Frame.cellCount() + After.RuntimeObject.cellCount() +
                  2))
        return true;
      Domain Continued = After;
      auto Stack = *Continued.CxxCatchStacks.begin();
      Stack.pop_back();
      Continued.Parent = Stack.empty();
      Continued.Callback = !Stack.empty();
      Continued.OtherCallback = false;
      Continued.CxxCatchStacks.clear();
      if (!Stack.empty())
        Continued.CxxCatchStacks.insert(std::move(Stack));
      Continued.RuntimeIdentity.reset();
      const int32_t SavedSlot = *Chain.RegistrationOffset - 4;
      Continued.Frame.store(SavedSlot, 4,
                            FrameValue::frame(CatchReturn->SavedStackOffset));
      Continued.RuntimeObject.store(SavedSlot, 4, {});
      if (Chain.RealignedFrame) {
        Continued.Frame.leaveCallback();
        // The CRT's call frame does not promise the callback's general
        // registers at its continuation. The restore block rebuilds them.
        for (auto &Register : Continued.Frame.Registers)
          Register = {{}, {}, false, true};
        Continued.Frame.OtherRegisterBytes.clear();
        Continued.Frame.OtherRegistersMayBeFrame = true;
      }
      Continued.Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
          FrameValue::frame(0);
      Continued.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
          FrameValue::frame(CatchReturn->SavedStackOffset);
      merge(Resume->second, Continued);
    }
  }
  dispatchBlock(I, Before);
  return true;
}

} // namespace neverd::registration_state
