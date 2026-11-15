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
#include "X86LegacyTransferJournal.h"

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
  bool IRET, RETF, INT, UD2, HLT = false, LockedHLT = false, MSR = false;
};

llvm::Expected<LegacyInstruction>
decodeInstruction(llvm::ArrayRef<uint8_t> Instruction, bool Code32,
                  bool Non64Mode = false) {
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
      decodeX86MpxEncoding(Instruction, Code32 ? X86MpxCodeMode::Legacy32
                                               : X86MpxCodeMode::Legacy16),
      Instruction,
      Code32 ? 4u : 2u,
      false,
      false,
      false,
      false};
  if (I.Mpx && I.Mpx->Size != Instruction.size())
    return invalid("MPX binding contains trailing bytes");
  detail::X86LegacyEncoding Control(Instruction, Code32);
  I.Body = Control.body();
  I.Width = Control.OperandBytes;
  const bool Unlocked = !Control.Group1Prefix;
  I.MSR = Non64Mode && I.Body.size() == 2 && I.Body[0] == 0x0f &&
          (I.Body[1] == 0x30 || I.Body[1] == 0x32);
  const bool Single =
      Unlocked && I.Body.size() == 1 &&
      (I.Body[0] == 0x90 || I.Body[0] == 0xfa || I.Body[0] == 0xfb ||
       I.Body[0] == 0xcc || I.Body[0] == 0xce || I.Body[0] == 0xf1);
  I.INT = Unlocked && I.Body.size() == 2 && I.Body[0] == 0xcd;
  I.UD2 =
      Unlocked && I.Body.size() == 2 && I.Body[0] == 0x0f && I.Body[1] == 0x0b;
  I.IRET = Unlocked && I.Body.size() == 1 && I.Body[0] == 0xcf;
  I.RETF = Unlocked && ((I.Body.size() == 1 && I.Body[0] == 0xcb) ||
                        (I.Body.size() == 3 && I.Body[0] == 0xca));
  if (Instruction.back() == 0xf4) {
    detail::X86LegacyEncoding HaltEncoding(Instruction, Code32);
    I.HLT = HaltEncoding.byte() == 0xf4 && HaltEncoding.done();
    I.LockedHLT = HaltEncoding.Group1Prefix == 0xf0;
  }
  if (!I.Segment && !I.Scalar && !I.FastSystem && !I.String && !I.Port &&
      !I.Far && !Single && !I.INT && !I.UD2 && !I.IRET && !I.RETF &&
      !I.DebugRegister && !I.ControlRegister && !I.Mpx && !I.HLT && !I.MSR)
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
                                         const X86LegacyExecutionOptions &O,
                                         bool Non64Mode) {
  if (!Non64Mode && O.Non64Mode)
    return invalid(
        "mixed-mode binding requires the deferred instruction owner");
  if (auto Error = detail::validateLegacyControlContinuation(S, M, O))
    return Error;
  if ((S.EFlags & 0x20000) && O.Virtual8086 != X86LegacyVM86Profile::IntelVME)
    return invalid("VM86 execution requires a bound Intel VME profile");
  if (!O.EventDelivery.MaxAttempts)
    return invalid("event delivery requires a positive work budget");
  if (S.Halted && (S.Core.CPL || S.STIBlocked || S.SSBlocked ||
                   S.PendingStack || S.PendingMpx || S.PendingTask ||
                   S.PendingTransfer || S.PendingControl || S.PendingEvent))
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
    if (auto Error = validateX86LegacySegmentState(S.Core, Controls, Non64Mode))
      return Error;
  }
  if (!(S.EFlags & 2) || (S.EFlags & ~uint32_t{0x3f7fd7}) ||
      (Controls.CR4 & ((uint64_t{1} << 23) | (uint64_t{1} << 32))))
    return invalid("legacy execution requires ordinary protected/VM86 flags "
                   "and no CET/FRED");
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

static bool retainsLegacyInstruction(const X86LegacyExecutionState &State) {
  return State.PendingStack || State.PendingMpx || State.PendingTransfer ||
         State.PendingControl || detail::legacyTaskInstructionPending(State);
}

bool detail::legacyInstructionPending(const X86LegacyExecutionState &State) {
  return !State.PendingEvent && retainsLegacyInstruction(State);
}

