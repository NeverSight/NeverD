//===- X86LegacyExecution.cpp - Authenticated protected-mode steps --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Connect original-byte fetch, segment transitions and persistent IDT state.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "X86LegacyControlRegisters.h"
#include "X86LegacyEncoding.h"
#include "X86LegacyExecutionInternal.h"
#include "X86LegacyFarTransfer.h"
#include "X86LegacyFastSystem.h"
#include "X86LegacyMpx.h"
#include "X86LegacyPortIO.h"
#include "X86LegacyScalar.h"
#include "X86LegacySegmentEncoding.h"
#include "X86LegacyString.h"
#include "X86LegacyTaskSwitch.h"

#include "neverd/ir/X86/X86DebugRules.h"
#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/lift/X86/X86DebugRegisterEncoding.h"
#include "neverd/translate/X86/X86WatchpointObservation.h"

#include "llvm/Support/Errc.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

struct LegacyInstruction {
  std::optional<X86DebugRegisterEncoding> DebugRegister;
  std::optional<detail::X86LegacyControlInstruction> ControlRegister;
  std::optional<detail::X86LegacySegmentInstruction> Segment;
  std::optional<detail::X86LegacyScalarInstruction> Scalar;
  std::optional<detail::X86LegacyFastSystemInstruction> FastSystem;
  std::optional<detail::X86LegacyStringInstruction> String;
  std::optional<detail::X86LegacyPortInstruction> Port;
  std::optional<detail::X86LegacyFarTransfer> Far;
  std::optional<X86MpxEncoding> Mpx;
  llvm::ArrayRef<uint8_t> Body;
  unsigned Width;
  bool IRET, RETF, INT, UD2, HLT = false, LockedHLT = false;
};

llvm::Expected<LegacyInstruction>
decodeInstruction(llvm::ArrayRef<uint8_t> Instruction, bool Code32) {
  if (Instruction.empty() || Instruction.size() > 15)
    return invalid("invalid legacy instruction length");
  auto DebugRegister = x86DebugRegisterEncoding(Instruction, false);
  if (DebugRegister && DebugRegister->Size != Instruction.size())
    return invalid("MOV-DR binding contains trailing bytes");
  LegacyInstruction I{
      DebugRegister,
      detail::decodeLegacyControlRegister(Instruction, Code32),
      detail::decodeLegacySegmentInstruction(Instruction, Code32),
      detail::decodeLegacyScalarInstruction(Instruction, Code32),
      detail::decodeLegacyFastSystem(Instruction, Code32),
      detail::decodeLegacyString(Instruction, Code32),
      detail::decodeLegacyPortIO(Instruction, Code32),
      detail::decodeLegacyFarTransfer(Instruction, Code32),
      Code32 ? decodeX86MpxEncoding(Instruction, Arch::X86) : std::nullopt,
      Instruction,
      Code32 ? 4u : 2u,
      false,
      false,
      false,
      false};
  if (I.Mpx && I.Mpx->Size != Instruction.size())
    return invalid("MPX binding contains trailing bytes");
  if (I.Body.front() == 0x66) {
    I.Width = 6 - I.Width;
    I.Body = I.Body.drop_front();
  }
  const bool Single = Instruction.size() == 1 &&
                      (Instruction[0] == 0x90 || Instruction[0] == 0xfa ||
                       Instruction[0] == 0xfb || Instruction[0] == 0xcc ||
                       Instruction[0] == 0xce || Instruction[0] == 0xf1);
  I.INT = Instruction.size() == 2 && Instruction[0] == 0xcd;
  I.UD2 = Instruction.size() == 2 && Instruction[0] == 0x0f &&
          Instruction[1] == 0x0b;
  I.IRET = I.Body.size() == 1 && I.Body[0] == 0xcf;
  I.RETF = (I.Body.size() == 1 && I.Body[0] == 0xcb) ||
           (I.Body.size() == 3 && I.Body[0] == 0xca);
  if (Instruction.back() == 0xf4) {
    detail::X86LegacyEncoding HaltEncoding(Instruction, Code32);
    I.HLT = HaltEncoding.byte() == 0xf4 && HaltEncoding.done();
    I.LockedHLT = HaltEncoding.Group1Prefix == 0xf0;
  }
  if (!I.Segment && !I.Scalar && !I.FastSystem && !I.String && !I.Port &&
      !I.Far && !Single && !I.INT && !I.UD2 && !I.IRET && !I.RETF &&
      !I.DebugRegister && !I.ControlRegister && !I.Mpx && !I.HLT)
    return invalid("unsupported or malformed legacy step instruction");
  return I;
}

} // namespace

