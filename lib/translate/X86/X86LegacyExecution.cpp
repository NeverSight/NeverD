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
#include "X86LegacySegmentEncoding.h"

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
  auto Body = Instruction;
  unsigned Width = Code32 ? 4 : 2;
  if (Body.front() == 0x66) {
    Width = 6 - Width;
    Body = Body.drop_front();
  }
  const bool Immediate =
      Body.size() == Width + 1 && Body.front() >= 0xb8 && Body.front() <= 0xbf;
  const bool Single = Instruction.size() == 1 &&
                      (Instruction[0] == 0x90 || Instruction[0] == 0xfa ||
                       Instruction[0] == 0xfb || Instruction[0] == 0xcc ||
                       Instruction[0] == 0xce || Instruction[0] == 0xf1);
  const bool INT = Instruction.size() == 2 && Instruction[0] == 0xcd;
  const bool UD2 = Instruction.size() == 2 && Instruction[0] == 0x0f &&
                   Instruction[1] == 0x0b;
  if (!Segment && !Immediate && !Single && !INT && !UD2)
    return invalid("unsupported or malformed legacy step instruction");
  if (State.EFlags & 0x100)
    return invalid("legacy TF stepping requires a debug-event producer");
  if (Segment &&
      (Options.Segments.Profile !=
           X86LegacySegmentProfile::WordStackOffsetFirst ||
       (Options.Segments.AlignmentPriority &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::BeforePaging &&
        *Options.Segments.AlignmentPriority !=
            X86AlignmentPriority::AfterPaging) ||
       (State.Core.CPL == 3 && (Memory.controls().CR0 & 0x40000) &&
        (State.EFlags & 0x40000) && !Options.Segments.AlignmentPriority)))
    return invalid("legacy segment step requires an operand profile");
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
  auto Next = State.Core;
  uint32_t Flags = State.EFlags;
  bool SS = false, STI = false;
  const auto NextIP = uint32_t(State.EIP + Instruction.size());
  if (!R.Exception && Segment) {
    auto Got = executeX86LegacySegmentInstruction(Next, Memory, Instruction,
                                                  Options.Segments);
    if (!Got)
      return Got.takeError();
    Accept(*Got);
    SS = Got->LoadsSSWithInhibition && !State.SSBlocked;
  } else if (!R.Exception && Immediate) {
    uint32_t Value = 0;
    for (unsigned I = 0; I != Width; ++I)
      Value |= uint32_t(Body[I + 1]) << (I * 8);
    auto &Register = Next.GPR[Body.front() - 0xb8];
    Register = Width == 4 ? Value : (Register & 0xffff0000) | Value;
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
  State.EFlags = Flags & ~0x10000u;
  State.SSBlocked = SS;
  State.STIBlocked = STI;
  auto Controls = Memory.controls();
  Controls.RFlags = State.EFlags;
  if (auto Error = Memory.bindControls(Controls))
    return std::move(Error);
  R.InstructionCompleted = true;
  return R;
}

} // namespace neverd::translate
