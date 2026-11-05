//===- X86LegacyExecution.cpp - Authenticated protected-mode steps --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Connect original-byte fetch, segment transitions and persistent IDT state.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LegacyExecution.h"

#include "X86LegacyExecutionInternal.h"
#include "X86LegacyFastSystem.h"
#include "X86LegacyScalar.h"
#include "X86LegacySegmentEncoding.h"
#include "X86LegacyString.h"

#include "neverd/ir/X86/X86ExceptionDelivery.h"

#include "llvm/Support/Errc.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

} // namespace

llvm::Error detail::admitLegacyExecution(const X86LegacyExecutionState &S,
                                         X86PagedMemory &M,
                                         const X86LegacyExecutionOptions &O) {
  auto Controls = M.controls();
  Controls.RFlags = S.EFlags;
  if (auto Error = validateX86LegacySegmentState(S.Core, Controls))
    return Error;
  if (!(S.EFlags & 2) || (S.EFlags & ~uint32_t{0x3d7fd7}) ||
      (Controls.CR4 & ((uint64_t{1} << 23) | (uint64_t{1} << 32))))
    return invalid(
        "legacy execution requires ordinary protected flags and no CET/FRED");
  const auto &CS = S.Core.Segments[1];
  const unsigned DPL = (CS.Attributes >> 5) & 3;
  if (DPL > S.Core.CPL || (!(CS.Attributes & 4) && DPL != S.Core.CPL))
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
  if (Instruction.empty() || Instruction.size() > 15)
    return invalid("invalid legacy instruction length");
  const bool Code32 = State.Core.Segments[1].Attributes & 0x4000;
  const auto Segment =
      detail::decodeLegacySegmentInstruction(Instruction, Code32);
  const auto Scalar =
      detail::decodeLegacyScalarInstruction(Instruction, Code32);
  const auto FastSystem = detail::decodeLegacyFastSystem(Instruction, Code32);
  const auto String = detail::decodeLegacyString(Instruction, Code32);
  auto Body = Instruction;
  unsigned Width = Code32 ? 4 : 2;
  if (Body.front() == 0x66) {
    Width = 6 - Width;
    Body = Body.drop_front();
  }
  const bool Single = Instruction.size() == 1 &&
                      (Instruction[0] == 0x90 || Instruction[0] == 0xfa ||
                       Instruction[0] == 0xfb || Instruction[0] == 0xcc ||
                       Instruction[0] == 0xce || Instruction[0] == 0xf1);
  const bool INT = Instruction.size() == 2 && Instruction[0] == 0xcd;
  const bool UD2 = Instruction.size() == 2 && Instruction[0] == 0x0f &&
                   Instruction[1] == 0x0b;
  const bool IRET = Body.size() == 1 && Body[0] == 0xcf;
  const bool RETF = (Body.size() == 1 && Body[0] == 0xcb) ||
                    (Body.size() == 3 && Body[0] == 0xca);
  if (!Segment && !Scalar && !FastSystem && !String && !Single && !INT &&
      !UD2 && !IRET && !RETF)
    return invalid("unsupported or malformed legacy step instruction");
  if (String &&
      Options.Strings != X86LegacyStringProfile::ElementwiseDestinationFirst)
    return invalid(
        "legacy string step requires a bound element-progress profile");
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
  std::optional<X86MpxContext> BoundMpx;
  if (Scalar && Scalar->Operation == detail::X86LegacyScalarOperation::Bound) {
    BoundMpx = State.Mpx.value_or(X86MpxContext{});
    auto Configuration = BoundMpx->configuration();
    if (Configuration.InTransaction == true)
      return invalid("BOUND in a transaction requires an abort continuation");
    Configuration.OSXSAVE = bool(Memory.controls().CR4 & (uint64_t{1} << 18));
    Configuration.PrivilegeLevel = State.Core.CPL;
    if (!BoundMpx->configure(Configuration))
      return invalid("BOUND requires a valid MPX configuration");
    const auto Status = BoundMpx->operationStatus(X86MpxMode::Bits32, 0);
    if (Status != X86MpxResult::Completed && Status != X86MpxResult::Disabled)
      return invalid("BOUND requires known nontransactional MPX enable state");
  }
  if (State.EFlags & 0x100)
    return invalid("legacy TF stepping requires a debug-event producer");
  if (IRET && (State.EFlags & 0x4000))
    return invalid("legacy nested-task IRET requires task-switch state");
  if ((IRET || RETF) &&
      (Options.Returns != X86LegacyReturnProfile::OrderedFrame ||
       (Options.Segments.AlignmentPriority &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::BeforePaging &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::AfterPaging) ||
       (State.Core.CPL == 3 && (Memory.controls().CR0 & 0x40000) &&
        (State.EFlags & 0x40000) && !Options.Segments.AlignmentPriority)))
    return invalid("legacy return requires a bound return/alignment profile");
  if ((Segment || Scalar || String) &&
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
    auto Delivered = deliverX86LegacyEvent(State, Memory, Event, Options);
    if (!Delivered)
      return Delivered.takeError();
    Delivered->InstructionAttempted = true;
    Delivered->InstructionCompleted =
        Completed &&
        (Event.Kind == X86LegacyEventKind::Trap ||
         (Delivered->Status == X86LegacyExecutionStatus::EventDelivered &&
          !Delivered->Exception));
    if (!Delivered->Exception)
      Delivered->Exception = R.Exception;
    Delivered->SelfModifications.insert(Delivered->SelfModifications.begin(),
                                        R.SelfModifications.begin(),
                                        R.SelfModifications.end());
    return Delivered;
  };
  if (x86PendingVirtualInterrupt(State.Core.CPL, Memory.controls().CR4,
                                 State.EFlags))
    R.Exception = X86MemoryException{13, 0, {}};
  for (unsigned I = 0; !R.Exception && I != Instruction.size(); ++I) {
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
      return R;
    }
    if (R.BackingFault)
      return R;
  }
  auto Next = State.Core;
  uint32_t Flags = State.EFlags;
  bool SS = false, STI = false, RepeatContinue = false;
  auto NextIP = uint32_t(State.EIP + Instruction.size());
  if (!R.Exception && FastSystem) {
    R.Exception = detail::executeLegacyFastSystem(Next, Flags, NextIP,
                                                  State.Sysenter, *FastSystem);
  } else if (!R.Exception && Segment) {
    auto Got = executeX86LegacySegmentInstruction(Next, Memory, Instruction,
                                                  Options.Segments);
    if (!Got)
      return Got.takeError();
    Accept(*Got);
    SS = Got->LoadsSSWithInhibition && !State.SSBlocked;
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
    } else if (BoundMpx && Got->Exception && Got->Exception->Vector == 5) {
      // This is an architectural effect of #BR, before attempting IDT entry.
      // Unlike GPR/EIP/FLAGS staging, it survives delivery faults and #DF.
      BoundMpx->completeLegacyBoundsFault();
      if (State.Mpx)
        State.Mpx = std::move(BoundMpx);
    }
  } else if (!R.Exception) {
    const auto Opcode = Body.front();
    if (Opcode == 0xfa || Opcode == 0xfb) {
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
      if (Opcode == 0xf1 && State.SSBlocked)
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
        State.STIBlocked = false;
      }
      return Enter({NextIP, 0, 0, Vector,
                    Opcode == 0xf1 ? X86LegacyEventKind::Trap
                                   : X86LegacyEventKind::Software},
                   true);
    } else if (UD2) {
      R.Exception = X86MemoryException{6, 0, {}};
    }
  }
  R.InstructionAttempted = true;
  if (R.Exception)
    return Enter({State.EIP, R.Exception->ErrorCode,
                  uint32_t(R.Exception->CR2.value_or(0)), R.Exception->Vector,
                  X86LegacyEventKind::Fault},
                 false);
  if (R.BackingFault)
    return R;
  State.Core = Next;
  State.EIP = NextIP;
  State.EFlags = RepeatContinue ? Flags | 0x10000u : Flags & ~0x10000u;
  State.SSBlocked = SS;
  State.STIBlocked = STI;
  auto Controls = Memory.controls();
  Controls.RFlags = State.EFlags;
  if (auto Error = Memory.bindControls(Controls))
    return std::move(Error);
  R.InstructionCompleted = !RepeatContinue;
  if (RepeatContinue)
    R.Status = X86LegacyExecutionStatus::RepeatContinue;
  return R;
}

} // namespace neverd::translate