llvm::Error
validateX86LegacyInstructionEncoding(llvm::ArrayRef<uint8_t> Instruction,
                                     bool Code32) {
  auto Decoded = decodeInstruction(Instruction, Code32);
  if (!Decoded)
    return Decoded.takeError();
  return llvm::Error::success();
}

llvm::Error detail::admitLegacyExecution(const X86LegacyExecutionState &S,
                                         X86PagedMemory &M,
                                         const X86LegacyExecutionOptions &O) {
  if (!O.EventDelivery.MaxAttempts)
    return invalid("event delivery requires a positive work budget");
  if (S.Halted &&
      (S.Core.CPL || S.STIBlocked || S.SSBlocked || S.PendingStack ||
       S.PendingMpx || S.PendingTask || S.PendingEvent))
    return invalid("legacy halt state requires completed CPL0 HLT state");
  if (bool(S.Debug) != bool(O.Debug) ||
      (S.PendingEvent && S.PendingEvent->DataBreakpoints &&
       (!S.Debug || !x86DebugWatchpoints(*O.Debug) ||
        (S.PendingEvent->DataBreakpoints & 0xf0))) ||
      (S.Debug && !x86InstructionDebugValid(
                      S.Debug->DR6, S.Debug->DR7, S.Debug->DebugCTL, *O.Debug,
                      M.controls().CR4, S.Debug->PendingData)))
    return invalid("legacy debug state requires a valid instruction-debug "
                   "profile, DR6 and supported DR7/DEBUGCTL");
  auto Controls = M.controls();
  Controls.RFlags = S.EFlags;
  if (S.PendingTask) {
    if (auto Error = detail::validateLegacyTaskState(S, M))
      return Error;
  } else {
    if (auto Error = validateX86LegacySegmentState(S.Core, Controls))
      return Error;
  }
  if (!(S.EFlags & 2) || (S.EFlags & ~uint32_t{0x3d7fd7}) ||
      (Controls.CR4 & ((uint64_t{1} << 23) | (uint64_t{1} << 32))))
    return invalid(
        "legacy execution requires ordinary protected flags and no CET/FRED");
  const auto &CS = S.Core.Segments[1];
  const unsigned DPL = (CS.Attributes >> 5) & 3;
  if (!S.PendingTask &&
      (DPL > S.Core.CPL || (!(CS.Attributes & 4) && DPL != S.Core.CPL)))
    return invalid("legacy execution requires an authenticated code privilege");
  if (O.Events != X86LegacyEventProfile::OrderedFrame ||
      (O.Segments.FullLimitChoice &&
       *O.Segments.FullLimitChoice != X86FullSegmentLimitChoice::Fault &&
       *O.Segments.FullLimitChoice != X86FullSegmentLimitChoice::Wrap))
    return invalid("legacy execution requires a bound event profile");
  return M.bindControls(Controls);
}