static llvm::Expected<X86LegacyExecutionResult>
executeLegacyInstruction(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                         llvm::ArrayRef<uint8_t> Instruction,
                         const X86LegacyExecutionOptions &Options,
                         bool Non64Mode) {
  if (Non64Mode != bool(Options.Non64Mode))
    return invalid("non-64-bit execution requires its full-width mode binding");
  if (auto Error = detail::validateLegacyControlContinuation(
          State, Memory, Options, Instruction))
    return std::move(Error);
  if (detail::legacyTaskModeUnavailable(State) &&
      Options.Virtual8086 != X86LegacyVM86Profile::IntelVME) {
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
    if (auto Error =
            detail::admitLegacyExecution(State, Memory, Options, Non64Mode))
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
  if (auto Error = detail::validateLegacyTransferContinuation(
          State, Memory, Instruction, Options))
    return std::move(Error);
  const bool ResumingStack = State.PendingStack.has_value();
  const bool ResumingTask = detail::legacyTaskInstructionPending(State);
  const bool Resuming = detail::legacyInstructionPending(State);
  const bool Attempted = !(Resuming && State.PendingInstructionAttempted);
  const bool Code32 = ResumingTask
                          ? bool(detail::legacyTaskInstructionKey(State) >> 32)
                          : bool(State.Core.Segments[1].Attributes & 0x4000);
  auto Decoded = decodeInstruction(Instruction, Code32, Non64Mode);
  if (!Decoded)
    return Decoded.takeError();
  auto &[DebugRegister, ControlRegister, Segment, Scalar, FastSystem, String,
         Port, Far, Mpx, Body, Width, IRET, RETF, INT, UD2, HLT, LockedHLT,
         MSR] = *Decoded;
  if (Non64Mode) {
    using Op = detail::X86LegacyScalarOperation;
    if ((State.EFlags & 0x20000) || State.PendingTask || State.PendingEvent ||
        State.Mpx || Options.Tasks || Segment || DebugRegister || FastSystem ||
        Port || String || Mpx || IRET || RETF ||
        (Far && (Far->Call || Far->Memory)) ||
        (Scalar && (Scalar->DescriptorQuery || Scalar->SystemSelector ||
                    Scalar->Operation == Op::LoadMachineStatus ||
                    Scalar->Operation == Op::ClearTaskSwitch ||
                    Scalar->Operation == Op::Bound)))
      return invalid("instruction requires a separate mixed-mode state owner");
    const auto &F = Memory.features();
    const uint64_t AllowedEFER = uint64_t(Options.Non64Mode->EFERSystemCall) |
                                 (uint64_t(F.LongMode) << 8) |
                                 (uint64_t(F.LongMode) << 10) |
                                 (uint64_t(F.NX) << 11);
    if (MSR && ((Memory.controls().EFER & ~AllowedEFER) ||
                State.Core.GPR[1] != 0xc0000080))
      return invalid("MSR access requires a bound Intel EFER bank");
  }
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
  const bool TaskReturn =
      IRET && !(EntryFlags & 0x20000) && (EntryFlags & 0x4000);
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
  if (auto Error =
          detail::admitLegacyExecution(State, Memory, Options, Non64Mode))
    return std::move(Error);
  X86LegacyExecutionResult R;
  X86WatchpointObservation Observation(
      Memory, State.Debug ? &*State.Debug : nullptr,
      Options.Debug.value_or(X86InstructionDebugProfile::InstructionStep), 0,
      Options.Non64Mode ? &Options.Non64Mode->DebugAddresses : nullptr);
  if (ResumingStack)
    Observation.Matches = State.PendingStack->DataBreakpoints;
  if (State.PendingMpx)
    Observation.Matches = State.PendingMpx->DataBreakpoints;
  if (State.PendingTransfer)
    Observation.Matches = detail::legacyTransferWatchpoints(State);
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
    if (Non64Mode) {
      R.RequestedEvent = Event;
      R.InstructionAttempted = Attempted;
      R.InstructionCompleted =
          Completed && Event.Kind == X86LegacyEventKind::Trap;
      return R;
    }
    auto Delivered = deliverX86LegacyEvent(State, Memory, Event, Options);
    if (!Delivered)
      return Delivered.takeError();
    Delivered->DeviceTransferCompleted = R.DeviceTransferCompleted;
    Delivered->PortInputBytesWritten = R.PortInputBytesWritten;
    Delivered->InstructionAttempted = Attempted;
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
  const auto ExecuteBreakpoints = [&](uint32_t PC, bool SS) {
    return Options.Non64Mode
               ? x86ExecutionBreakpoints(Options.Non64Mode->DebugAddresses,
                                         State.Debug->DR7, uint64_t(PC),
                                         State.EFlags, SS)
               : x86ExecutionBreakpoints(State.Debug->DR, State.Debug->DR7, PC,
                                         State.EFlags, SS);
  };
  const auto EnterDebug =
      [&](bool FromSingleStep, bool Completed, uint8_t Data = 0,
          bool Icebp = false) -> llvm::Expected<X86LegacyExecutionResult> {
    // Publish the debug cause before IDT entry: it survives a failed #DB
    // delivery and a backing stop. Software INT 1 does not update DR6.
    if (State.Debug) {
      const auto Matches =
          Data | ((FromSingleStep || Data) && !Icebp
                      ? ExecuteBreakpoints(
                            uint32_t((State.Core.Segments[1].Attributes & 0x2000
                                          ? 0
                                          : State.Core.Segments[1].Base) +
                                     State.EIP),
                            false)
                      : 0);
      x86PublishInstructionDebug(*State.Debug, FromSingleStep, Matches);
    }
    State.SSBlocked = State.STIBlocked = false;
    return Enter({State.EIP, 0, 0, 1, X86LegacyEventKind::Trap}, Completed);
  };
  const auto FinishTask = [&](const detail::X86LegacyTaskResult &Got)
      -> llvm::Expected<X86LegacyExecutionResult> {
    Accept(Got);
    R.InstructionAttempted = Attempted;
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
    const auto Matches = ExecuteBreakpoints(
        uint32_t(State.Core.Segments[1].Base + State.EIP), State.SSBlocked);
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
                                         Instruction, Options);
    State.NMIBlocked = Returned.NMIBlocked;
    State.PendingTransfer = Returned.PendingTransfer;
    detail::retainLegacyTransferWatchpoints(State, Observation.Matches);
    if (!Got)
      return Got.takeError();
    R.InstructionAttempted = Attempted;
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
    auto Got = detail::executeLegacyControlRegister(
        Staged, NextControls, Memory, *ControlRegister, Instruction, Options);
    State.PendingControl = Staged.PendingControl;
    if (!Got)
      return Got.takeError();
    if (Accept(*Got)) {
      Next = Staged.Core;
      NextCR2 = Staged.CR2;
      R.CR2Written = ControlRegister->Write && ControlRegister->Control == 2;
    }
  } else if (!R.Exception && MSR) {
    detail::X86LegacyEncoding Encoding(Instruction, Code32);
    if (Encoding.Group1Prefix == 0xf0 || State.Core.CPL)
      R.Exception = X86MemoryException{
          uint8_t(Encoding.Group1Prefix == 0xf0 ? 6 : 13), 0, {}};
    else if (Body[1] == 0x32) {
      Next.GPR[0] = NextControls.EFER;
      Next.GPR[2] = NextControls.EFER >> 32;
    } else {
      const uint64_t Value =
          uint64_t(Next.GPR[0]) | (uint64_t(Next.GPR[2]) << 32);
      const auto &F = Memory.features();
      const uint64_t Allowed = uint64_t(Options.Non64Mode->EFERSystemCall) |
                               (uint64_t(F.LongMode) << 8) | 0x400 |
                               (uint64_t(F.NX) << 11);
      if ((Value & ~Allowed) || ((NextControls.CR0 & 0x80000000) &&
                                 ((Value ^ NextControls.EFER) & 0x100)))
        R.Exception = X86MemoryException{13, 0, {}};
      else
        NextControls.EFER =
            (Value & ~uint64_t(0x400)) | (NextControls.EFER & 0x400);
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
    auto Got = detail::executeLegacyMpx(State, Memory, Instruction, Code32,
                                        *NextMpx, Options);
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
    detail::retainLegacyTransferWatchpoints(State, Observation.Matches);
    if (!Got)
      return Got.takeError();
    if (Got->Committed || detail::legacyTaskInstructionPending(State))
      return FinishTask(*Got);
    if (Accept(*Got)) {
      Next = State.Core;
      NextIP = State.EIP;
      TakenBranch = true;
      R.CSWritten = true;
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
      } else if (State.Core.CPL == 3 &&
                 (Memory.controls().CR4 & (Flags & 0x20000 ? 1 : 2)) &&
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
      const uint8_t Vector = INT              ? Body[1]
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
      return Enter({NextIP, 0, 0, Vector, X86LegacyEventKind::Software, INT},
                   true);
    } else if (UD2) {
      R.Exception = X86MemoryException{6, 0, {}};
    }
  }
  R.InstructionAttempted = Attempted;
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

static llvm::Expected<X86LegacyExecutionResult> executeInstructionBoundary(
    X86LegacyExecutionState &State, X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Instruction,
    const X86LegacyExecutionOptions &Options, bool Non64Mode) {
  const bool Consumed =
      retainsLegacyInstruction(State) && State.PendingInstructionAttempted;
  auto Result =
      executeLegacyInstruction(State, Memory, Instruction, Options, Non64Mode);
  // Owner Error may retain real progress without reporting a new attempt.
  // Entry refusal (including conflicting journals) preserves a consumed
  // receipt. Only an accepted result can transfer ownership to an event.
  // Completion or event ownership retires it. It must not be derived from
  // the source lastResult.
  State.PendingInstructionAttempted =
      retainsLegacyInstruction(State) && (!Result || !State.PendingEvent) &&
      (Consumed || (Result && Result->InstructionAttempted));
  return Result;
}

llvm::Expected<X86LegacyExecutionResult>
executeX86LegacyInstruction(X86LegacyExecutionState &State,
                            X86PagedMemory &Memory,
                            llvm::ArrayRef<uint8_t> Instruction,
                            const X86LegacyExecutionOptions &Options) {
  return executeInstructionBoundary(State, Memory, Instruction, Options, false);
}

llvm::Expected<X86LegacyExecutionResult>
executeX86Non64Instruction(X86LegacyExecutionState &State,
                           X86PagedMemory &Memory,
                           llvm::ArrayRef<uint8_t> Instruction,
                           const X86LegacyExecutionOptions &Options) {
  return executeInstructionBoundary(State, Memory, Instruction, Options, true);
}

} // namespace neverd::translate
