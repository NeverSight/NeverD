//===- RegistrationStateTransfer.cpp - x86 EH ordinary transfers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Transfer ordinary x86 operations through registration and frame state.
///
//===----------------------------------------------------------------------===//

#include "RegistrationFrameEffects.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFSEH4Entry.h"

#include "llvm/ADT/STLExtras.h"

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

bool RegistrationStateSolver::transferBlock(size_t I, const Domain &Before,
                                            BlockFacts &Facts) {
  const LowBlock &Block = Function.Blocks[I];
  if (!charge(Before.catchCellCount() + Before.sehCallerCellCount() + 1))
    return true;
  Domain After = Before;
  std::optional<Domain> CallException;
  bool UnprovedExceptionalBindings = false;
  FrameTransfer Transfer(
      After.Frame, *Chain.RegistrationOffset,
      EH4 || CxxCookies || CxxPrologue ? SecurityCookieVA : 0,
      Chain.RealignedFrame ? &*Chain.RealignedFrame : nullptr);
  FrameTransfer RuntimeTransfer(After.RuntimeObject, *Chain.RegistrationOffset);
  std::set<va_t> ProvenStores;
  std::optional<RegistrationCxxContinuation> CatchReturn;
  bool NoReturnAtExit = false;
  va_t NoReturnEnd = InvalidVA;
  va_t InvocationEnd = InvalidVA;
  auto Invalidate = [&] {
    Facts.Invalid = true;
    After.Unknown = true;
    After.Levels.clear();
  };
  if (SEHControls && !KnownCxx && Before.Callback && Before.Activation.Entry &&
      FinallyTargets.count(*Before.Activation.Entry) &&
      !validateSEHInvocationBlock(I)) {
    Invalidate();
    Result.SEHInvocationExitsComplete = false;
  }
  for (const LowOp &Op : Block.Ops) {
    Transfer.beginInstruction(Op.Addr);
    RuntimeTransfer.beginInstruction(Op.Addr);
    const auto Memory = lowMemoryOperands(Op);
    size_t MemoryWork = 0;
    if (Memory.Address && Op.Opcode == NdOp::LOAD &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto Address = Transfer.read(*Memory.Address);
      MemoryWork = After.Frame.memoryAccessWork(Address, Memory.AccessSize);
      if (CheckRuntimeObjects && Address.Offset)
        MemoryWork += After.RuntimeObject.memoryAccessWork(
            FrameValue::frame(*Address.Offset), Memory.AccessSize);
    }
    // Known frame accesses visit an ordered interval. Ambiguous addresses
    // retain the full-scan charge before inspecting possible pointer cells.
    size_t RegisterBytes = Op.Output.Size;
    for (unsigned Input = 0; Input != Op.NumInputs; ++Input)
      RegisterBytes += Op.Inputs[Input].Size;
    if (!charge(1 + RegisterBytes + MemoryWork))
      break;
    const bool SEH4Entry = SEH4Prologue && Op.Addr == SEH4Prologue->CallVA;
    const auto SEH4Exit = SEH4EpilogueIndex.find(Op.Addr);
    if (SEH4Entry || SEH4Exit != SEH4EpilogueIndex.end()) {
      const bool Applied = SEH4Entry ? transferSEH4Prologue(I, After, Op, Facts)
                                     : transferSEH4Epilogue(I, After, Op, Facts,
                                                            *SEH4Exit->second);
      if (!Applied) {
        Invalidate();
        CompleteCalls = CompleteChainOperations = CompleteCookies = false;
      } else {
        const auto Value =
            After.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride];
        LowOp Returned;
        Returned.Opcode = NdOp::COPY;
        Returned.Output = Op.Output;
        Transfer.write(Returned, Value);
        auto [It, New] =
            FrameValues.emplace(std::make_pair(Op.Addr, Op.Seq), Value);
        if (!New)
          It->second = registration_state::join(It->second, Value);
      }
      continue;
    }
    if (CxxPrologue && Op.Addr == CxxPrologue->CallVA) {
      if (!transferCxxPrologue(I, After, Op, Facts)) {
        Invalidate();
        CompleteCalls = CompleteChainOperations = CompleteCookies = false;
      } else {
        const auto Value = FrameValue::frame(CxxPrologue->RegistrationOffset);
        LowOp Returned;
        Returned.Opcode = NdOp::COPY;
        Returned.Output = Op.Output;
        Transfer.write(Returned, Value);
        if (CheckRuntimeObjects)
          RuntimeTransfer.write(Returned, {});
        auto [It, New] =
            FrameValues.emplace(std::make_pair(Op.Addr, Op.Seq), Value);
        if (!New)
          It->second = registration_state::join(It->second, Value);
      }
      continue;
    }
    if (const auto Exit = CxxEpilogueIndex.find(Op.Addr);
        Exit != CxxEpilogueIndex.end()) {
      if (!transferCxxEpilogue(I, After, Op, Facts, *Exit->second)) {
        Invalidate();
        CompleteCalls = CompleteChainOperations = CompleteCookies = false;
      } else {
        const auto Value =
            After.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride];
        LowOp Returned;
        Returned.Opcode = NdOp::COPY;
        Returned.Output = Op.Output;
        Transfer.write(Returned, Value);
        if (CheckRuntimeObjects)
          RuntimeTransfer.write(
              Returned, After.RuntimeObject
                            .Registers[x86reg::RAX / x86reg::GeneralRegStride]);
        auto [It, New] =
            FrameValues.emplace(std::make_pair(Op.Addr, Op.Seq), Value);
        if (!New)
          It->second = registration_state::join(It->second, Value);
      }
      continue;
    }
    if (!StackImportSlots.empty()) {
      if (!charge(StackImportSlots.size() + 1))
        break;
      Transfer.invalidateImportBindings(Op, StackImportSlots);
    }
    const auto ImportLoad = StackImportLoads.find({Op.Addr, Op.Seq});
    FrameValue Value =
        Transfer.evaluate(Op, After.Installed,
                          ImportLoad == StackImportLoads.end()
                              ? std::nullopt
                              : std::optional<va_t>{ImportLoad->second});
    std::optional<FrameValue> CallResult;
    if (Conditional) {
      if (Conditional->EvaluateOriginalImage && !Value.SecurityCookie)
        if (const auto Original =
                Conditional->EvaluateOriginalImage(Op, Transfer, WorkUsed))
          Value = *Original;
      // The caller's physical registration address changes during native
      // reconstruction. Preserve its provenance through arithmetic and spills
      // while retaining the exact identity for chain install/remove checks.
      Value.MayBeFrame |= Value.PreviousChain;
      checkConditionalNativeFrame(After.Frame, Op, Transfer, *Conditional);
    }
    if (Memory.Address && Memory.StoredValue &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      invalidateJumpRuntimeBindings(After, Transfer.read(*Memory.Address),
                                    Memory.AccessSize);
    if (Conditional && (!checkConditionalMemory(After.Frame, Op, Transfer,
                                                &After.InitializedFrameBytes) ||
                        !conditionalReturn(After, Op) ||
                        !conditionalCall(After, Op, CallResult)))
      Invalidate();
    FrameValue RuntimeValue;
    if (CheckRuntimeObjects) {
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) {
        const auto Address = Transfer.read(Op.Inputs[0]);
        if (Address.Offset)
          RuntimeValue =
              After.RuntimeObject.load(Address.Offset, Op.Output.Size);
      } else
        RuntimeValue = RuntimeTransfer.evaluate(Op, After.Installed);
    }
    if (CheckRuntimeObjects &&
        (Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::COND_BR ||
         Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::INDIR_CALL))
      for (unsigned Input = 0; Input != Op.NumInputs; ++Input)
        if (RuntimeTransfer.read(Op.Inputs[Input]).MayBeFrame)
          CompleteRuntimeObjects = false;
    recordCatchReturn(I, After, Op, Transfer, Facts, CatchReturn);
    if (!KnownCxx && After.Callback && Op.Opcode == NdOp::RETURN) {
      if (!callbackCanReturn(After) || !callbackReturnInstruction(I, Op)) {
        Invalidate();
        if (SEHControls)
          Result.SEHInvocationExitsComplete = false;
      } else
        Facts.CallbackReturnAtExit = true;
    }
    transferSEHExit(I, After, Op, Facts);
    // Different activations can execute the same source occurrence. Remember
    // scalar/unknown observations too: a frame value in only one invocation
    // cannot authorize an exact frame projection for the shared instruction.
    if (Op.Seq >= 0 && Op.Output.Size != 0) {
      if (!charge(1))
        break;
      auto [It, New] =
          FrameValues.emplace(std::make_pair(Op.Addr, Op.Seq),
                              Value.MayBeFrame ? Value : FrameValue{});
      if (!New)
        It->second = registration_state::join(It->second, Value);
    }
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
        recordRuntimeMemory(I, After, Op, Transfer, RuntimeTransfer, Facts);
    RuntimeMemory |= recordSEHExceptionMemory(I, After, Op, Transfer, Facts);
    if (Exhausted)
      break;

    if ((EH4 || CxxCookies) && Op.Opcode == NdOp::INTRINSIC &&
        !isFramePreservingIntrinsic(Op))
      CompleteCookies = false;
    if ((EH4 || CxxCookies) && Memory.Address &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto Address = Transfer.read(*Memory.Address);
      if (!Memory.Complete)
        CompleteCookies = false;
      if (After.Installed && !RuntimeMemory && !Address.CallbackAddress &&
          !Address.EntryOffset && (!Address.Offset && Address.MayBeFrame))
        CompleteCookies = false;
      if (After.Installed && Address.Offset) {
        auto Touches = [&](std::optional<int32_t> Slot) {
          return Slot && overlaps(*Address.Offset, Memory.AccessSize, *Slot, 4);
        };
        // Source cookie storage is owned only by its checked initialization
        // and the native runtime. Reads or writes after installation would
        // observe or change a synthetic cookie instead of the physical one.
        const bool CxxSlot =
            llvm::any_of(CxxCookieConditions, [&](const auto &Condition) {
              return Address.Offset == Condition.FrameOffset;
            });
        const bool CheckedGSRead =
            CookieCheckVA && Op.Opcode == NdOp::LOAD &&
            (Address.Offset == GSCookieSlot || CxxSlot) &&
            Memory.AccessSize == 4;
        ReadsGSCookie |= CheckedGSRead;
        if (EH4 && (Touches(EHCookieSlot) ||
                    (Touches(GSCookieSlot) && !CheckedGSRead) ||
                    overlaps(*Address.Offset, Memory.AccessSize,
                             *Chain.RegistrationOffset + 8, 4)))
          CompleteCookies = false;
        for (const auto &Condition : CxxCookieConditions)
          if (Touches(Condition.FrameOffset) && !CheckedGSRead)
            CompleteCookies = false;
      }
      if (Memory.StoredValue) {
        const uint64_t TableEnd =
            Chain.ScopeTableVA + 16 + uint64_t(Chain.Scopes.size()) * 12;
        // The callback and realignment owners above prove their separate
        // stack coordinates disjoint from the establisher and image. Neither
        // the callback stack nor the saved entry registers alias a cookie.
        if (!Address.Offset && !Address.EntryOffset &&
            !Address.CallbackAddress &&
            (!Address.preferredAddress() ||
             (uint64_t(*Address.preferredAddress()) < SecurityCookieVA + 4 &&
              SecurityCookieVA <
                  uint64_t(*Address.preferredAddress()) + Memory.AccessSize) ||
             (EH4 && uint64_t(*Address.preferredAddress()) < TableEnd &&
              Chain.ScopeTableVA <
                  uint64_t(*Address.preferredAddress()) + Memory.AccessSize)))
          CompleteCookies = false;
      }
    }
    // A call also writes its return PC below the current ESP. A later ESP
    // restoration cannot make a cookie that was deallocated at this exact
    // call remain intact for exceptional dispatch.
    if (CxxCookies && After.Installed &&
        (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
        !cxxCookiesReady(After))
      CompleteCookies = false;
    bool CheckedCxxCookieCall = false;
    if (CookieCheckVA && Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
        Op.Inputs[0].isConst() && Op.Inputs[0].Offset == CookieCheckVA) {
      const auto Boundary = Boundaries.find(Op.Addr);
      if (!Transfer.isCookieCheck(Op, CookieCheckVA) || Op.Seq < 0 ||
          Boundary == Boundaries.end() || Boundary->second.first != Block.Id ||
          (CxxCookies &&
           Boundary->second.second.Control != LowInstructionControl::Call))
        CompleteCookies = false;
      else {
        CookieChecks.emplace(
            std::make_pair(Op.Addr, Op.Seq),
            RegistrationCookieCheck{
                Op.Addr, Op.Addr + Boundary->second.second.Size, Op.Seq});
        CheckedCxxCookieCall = CxxCookies;
        if (CxxCookies)
          ImageReads.emplace(SecurityCookieVA, SecurityCookieVA + 4);
      }
    }
    if (!validateSEHParentChainUse(After, Op, Transfer))
      Result.SEHParentChainComplete = false;
    if (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)) {
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
      } else if (Address.preferredAddress() && !Address.MayBeFrame)
        ImageReads.emplace(*Address.preferredAddress(),
                           va_t(*Address.preferredAddress()) +
                               Memory.AccessSize);
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
          !Address.CallbackAddress && !RuntimeMemory)
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
            !After.Installed && registrationInstallationReady(After.Frame)) {
          After.Levels = {*Chain.SeededTryLevel};
          After.Unknown = false;
          After.Uninstalled = false;
          After.Installed = true;
          After.AbandonedSEHGuard = false;
          After.JumpCaptures.clear();
          ProvenInstallation = true;
          if (EH4 && !cookiesReady(After.Frame))
            CompleteCookies = false;
          if (CxxCookies && !cxxCookiesReady(After))
            CompleteCookies = false;
          recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Install);
        } else if (Address.Constant == uint32_t{0} && Width == 4 && AtEnd &&
                   Stored.Offset == Chain.RegistrationOffset &&
                   canReconnectSEHParentChain(After)) {
          // Republish the same live node after abandoning a CRT callback.
          // Keep its current TryLevel and buffer bindings, unlike first entry.
          After.AbandonedSEHGuard = false;
          recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Install);
        } else if (Address.Constant == uint32_t{0} && Width == 4 && AtEnd &&
                   Stored.PreviousChain && After.Installed &&
                   !After.Uninstalled) {
          After.Levels.clear();
          After.Unknown = false;
          After.Uninstalled = true;
          After.Installed = false;
          After.AbandonedSEHGuard = false;
          After.JumpCaptures.clear();
          recordChainAccess(Op, Block, RegistrationChainAccess::Kind::Remove);
          if (!KnownCxx && After.Callback &&
              !transferSEHParentFallthrough(I, After, Op, Facts))
            Invalidate();
        } else {
          CompleteChainOperations = false;
          Invalidate();
          After.Installed = After.Uninstalled = true;
        }
      } else if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        invalidateJumpCaptures(After, Address, Width);
        if (Chain.hasCxxCallbackStack() && !After.Installed &&
            After.Uninstalled && Address.Offset == Chain.TryLevelOffset &&
            Width == 4 && Stored.Constant == uint32_t(*Chain.SeededTryLevel)) {
          const auto Observation = Stores.find(Op.Addr);
          if (Observation != Stores.end() && Observation->second.Width == 4 &&
              Observation->second.Level == *Chain.SeededTryLevel)
            ProvenStores.insert(Op.Addr);
        }
        if (After.Installed && Address.MayBeFrame && !Address.EntryOffset &&
            !Address.CallbackAddress &&
            (!Address.Offset ||
             overlaps(*Address.Offset, Width, *Chain.TryLevelOffset, 4))) {
          std::set<int32_t> WrittenLevels;
          const bool Valid =
              !After.Uninstalled &&
              transferTryLevelStore(I, Op, Address, Stored, After.Levels,
                                    After.Unknown, WrittenLevels);
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
        if (After.Installed && Chain.RealignedFrame &&
            Chain.RealignedFrame->hasSavedParentFrame() && Address.Offset &&
            overlaps(*Address.Offset, Width,
                     Chain.RealignedFrame->SavedParentFrameOffset, 4))
          Invalidate();
        if (Address.Offset) {
          if (!charge(After.Frame.memoryAccessWork(Address, Width, true) + 1))
            break;
          After.Frame.store(*Address.Offset, Width, Stored);
          if (CheckCalls || Conditional) {
            auto SP = parentStackOffset(After);
            if (Conditional && !SP && After.Frame.CallbackEntry)
              SP = After.Frame.load(*Chain.RegistrationOffset - 8, 4).Offset;
            const int64_t End = int64_t(*Address.Offset) + Width;
            bool Borrowed = false;
            if (KnownCxx && Conditional && *Address.Offset >= 8 &&
                End <= INT32_MAX) {
              if (!charge(Conditional->Objects.size()))
                break;
              Borrowed = llvm::any_of(Conditional->Objects, [&](const auto &R) {
                return R.Begin <= *Address.Offset && End <= R.End;
              });
            }
            if (Borrowed || (SP && *Address.Offset >= *SP &&
                             End <= (Conditional ? 4 : 0))) {
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
              !charge(After.Frame.memoryAccessWork(Address, Width, true) +
                      Width + 1))
            Invalidate();
          else
            After.Frame.storeCallback(Address.CallbackAddress->Offset, Width,
                                      Stored);
        } else if (Address.EntryOffset) {
          // Only the checked entry's saved-register area is disjoint from
          // the newly allocated frame. Do not guess aliases below it.
          if (*Address.EntryOffset < -12 ||
              int64_t(*Address.EntryOffset) + Width > 4 || After.Installed ||
              !charge(After.Frame.memoryAccessWork(Address, Width, true) + 1))
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
            !Address.preferredAddress()) {
          After.InitializedFrameBytes.clear();
          if (!charge(After.Frame.cellCount()))
            break;
          After.Frame.forgetCellValues();
        }
      }
    }
    std::optional<FrameValue> CallSP;
    bool CheckedCall = false;
    bool CallPreservesImportBindings = false;
    if (!KnownCxx && !After.JumpBindingsIntact &&
        (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)) {
      if (!charge(1))
        break;
      InvalidRuntimeBindings.emplace(Op.Addr, Op.Seq);
    }
    const bool JumpCapture = transferJumpCapture(I, After, Op, Facts, CallSP);
    const bool JumpResume = transferJumpResume(I, After, Op, Facts);
    const auto LocalSP = !JumpCapture && !JumpResume
                             ? transferLocalUnwind(I, After, Op, Facts)
                             : std::nullopt;
    if (LocalSP)
      CallSP = *LocalSP;
    if (CheckedCxxCookieCall)
      CallSP = After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
    if (!CheckedCxxCookieCall && !JumpCapture && !JumpResume && !LocalSP &&
        !transferOrdinaryFinally(I, After, Op, Facts, CallSP))
      if (auto Call = transferCall(I, After, Op, Facts)) {
        CheckedCall = true;
        CallPreservesImportBindings = Call->PreservesImportBindings;
        CallSP = Call->StackPointer;
        CallResult = Call->Result;
        if (!Call->MayWrites.empty()) {
          if (!charge(Before.Frame.cellCount() + 1))
            break;
          if (!CallException)
            CallException = Before;
          for (const auto &Write : Call->MayWrites) {
            if (!charge(CallException->Frame.cellCount() + 1))
              break;
            CallException->Frame.forgetCellValues(
                Write.Begin, uint32_t(Write.End - Write.Begin));
          }
        }
        NoReturnAtExit = Call->DoesNotReturn;
        if (NoReturnAtExit)
          NoReturnEnd = Call->EndAddress;
      }
    if (!CallSP)
      CallSP = transferControlFP(Before, After, Op, Facts, CallException);
    if (!CallSP)
      CallSP = transferStartupInfo(Before, After, Op, Facts, CallException);
    if (!CallSP && JumpRuntime && JumpRuntime->Raise &&
        After.JumpBindingsIntact && !After.Unknown && !Facts.Invalid &&
        After.Installed && !After.Uninstalled &&
        !After.Activation.SearchFilter &&
        JumpRuntime->Raise(Op, After.Frame, WorkUsed)) {
      const auto SP =
          After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride];
      if (SP.Offset && *SP.Offset <= int64_t(*Chain.RegistrationOffset) - 24) {
        recordSEHRuntimeCall(After, Op,
                             RegistrationSEHBoundCall::Kind::RaiseException);
        CallSP = FrameValue::frame(*SP.Offset + 16);
      } else if (SP.CallbackAddress && After.Frame.callbackMemoryIsPrivate(
                                           *SP.CallbackAddress, 16, true)) {
        recordSEHRuntimeCall(After, Op,
                             RegistrationSEHBoundCall::Kind::RaiseException);
        CallSP = FrameValue::callbackFrame(SP.CallbackAddress->Entry,
                                           SP.CallbackAddress->Offset + 16);
        if (After.Callback && After.Activation.Entry &&
            FinallyTargets.count(*After.Activation.Entry) &&
            !applySEHContinueFilters(I, After, Facts)) {
          Invalidate();
          Result.SEHInvocationExitsComplete = false;
        }
      }
    }
    if (!transferBoundSEHCallee(I, Before, After, Op, Facts, CallSP,
                                CallException, CallResult))
      break;
    if (!CallSP && !JumpCapture && !JumpResume)
      CallSP = transferPrivateSEHCall(After, Op, Facts);
    const bool UnprovedCallEffects =
        !JumpCapture && !JumpResume && !CallSP && !Facts.InvocationAtExit &&
        (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
         (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)));
    if (UnprovedCallEffects) {
      if (After.Callback)
        for (const auto &[Offset, Capture] : After.JumpCaptures)
          ModifiedCallbackCaptures.insert(Capture.Call);
      After.JumpCaptures.clear();
      UnprovedExceptionalBindings = true;
    }
    if (!JumpResume && !CallSP && !Facts.InvocationAtExit && SEHControls &&
        !KnownCxx && Before.Callback && Before.Activation.Entry &&
        FinallyTargets.count(*Before.Activation.Entry) &&
        ((Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)) ||
         ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          !LocalUnwindTargets.count({Op.NumInputs && Op.Inputs[0].isConst()
                                         ? Op.Inputs[0].Offset
                                         : InvalidVA,
                                     Op.Opcode == NdOp::INDIR_CALL})))) {
      // A returning stack declaration does not bound effects on the parent
      // or on the suspended invocation. Other runtime calls need their own
      // argument and callback contracts before they can enter this domain.
      Invalidate();
      Result.SEHInvocationExitsComplete = false;
    }
    // The CFG's exact instruction already owns the no-return decision.
    // A missing memory-borrow contract cannot create an ordinary return from
    // it. Keep transferCall's refusal and the exceptional dispatch below.
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      const auto It = Boundaries.find(Op.Addr);
      if (Op.Seq >= 0 && It != Boundaries.end() &&
          It->second.first == Block.Id) {
        const auto &Insn = It->second.second;
        const size_t OpIndex = &Op - Block.Ops.data();
        if (Insn.Control == LowInstructionControl::Call &&
            isUnconditionalNoReturn(Insn) && OpIndex >= Insn.FirstOp &&
            OpIndex - Insn.FirstOp < Insn.OpCount &&
            Insn.FirstOp <= Block.Ops.size() &&
            Insn.OpCount <= Block.Ops.size() - Insn.FirstOp) {
          NoReturnAtExit = true;
          NoReturnEnd = Insn.Address + Insn.Size;
        }
      }
    }
    // Stack balance alone does not discharge transferCall's memory, object
    // initialization or no-return obligations. Retain their refusal while
    // recovering the independent register coordinate for ordinary flow.
    bool ImportedCall = false;
    bool PreserveImportBindings = false;
    if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
        Op.NumInputs == 1 && Op.Inputs[0].Size == 4 &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Op.MemoryOrdering == NdMemoryOrdering::None) {
      const auto Target =
          Op.Inputs[0].isConst() ? std::optional<va_t>{Op.Inputs[0].Offset}
          : Op.Opcode == NdOp::INDIR_CALL
              ? std::optional<va_t>{Transfer.read(Op.Inputs[0]).ImportTarget}
              : std::nullopt;
      const auto It =
          Target ? StackPops.find({*Target, Op.Opcode == NdOp::INDIR_CALL})
                 : StackPops.end();
      const auto Boundary = Boundaries.find(Op.Addr);
      if (It != StackPops.end() && Op.Seq >= 0 &&
          (!Op.Inputs[0].isConst() || After.Frame.ImportBindingsIntact ||
           (Op.Opcode == NdOp::CALL && !StackBindingCallees.count(*Target))) &&
          Boundary != Boundaries.end() && Boundary->second.first == Block.Id &&
          Boundary->second.second.Control == LowInstructionControl::Call) {
        ImportedCall =
            Op.Opcode == NdOp::INDIR_CALL || StackImportVeneers.count(*Target);
        PreserveImportBindings = StackBindingPreservers.count(It->first);
        if (!CallSP) {
          LowOp Adjust;
          Adjust.Opcode = NdOp::INT_ADD;
          Adjust.Output = NdVar::reg(x86reg::RSP, 4);
          Adjust.NumInputs = 2;
          Adjust.Inputs[0] = Adjust.Output;
          Adjust.Inputs[1] = NdVar::cst(It->second, 4);
          CallSP = Transfer.evaluate(Adjust, After.Installed);
        }
      }
    }
    // Stack cleanup does not preserve a future runtime target. A separate
    // imported normal-return effect may retain a still-live binding, but it
    // cannot repair earlier loss or authorize the exceptional path above.
    if (UnprovedCallEffects && !(ImportedCall && PreserveImportBindings))
      After.JumpBindingsIntact = false;
    recordSEHStackRestore(Op, After, Transfer, Value);
    Transfer.write(Op, Value, CookieCheckVA, ImportedCall,
                   PreserveImportBindings || CallPreservesImportBindings);
    if (JumpCapture)
      After.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
          FrameValue::constant(0);
    if (CheckRuntimeObjects) {
      if (!CheckedCxxCookieCall)
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
    if (CallResult) {
      After.Frame.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
          *CallResult;
      LowOp Returned;
      Returned.Opcode = NdOp::COPY;
      Returned.Output = Op.Output;
      Transfer.write(Returned, *CallResult);
      if (CheckRuntimeObjects)
        RuntimeTransfer.write(Returned, {});
    }
    if (const auto SP = parentStackOffset(After))
      std::erase_if(After.JumpCaptures,
                    [&](const auto &Capture) { return *SP > Capture.first; });
    if ((CheckCalls || Conditional) &&
        !(Conditional &&
          After.Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride]
              .CallbackAddress) &&
        Op.Output.isReg() &&
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
    if (Facts.InvocationAtExit && Op.Opcode == NdOp::CALL) {
      const auto Source = Boundaries.find(Op.Addr);
      if (Source != Boundaries.end())
        InvocationEnd =
            Source->second.second.Address + Source->second.second.Size;
      break;
    }
    if (NoReturnAtExit || (JumpResume && Facts.InvocationAtExit))
      break;
  }
  for (auto It = Stores.lower_bound(Block.StartAddr);
       It != Stores.end() && It->first < Block.EndAddr; ++It)
    if ((!NoReturnAtExit || It->first < NoReturnEnd) &&
        (InvocationEnd == InvalidVA || It->first < InvocationEnd) &&
        !ProvenStores.count(It->first))
      Invalidate();
  if (Facts.Invalid)
    After.Unknown = true;
  Facts.InstalledAtExit = After.Installed;
  Facts.CxxContinuationAtExit = CatchReturn.has_value();
  Facts.NoReturnAtExit = NoReturnAtExit;
  if (!NoReturnAtExit && !Facts.InvocationAtExit) {
    const auto SelectedBranch = !Facts.Invalid && !After.Unknown
                                    ? knownSuccessor(Block, Transfer)
                                    : std::nullopt;
    for (int Successor : Block.Succs) {
      auto It = Index.find(Successor);
      if (It == Index.end()) {
        Result.Diagnostics.push_back(
            "registration-state CFG has a missing edge");
        return false;
      }
      if (!SelectedBranch ||
          Function.Blocks[It->second].StartAddr == *SelectedBranch)
        merge(It->second, After);
    }
  }
  if (CatchReturn)
    resumeCxxCatch(After, *CatchReturn);
  // A call or explicit image write can change a runtime binding before a
  // fault dispatches this block. Do not lend the pre-block binding or capture
  // to a filter/finally after that possible effect.
  if (Before.JumpBindingsIntact &&
      (UnprovedExceptionalBindings || !After.JumpBindingsIntact)) {
    if (!CallException) {
      if (!charge(Before.snapshotWork()))
        return true;
      CallException = Before;
    }
    CallException->JumpBindingsIntact = false;
    CallException->JumpCaptures.clear();
  }
  dispatchBlock(I, CallException ? *CallException : Before, Facts);
  return true;
}

} // namespace neverd::registration_state