llvm::Expected<X86LegacyExecutionResult>
executeX86LegacyInstruction(X86LegacyExecutionState &State,
                            X86PagedMemory &Memory,
                            llvm::ArrayRef<uint8_t> Instruction,
                            const X86LegacyExecutionOptions &Options) {
  if (detail::legacyTaskModeUnavailable(State)) {
    if (auto Error = detail::validateLegacyTaskState(State, Memory))
      return std::move(Error);
    X86LegacyExecutionResult R;
    R.Status = X86LegacyExecutionStatus::ModeUnavailable;
    return R;
  }
  if (State.PendingEvent)
    return invalid(
        "resume the pending legacy event before another instruction");
  if (detail::legacyTaskFaultContext(State))
    return invalid(
        "deliver the incoming task fault before another instruction");
  if (State.Halted) {
    if (auto Error = detail::admitLegacyExecution(State, Memory, Options))
      return std::move(Error);
    X86LegacyExecutionResult R;
    R.Status = X86LegacyExecutionStatus::Halted;
    return R;
  }
  if (auto Error = detail::validateLegacyStackContinuation(State, Instruction))
    return std::move(Error);
  if (auto Error =
          detail::validateLegacyMpxContinuation(State, Memory, Instruction))
    return std::move(Error);
  const bool ResumingStack = State.PendingStack.has_value();
  const bool ResumingTask = detail::legacyTaskInstructionPending(State);
  const bool Resuming =
      ResumingStack || State.PendingMpx.has_value() || ResumingTask;
  const bool Code32 = ResumingTask
                          ? bool(detail::legacyTaskInstructionKey(State) >> 32)
                          : bool(State.Core.Segments[1].Attributes & 0x4000);
  auto Decoded = decodeInstruction(Instruction, Code32);
  if (!Decoded)
    return Decoded.takeError();
  auto &[DebugRegister, ControlRegister, Segment, Scalar, FastSystem, String,
         Port, Far, Mpx, Body, Width, IRET, RETF, INT, UD2, HLT, LockedHLT] =
      *Decoded;
  if (ControlRegister) {
    if (!Options.ControlRegisters)
      return invalid("MOV-CR requires a bound Intel control-register profile");
    if (auto Error = detail::validateLegacyControlBinding(
            Memory, *Options.ControlRegisters))
      return std::move(Error);
  }
  if (Scalar && Scalar->DescriptorQuery &&
      Options.DescriptorQueries != X86LegacyDescriptorProfile::TableImage)
    return invalid("legacy descriptor query requires a bound table profile");
  if (Scalar && Scalar->SystemSelector &&
      Options.SystemSelectors != X86LegacySystemSelectorProfile::ExclusiveTable)
    return invalid(
        "legacy system selector requires an exclusive table profile");
  if (Scalar &&
      Scalar->Operation ==
          detail::X86LegacyScalarOperation::StoreMachineStatus &&
      Scalar->Destination.Register && Scalar->Bytes == 4 &&
      Options.MachineStatus != X86LegacyMachineStatusProfile::FullCR0)
    return invalid("legacy dword SMSW requires an upper-word result profile");
  if (Scalar &&
      (Scalar->bitCounting() || Scalar->carryChain() || Scalar->BMI)) {
    if (!Options.IntegerExtensions)
      return invalid(
          "legacy integer extension requires a target feature binding");
    using Op = detail::X86LegacyScalarOperation;
    if (Scalar->Operation == Op::CountLeadingZeros &&
        !Options.IntegerExtensions->LZCNT)
      Scalar->Operation = Op::BitScanReverse;
    if (Scalar->Operation == Op::CountTrailingZeros &&
        !Options.IntegerExtensions->BMI1)
      Scalar->Operation = Op::BitScanForward;
    if ((Scalar->Operation == Op::CountLeadingZeros ||
         Scalar->Operation == Op::CountTrailingZeros) &&
        (!Options.BitCountUndefinedFlags ||
         (*Options.BitCountUndefinedFlags &
          ~kX86LegacyBitCountUndefinedFlagsMask)))
      return invalid("legacy zero count requires an undefined-flags image");
    if (Scalar->BMI && Scalar->BMI->enabled(*Options.IntegerExtensions)) {
      const uint32_t Mask = Scalar->BMI->undefinedFlagsMask();
      if (Mask &&
          (!Options.BMIUndefinedFlags || (*Options.BMIUndefinedFlags & ~Mask)))
        return invalid("legacy BMI requires an undefined-flags image");
    }
  }
  if (ResumingStack && (!Scalar || !Scalar->stackFrame()))
    return invalid("pending legacy stack state requires a stack instruction");
  if (DebugRegister && (!Options.Debug || !x86DebugRegisters(*Options.Debug)))
    return invalid("MOV-DR requires the debug-register profile");
  if (Far && Options.FarTransfers != X86LegacyFarTransferProfile::OrderedFrame)
    return invalid("legacy far transfer requires a bound transfer profile");
  if (Port) {
    if (!Options.PortIO)
      return invalid("legacy port step requires a bound device profile");
    if (auto Error = validateX86PortIOBinding(*Options.PortIO))
      return std::move(Error);
  }
  if (String &&
      Options.Strings != X86LegacyStringProfile::ElementwiseDestinationFirst)
    return invalid(
        "legacy string step requires a bound element-progress profile");
  if (String && String->Repeat && Options.Debug &&
      x86DebugWatchpoints(*Options.Debug) &&
      (!Options.RepeatDebug || !Options.RepeatDebug->EndGroup))
    return invalid("legacy REP watchpoints require a debug-group binding");
  if (Scalar &&
      !(Scalar->Operation == detail::X86LegacyScalarOperation::Move &&
        Scalar->Destination.Register && Scalar->Source.Immediate) &&
      Options.Scalars != X86LegacyScalarProfile::OrderedRAM)
    return invalid("legacy scalar step requires a bound ordinary-RAM profile");
  if (Scalar && Scalar->Atomic &&
      Options.Atomics != X86LegacyAtomicProfile::ExclusiveRAM)
    return invalid("legacy atomic step requires a bound exclusive-RAM profile");
  if (Scalar && Scalar->Adjustment &&
      (!Options.DecimalUndefinedFlags ||
       (*Options.DecimalUndefinedFlags &
        ~Scalar->Adjustment->undefinedFlagsMask())))
    return invalid(
        "legacy decimal adjustment requires an undefined-flags image");
  if (FastSystem && Options.FastSystem != X86LegacyFastSystemProfile::IntelSEP)
    return invalid("legacy fast transfer requires a bound Intel SEP profile");
  if (Scalar && Scalar->logical() && !Options.LogicalAuxiliaryCarry)
    return invalid("legacy logical instruction requires an explicit AF choice");
  if (Scalar && Scalar->division() &&
      (!Options.DivisionArithmeticFlags ||
       (*Options.DivisionArithmeticFlags & ~kX86LegacyArithmeticFlagsMask)))
    return invalid(
        "legacy division requires an explicit arithmetic-flags image");
  if (Scalar && Scalar->multiplication() &&
      Options.Multiply != X86LegacyMultiplyProfile::ProductSignParity &&
      Options.Multiply != X86LegacyMultiplyProfile::ProductSignParityZero)
    return invalid("legacy multiplication requires a bound flags profile");
  if (Scalar && Scalar->shifting() &&
      (!Options.ShiftUndefinedFlags ||
       (*Options.ShiftUndefinedFlags & ~kX86LegacyShiftUndefinedFlagsMask)))
    return invalid("legacy shift requires an explicit undefined-flags image");
  if (Scalar && Scalar->doubleShifting() &&
      (!Options.DoubleShiftUndefined ||
       (Options.DoubleShiftUndefined->Flags & ~kX86LegacyArithmeticFlagsMask)))
    return invalid("legacy double shift requires an explicit undefined state");
  if (Scalar && Scalar->bitTesting() &&
      (!Options.BitTestUndefinedFlags ||
       (*Options.BitTestUndefinedFlags & ~kX86LegacyBitTestUndefinedFlagsMask)))
    return invalid(
        "legacy bit test requires an explicit undefined-flags image");
  if (Scalar && Scalar->bitScanning() &&
      (!Options.BitScanChoices ||
       (Options.BitScanChoices->Flags & ~kX86LegacyBitScanChoiceFlagsMask)))
    return invalid("legacy bit scan requires explicit CPU choices");
  if (Scalar && Scalar->Operation == detail::X86LegacyScalarOperation::Call &&
      Options.Calls != X86LegacyCallProfile::StoreBeforeTargetLimit &&
      Options.Calls != X86LegacyCallProfile::CheckBeforeTargetLimit)
    return invalid("legacy CALL requires a bound stack publication profile");
  const auto Branch = x86MpxBranchEffect(Instruction, Arch::X86);
  std::optional<X86MpxContext> NextMpx;
  if ((Scalar &&
       Scalar->Operation == detail::X86LegacyScalarOperation::Bound) ||
      (Mpx && Mpx->Id != X86_INS_NOP) || Branch == X86MpxBranch::Initialize) {
    auto Bound = detail::bindLegacyMpx(State, Memory);
    if (!Bound)
      return Bound.takeError();
    NextMpx = std::move(*Bound);
    if (Mpx)
      if (auto Error = detail::admitLegacyMpxMemory(*Mpx, *NextMpx, State,
                                                    Memory, Options))
        return std::move(Error);
    // Only staged state changes here. Publish BNDINIT after successful branch
    // completion, including an untaken Jcc, and before its debug trap.
    if (Branch)
      NextMpx->completeBranch(*Branch);
  }
  const uint32_t EntryFlags =
      ResumingTask ? detail::legacyTaskEntryFlags(State) : State.EFlags;
  const bool SingleStep = EntryFlags & 0x100;
  const uint64_t DebugCTL = ResumingTask
                                ? detail::legacyTaskEntryDebugControl(State)
                            : State.Debug ? State.Debug->DebugCTL
                                          : 0;
  if (SingleStep && (!State.Debug || !Options.Debug))
    return invalid("legacy TF stepping requires a debug-event producer");
  const bool TaskReturn = IRET && (EntryFlags & 0x4000);
  if (TaskReturn && !Options.Tasks)
    return invalid("legacy nested-task IRET requires task-switch state");
  if (((IRET && !TaskReturn) || RETF) &&
      (Options.Returns != X86LegacyReturnProfile::OrderedFrame ||
       (Options.Segments.AlignmentPriority &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::BeforePaging &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::AfterPaging) ||
       (State.Core.CPL == 3 && (Memory.controls().CR0 & 0x40000) &&
        (State.EFlags & 0x40000) && !Options.Segments.AlignmentPriority)))
    return invalid("legacy return requires a bound return/alignment profile");
  if ((Segment || Scalar || String || Far || (Port && Port->String)) &&
      (Options.Segments.Profile !=
           X86LegacySegmentProfile::WordStackOffsetFirst ||
       (Options.Segments.AlignmentPriority &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::BeforePaging &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::AfterPaging) ||
       (State.Core.CPL == 3 && (Memory.controls().CR0 & 0x40000) &&
        (State.EFlags & 0x40000) && !Options.Segments.AlignmentPriority)))
    return invalid("legacy operand step requires a bound operand profile");
  if (auto Error = detail::admitLegacyExecution(State, Memory, Options))
    return std::move(Error);
  X86LegacyExecutionResult R;
  X86WatchpointObservation Observation(
      Memory, State.Debug ? &*State.Debug : nullptr,
      Options.Debug.value_or(X86InstructionDebugProfile::InstructionStep));
  if (ResumingStack)
    Observation.Matches = State.PendingStack->DataBreakpoints;
  if (State.PendingMpx)
    Observation.Matches = State.PendingMpx->DataBreakpoints;
  if (ResumingTask)
    Observation.Matches = detail::legacyTaskWatchpoints(State);
  const auto Accept = [&](const X86MemoryAccessResult &Got) {
    R.SelfModifications.insert(R.SelfModifications.end(),
                               Got.SelfModifications.begin(),
                               Got.SelfModifications.end());
    R.Exception = Got.Exception;
    R.BackingFault = Got.BackingFault;
    if (Got.BackingFault)
      R.Status = X86LegacyExecutionStatus::BackingFault;
    return Got.Status == X86PageWalkStatus::Completed;
  };
  const auto Enter =
      [&](const X86LegacyEvent &Event,
          bool Completed) -> llvm::Expected<X86LegacyExecutionResult> {
    Observation.stop();
    auto Delivered = deliverX86LegacyEvent(State, Memory, Event, Options);
    if (!Delivered)
      return Delivered.takeError();
    Delivered->DeviceTransferCompleted = R.DeviceTransferCompleted;
    Delivered->PortInputBytesWritten = R.PortInputBytesWritten;
    Delivered->InstructionAttempted = !Resuming;
    Delivered->InstructionCompleted =
        Completed && (Event.Kind == X86LegacyEventKind::Trap ||
                      Delivered->SoftwareCompleted);
    if (!Delivered->Exception)
      Delivered->Exception = R.Exception;
    Delivered->SelfModifications.insert(Delivered->SelfModifications.begin(),
                                        R.SelfModifications.begin(),
                                        R.SelfModifications.end());
    return Delivered;
  };
  const auto EnterDebug =
      [&](bool FromSingleStep, bool Completed, uint8_t Data = 0,
          bool Icebp = false) -> llvm::Expected<X86LegacyExecutionResult> {
    // Publish the debug cause before IDT entry: it survives a failed #DB
    // delivery and a backing stop. Software INT 1 does not update DR6.
    if (State.Debug) {
      const auto Matches =
          Data | ((FromSingleStep || Data) && !Icebp
                      ? x86ExecutionBreakpoints(
                            State.Debug->DR, State.Debug->DR7,
                            uint32_t(State.Core.Segments[1].Base + State.EIP),
                            State.EFlags, false)
                      : 0);
      x86PublishInstructionDebug(*State.Debug, FromSingleStep, Matches);
    }
    State.SSBlocked = State.STIBlocked = false;
    return Enter({State.EIP, 0, 0, 1, X86LegacyEventKind::Trap}, Completed);
  };
  const auto FinishTask = [&](const detail::X86LegacyTaskResult &Got)
      -> llvm::Expected<X86LegacyExecutionResult> {
    Accept(Got);
    R.InstructionAttempted = !Resuming;
    if (Got.ModeUnavailable) {
      detail::retainLegacyTaskWatchpoints(State, Observation.Matches);
      R.Status = X86LegacyExecutionStatus::ModeUnavailable;
      return R;
    }
    if (R.BackingFault) {
      detail::retainLegacyTaskWatchpoints(State, Observation.Matches);
      return R;
    }
    if (R.Exception)
      return Enter({State.EIP, R.Exception->ErrorCode,
                    uint32_t(R.Exception->CR2.value_or(0)), R.Exception->Vector,
                    X86LegacyEventKind::Fault},
                   false);
    // The task owner has published TR, the address space and incoming FLAGS.
    // Do not apply ordinary instruction RF clearing to this new task image.
    R.InstructionCompleted = true;
    if (State.Mpx && NextMpx)
      State.Mpx = std::move(NextMpx);
    const uint8_t Data =
        x86CompleteDataBreakpoints(*State.Debug, Observation.Matches, false);
    if (Got.DebugTrap)
      x86PublishTaskDebug(*State.Debug);
    // TF is saved with the old task; incoming TF starts after its first
    // instruction. Task-switch debug is independently selected by TSS.T.
    if (Got.DebugTrap || Data)
      return EnterDebug(false, true, Data);
    return R;
  };
  if (ResumingTask) {
    auto Got = detail::resumeLegacyTask(State, Memory, Instruction, Options);
    if (!Got)
      return Got.takeError();
    return FinishTask(*Got);
  }
  if (!Resuming && x86PendingVirtualInterrupt(
                       State.Core.CPL, Memory.controls().CR4, State.EFlags))
    R.Exception = X86MemoryException{13, 0, {}};
  if (State.Debug && !R.Exception && !Resuming) {
    const auto Matches = x86ExecutionBreakpoints(
        State.Debug->DR, State.Debug->DR7,
        uint32_t(State.Core.Segments[1].Base + State.EIP), State.EFlags,
        State.SSBlocked);
    if (Matches) {
      x86PublishInstructionDebug(*State.Debug, false, Matches);
      return Enter(
          {State.EIP, 0, 0, 1, X86LegacyEventKind::InstructionBreakpoint},
          false);
    }
  }
  for (unsigned I = 0; !Resuming && !R.Exception && I != Instruction.size();
       ++I) {
    uint8_t Byte = 0;
    auto Got =
        Memory.loadSegment({X86SegmentRegister::CS, State.Core.Segments[1],
                            State.EIP + I, MemoryAccessKind::Execute,
                            State.Core.CPL, Options.Segments.FullLimitChoice},
                           llvm::MutableArrayRef(&Byte, 1));
    if (!Got)
      return Got.takeError();
    if (!Accept(*Got))
      break;
    if (Byte != Instruction[I]) {
      R.Status = X86LegacyExecutionStatus::CodeChanged;
      return R;
    }
  }
  if (R.BackingFault)
    return R;
  if (!R.Exception && TaskReturn) {
    State.NMIBlocked = false;
    detail::X86LegacyTaskRequest Request{detail::X86LegacyTaskReason::Return};
    Request.SavedEIP = State.EIP + Instruction.size();
    Request.SavedFlags = State.EFlags;
    auto Got =
        detail::switchLegacyTask(State, Memory, Request, Instruction, Options);
    if (!Got)
      return Got.takeError();
    return FinishTask(*Got);
  }
  if (!R.Exception && (IRET || RETF)) {
    auto Returned = State;
    const uint16_t Discard = RETF && Body[0] == 0xca
                                 ? uint16_t(Body[1]) | uint16_t(Body[2]) << 8
                                 : 0;
    auto Got = detail::returnLegacyFrame(Returned, Memory, Width, Discard, IRET,
                                         Options);
    State.NMIBlocked = Returned.NMIBlocked;
    if (!Got)
      return Got.takeError();
    R.InstructionAttempted = true;
    if (Accept(*Got)) {
      State = Returned;
      State.SSBlocked = State.STIBlocked = false;
      auto Controls = Memory.controls();
      Controls.RFlags = State.EFlags;
      if (auto Error = Memory.bindControls(Controls))
        return std::move(Error);
      R.InstructionCompleted = true;
      const uint8_t Data =
          State.Debug ? x86CompleteDataBreakpoints(*State.Debug,
                                                   Observation.Matches, false)
                      : 0;
      const bool Step = x86InstructionDebugStep(SingleStep, DebugCTL, true);
      if (Step || Data)
        return EnterDebug(Step, true, Data);
      return R;
    }
    if (R.BackingFault)
      return R;
  }
  auto Next = State.Core;
  auto NextTR = State.TR;
  auto NextControls = Memory.controls();
  auto NextCR2 = State.CR2;
  uint32_t Flags = State.EFlags;
  bool SS = false, STI = false, RepeatContinue = false;
  bool TakenBranch = false;
  auto NextIP = uint32_t(State.EIP + Instruction.size());
  if (!R.Exception && ControlRegister) {
    auto Staged = State;
    auto Got = detail::executeLegacyControlRegister(Staged, NextControls,
                                                    Memory, *ControlRegister,
                                                    *Options.ControlRegisters);
    if (!Got)
      return Got.takeError();
    if (Accept(*Got)) {
      if (State.Debug &&
          !x86InstructionDebugValid(State.Debug->DR6, State.Debug->DR7,
                                    State.Debug->DebugCTL, *Options.Debug,
                                    NextControls.CR4, State.Debug->PendingData))
        return invalid("MOV-CR requests unbound debug controls");
      Next = Staged.Core;
      NextCR2 = Staged.CR2;
    }
  } else if (!R.Exception && DebugRegister) {
    auto Bank = *State.Debug;
    const auto Result = x86AccessDebugRegister(
        Bank, Next.GPR[DebugRegister->Access.General], DebugRegister->Access,
        Memory.controls().CR4, State.Core.CPL, *Options.Debug);
    using Status = X86DebugRegisterResult;
    if (Result == Status::UnsupportedControl)
      return invalid("MOV-DR requests unbound debug controls");
    if (Result == Status::GeneralDetect) {
      if (Options.GeneralDetectFrame != X86GeneralDetectFrame::FaultResume &&
          Options.GeneralDetectFrame != X86GeneralDetectFrame::ClearResume)
        return invalid("general detect requires a saved-RF binding");
      x86PublishInstructionDebug(*State.Debug, false);
      State.Debug->DR6 |= 0x2000;
      State.SSBlocked = State.STIBlocked = false;
      return Enter(
          {State.EIP, 0, 0, 1,
           Options.GeneralDetectFrame == X86GeneralDetectFrame::FaultResume
               ? X86LegacyEventKind::Fault
               : X86LegacyEventKind::DebugRegisterFault},
          false);
    }
    if (Result == Status::Completed)
      *State.Debug = Bank;
    else
      R.Exception = X86MemoryException{
          uint8_t(Result == Status::UndefinedOpcode ? 6 : 13), 0, {}};
  } else if (!R.Exception && Mpx && Mpx->Id != X86_INS_NOP) {
    auto Got =
        detail::executeLegacyMpx(State, Memory, Instruction, *NextMpx, Options);
    if (State.PendingMpx)
      State.PendingMpx->DataBreakpoints = Observation.Matches;
    if (!Got)
      return Got.takeError();
    Accept(*Got);
    // #BR status and BNDLDX's unknown destination belong to the instruction,
    // before IDT delivery. Host backing suspension publishes neither effect.
    if (State.Mpx && Got->Exception)
      State.Mpx = *NextMpx;
  } else if (!R.Exception && FastSystem) {
    R.Exception = detail::executeLegacyFastSystem(Next, Flags, NextIP,
                                                  State.Sysenter, *FastSystem);
    TakenBranch = !R.Exception;
  } else if (!R.Exception && Far) {
    auto Got = detail::executeLegacyFarTransfer(State, Memory, *Far, NextIP,
                                                Instruction, Options);
    if (!Got)
      return Got.takeError();
    if (Got->Committed || detail::legacyTaskInstructionPending(State))
      return FinishTask(*Got);
    if (Accept(*Got)) {
      Next = State.Core;
      NextIP = State.EIP;
      TakenBranch = true;
    }
  } else if (!R.Exception && Segment) {
    auto Got = executeX86LegacySegmentInstruction(Next, Memory, Instruction,
                                                  Options.Segments);
    if (!Got)
      return Got.takeError();
    Accept(*Got);
    SS = Got->LoadsSSWithInhibition && !State.SSBlocked;
  } else if (!R.Exception && Port) {
    auto Staged = State;
    auto Got =
        detail::executeLegacyPortIO(Staged, Memory, *Port, NextIP, Options);
    if (!Got)
      return Got.takeError();
    R.DeviceTransferCompleted = Got->DeviceTransferCompleted;
    R.PortInputBytesWritten = Got->InputBytesWritten;
    if (Got->DeviceTransferCompleted)
      Observation.port(Got->Port, Got->TransferBytes);
    if (Accept(Got->Access)) {
      if (Got->DeviceUnavailable) {
        R.Status = X86LegacyExecutionStatus::DeviceUnavailable;
        return R;
      }
      Next = Staged.Core;
      NextIP = Staged.EIP;
      RepeatContinue = Got->RepeatContinue;
    }
  } else if (!R.Exception && String) {
    auto Staged = State;
    auto Got =
        detail::executeLegacyString(Staged, Memory, *String, NextIP, Options);
    if (!Got)
      return Got.takeError();
    if (Accept(*Got)) {
      Next = Staged.Core;
      NextIP = Staged.EIP;
      Flags = Staged.EFlags;
      RepeatContinue = Got->RepeatContinue;
    }
  } else if (!R.Exception && Scalar && Scalar->stackFrame()) {
    auto Got = detail::executeLegacyStack(State, Memory, *Scalar, Instruction,
                                          NextIP, Options);
    if (State.PendingStack)
      State.PendingStack->DataBreakpoints = Observation.Matches;
    if (!Got)
      return Got.takeError();
    if (Accept(*Got)) {
      Next = State.Core;
      NextIP = State.EIP;
    }
  } else if (!R.Exception && Scalar) {
    auto Staged = State;
    auto Got =
        detail::executeLegacyScalar(Staged, Memory, *Scalar, NextIP, Options);
    if (!Got)
      return Got.takeError();
    if (Accept(*Got)) {
      Next = Staged.Core;
      NextIP = Staged.EIP;
      Flags = Staged.EFlags;
      TakenBranch = Got->TakenBranch;
      NextTR = Staged.TR;
      if (Got->CR0)
        NextControls.CR0 = *Got->CR0;
    } else if (NextMpx && Got->Exception && Got->Exception->Vector == 5) {
      // This is an architectural effect of #BR, before attempting IDT entry.
      // Unlike GPR/EIP/FLAGS staging, it survives delivery faults and #DF.
      NextMpx->completeLegacyBoundsFault();
      if (State.Mpx)
        State.Mpx = *NextMpx;
    }
  } else if (!R.Exception) {
    const auto Opcode = Body.front();
    if (HLT) {
      if (LockedHLT || State.Core.CPL)
        R.Exception = X86MemoryException{uint8_t(LockedHLT ? 6 : 13), 0, {}};
    } else if (Opcode == 0xfa || Opcode == 0xfb) {
      const bool Set = Opcode == 0xfb;
      if (State.Core.CPL <= ((Flags >> 12) & 3)) {
        STI = Set && !(Flags & 0x200);
        Flags = Set ? Flags | 0x200 : Flags & ~0x200u;
      } else if (State.Core.CPL == 3 && (Memory.controls().CR4 & 2) &&
                 (!Set || !(Flags & 0x100000))) {
        Flags = Set ? Flags | 0x80000 : Flags & ~0x80000u;
      } else {
        R.Exception = X86MemoryException{13, 0, {}};
      }
    } else if (INT || Opcode == 0xcc || Opcode == 0xf1 ||
               (Opcode == 0xce && (Flags & 0x800))) {
      if (Opcode == 0xf1 && State.SSBlocked && !State.Debug)
        return invalid(
            "ICEBP under MOV-SS inhibition requires deferred debug state");
      const uint8_t Vector = INT              ? Instruction[1]
                             : Opcode == 0xcc ? 3
                             : Opcode == 0xce ? 4
                                              : 1;
      if (Opcode == 0xf1) {
        // ICEBP completes before its debug trap. A failed #DB entry keeps
        // the fallthrough and must not restore pre-instruction RF or STI.
        State.EIP = NextIP;
        State.EFlags = Flags & ~0x10000u;
        const uint8_t Data =
            State.Debug ? x86CompleteDataBreakpoints(*State.Debug,
                                                     Observation.Matches, false)
                        : 0;
        return EnterDebug(false, true, Data, true);
      }
      return Enter({NextIP, 0, 0, Vector, X86LegacyEventKind::Software}, true);
    } else if (UD2) {
      R.Exception = X86MemoryException{6, 0, {}};
    }
  }
  R.InstructionAttempted = !Resuming;
  if (R.Exception)
    return Enter({State.EIP, R.Exception->ErrorCode,
                  uint32_t(R.Exception->CR2.value_or(0)), R.Exception->Vector,
                  X86LegacyEventKind::Fault},
                 false);
  if (R.BackingFault)
    return R;
  NextControls.RFlags = RepeatContinue ? Flags | 0x10000u : Flags & ~0x10000u;
  if (auto Error = Memory.bindControls(NextControls))
    return std::move(Error);
  State.Core = Next;
  State.TR = NextTR;
  State.CR2 = NextCR2;
  State.EIP = NextIP;
  State.EFlags = NextControls.RFlags;
  if (State.Mpx && NextMpx)
    State.Mpx = std::move(NextMpx);
  State.SSBlocked = SS;
  State.STIBlocked = STI;
  State.Halted = HLT;
  R.InstructionCompleted = !RepeatContinue;
  if (HLT)
    R.Status = X86LegacyExecutionStatus::Halted;
  else if (RepeatContinue)
    R.Status = X86LegacyExecutionStatus::RepeatContinue;
  bool RepeatGroupContinues = false;
  if (RepeatContinue && String && Options.Debug &&
      x86DebugWatchpoints(*Options.Debug)) {
    const uint32_t Mask = String->AddressBytes == 2 ? UINT16_MAX : UINT32_MAX;
    const auto &GPR = State.Core.GPR;
    RepeatGroupContinues = x86RepeatDebugDefers(
        *Options.RepeatDebug,
        {State.EIP, Flags, GPR[1] & Mask, GPR[6] & Mask, GPR[7] & Mask,
         uint8_t(String->AddressBytes), uint8_t(String->Element.Bytes),
         String->SourceIndex, String->DestinationIndex,
         String->Element.Operation == detail::X86LegacyScalarOperation::Compare,
         DebugCTL});
  }
  // Sample TF at instruction entry, before POPF/IRET can change it. A MOV/POP
  // SS that establishes a shadow suppresses this trap, rather than postponing
  // it. A REP iteration traps at its retained PC with RF set until completion.
  const uint8_t Data =
      State.Debug
          ? x86CompleteDataBreakpoints(*State.Debug, Observation.Matches, SS,
                                       RepeatGroupContinues)
          : 0;
  const bool Step =
      x86InstructionDebugStep(SingleStep, DebugCTL, TakenBranch) && !SS;
  if (Step || Data)
    return EnterDebug(Step, !RepeatContinue, Data);
  return R;
}

} // namespace neverd::translate
