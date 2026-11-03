//===- X86LegacyEventDelivery.cpp - Protected 16/32-bit IDT entry
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Faultable descriptor, TSS and event-frame transitions over paged RAM.
//===----------------------------------------------------------------------===//

#include "X86LegacyExecutionInternal.h"

#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/ir/X86/X86SegmentDescriptorRules.h"
#include "neverd/translate/X86/X86LegacyExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace neverd::translate {
namespace {

using Kind = X86LegacyEventKind;

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

bool validEvent(const X86LegacyEvent &E) {
  const auto Member = [&](llvm::ArrayRef<unsigned> Values) {
    return std::find(Values.begin(), Values.end(), E.Vector) != Values.end();
  };
  const bool Valid = E.Kind == Kind::Software || E.Kind == Kind::Interrupt ||
                     (E.Kind == Kind::NMI && E.Vector == 2) ||
                     (E.Kind == Kind::InstructionBreakpoint && E.Vector == 1) ||
                     (E.Kind == Kind::Fault && Member(kX86FaultVectors)) ||
                     (E.Kind == Kind::Trap && Member(kX86TrapVectors)) ||
                     (E.Kind == Kind::Abort && Member(kX86AbortVectors));
  return Valid &&
         (!E.ErrorCode || (x86EventHasErrorCode(E.Kind, E.Vector) &&
                           E.Vector != 8 && E.Vector != 17)) &&
         (!E.FaultAddress || (E.Kind == Kind::Fault && E.Vector == 14));
}

class Entry {
public:
  using Value = uint64_t;
  using Rules = neverd::detail::X86SegmentDescriptorRules<Entry>;

  Entry(X86LegacyExecutionState &State, X86PagedMemory &Memory,
        const X86LegacyEvent &Event, const X86LegacyExecutionOptions &Options)
      : S(State), M(Memory), E(Event), O(Options),
        External(E.Kind != Kind::Software) {}

  static Value k(uint64_t V) { return V; }
  static Value masked(Value V, uint64_t Mask) { return V & Mask; }
  static Value shifted(Value V, unsigned Bits) { return V >> Bits; }
  static Value left(Value V, unsigned Bits) { return V << Bits; }
  static Value bitOr(Value A, Value B) { return A | B; }
  static bool eq(Value A, Value B) { return A == B; }
  static bool ne(Value A, Value B) { return A != B; }
  static bool less(Value A, Value B) { return A < B; }
  static bool both(bool A, bool B) { return A && B; }
  static bool either(bool A, bool B) { return A || B; }
  static Value choose(bool C, Value A, Value B) { return C ? A : B; }

  bool faultIf(bool Bad, uint8_t Vector, uint32_t Error) {
    if (Bad && R.Status == X86PageWalkStatus::Completed) {
      R.Status = X86PageWalkStatus::Exception;
      R.Exception = X86MemoryException{Vector, Error | External, {}};
    }
    return Bad;
  }

  llvm::Expected<X86MemoryAccessResult> run() {
    execute();
    if (Failure)
      return std::move(Failure);
    return std::move(R);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyEvent &E;
  const X86LegacyExecutionOptions &O;
  uint32_t External;
  X86MemoryAccessResult R;
  llvm::Error Failure = llvm::Error::success();

  bool access(uint32_t Address, unsigned Bytes, Value &V, bool Write = false,
              bool Implicit = true, uint8_t CPL = 0) {
    std::array<uint8_t, 8> Buffer{};
    for (unsigned I = 0; I != Bytes; ++I)
      Buffer[I] = V >> (I * 8);
    const X86LinearAccess A{
        Address,   Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        CPL,       Implicit,
        !Implicit, true};
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Bytes);
    auto Got = Write ? M.store(A, Span) : M.load(A, Span);
    if (!Got) {
      Failure = Got.takeError();
      return false;
    }
    R.SelfModifications.insert(R.SelfModifications.end(),
                               Got->SelfModifications.begin(),
                               Got->SelfModifications.end());
    if (Got->Status != X86PageWalkStatus::Completed) {
      R.Status = Got->Status;
      R.Exception = Got->Exception;
      R.BackingFault = Got->BackingFault;
      if (Got->Exception && Got->Exception->CR2)
        S.CR2 = *Got->Exception->CR2;
      return false;
    }
    if (!Write) {
      V = 0;
      for (unsigned I = 0; I != Bytes; ++I)
        V |= Value(Buffer[I]) << (I * 8);
    }
    return true;
  }

  bool descriptor(uint16_t Selector, uint8_t Fault, Value &D,
                  uint32_t &Address) {
    const bool Local = Selector & 4;
    const auto &C = S.Core;
    if (faultIf((Local && C.LDTR.Unusable) ||
                    (Selector & 0xfff8u) + 7 >
                        (Local ? C.LDTR.Limit : C.GDTRLimit),
                Fault, Selector & 0xfffc))
      return false;
    Address = (Local ? C.LDTR.Base : C.GDTRBase) + (Selector & 0xfff8);
    return access(Address, 8, D);
  }

  bool accessed(uint32_t Address, Value D) {
    if (D & (Value{1} << 40))
      return true;
    D |= Value{1} << 40;
    return access(Address, 8, D, true);
  }

  X86LegacySegmentCache cache(uint16_t Selector, Value D) {
    return {uint32_t(Rules::base(*this, D)), uint32_t(Rules::limit(*this, D)),
            Selector, uint16_t(Rules::attributes(*this, D)), false};
  }

  void execute() {
    const uint32_t GateOffset = unsigned(E.Vector) * 8;
    const uint32_t GateError = GateOffset | 2;
    if (faultIf(GateOffset + 7 > S.IDTRLimit, 13, GateError))
      return;
    Value Gate = 0;
    if (!access(S.IDTRBase + GateOffset, 8, Gate))
      return;
    const auto Type = (Gate >> 40) & 31;
    if (faultIf(Type != 5 && Type != 6 && Type != 7 && Type != 14 && Type != 15,
                13, GateError) ||
        faultIf(E.Kind == Kind::Software && ((Gate >> 45) & 3) < S.Core.CPL, 13,
                GateError) ||
        faultIf(!(Gate & (Value{1} << 47)), 11, GateError))
      return;
    if (Type == 5) {
      Failure = invalid("legacy task-gate delivery requires task-switch state");
      return;
    }
    const unsigned Width = Type & 8 ? 4 : 2;
    const uint16_t Selector = Gate >> 16;
    const uint32_t IP =
        uint16_t(Gate) | (Width == 4 ? uint32_t(Gate >> 32) & 0xffff0000 : 0);
    if (faultIf(!(Selector & 0xfffc), 13, 0))
      return;
    Value Code = 0;
    uint32_t CodeAddress = 0;
    if (!descriptor(Selector, 13, Code, CodeAddress))
      return;
    const auto Attributes = uint16_t(Code >> 40);
    const unsigned DPL = (Attributes >> 5) & 3;
    if (faultIf((Attributes & 0x18) != 0x18 || DPL > S.Core.CPL, 13,
                Selector & 0xfffc) ||
        faultIf(!(Attributes & 0x80), 11, Selector & 0xfffc))
      return;
    if (Attributes & 0x2000) {
      Failure = invalid("legacy event target has unsupported reserved L bit");
      return;
    }
    const uint8_t CPL = Attributes & 4 ? S.Core.CPL : DPL;
    const bool Inner = CPL < S.Core.CPL;
    const auto CS = cache((Selector & 0xfffc) | CPL, Code);
    if (!accessed(CodeAddress, Code))
      return;
    auto SS = S.Core.Segments[2];
    uint32_t SP = S.Core.GPR[4];
    if (Inner) {
      const auto &TR = S.TR;
      const bool WideTSS = (TR.Attributes & 0xf) == 11;
      const unsigned Offset = WideTSS ? 4 + CPL * 8 : 2 + CPL * 4;
      const unsigned SPBytes = WideTSS ? 4 : 2;
      if (faultIf(TR.Unusable ||
                      ((TR.Attributes & 0x9f) != 0x8b &&
                       (TR.Attributes & 0x9f) != 0x83) ||
                      Offset + SPBytes + 1 > TR.Limit,
                  10, TR.Selector & 0xfffc))
        return;
      Value NewSS = 0, NewSP = 0;
      if (!access(TR.Base + Offset, SPBytes, NewSP) ||
          !access(TR.Base + Offset + SPBytes, 2, NewSS))
        return;
      if (faultIf(!(NewSS & 0xfffc), 10, 0) ||
          faultIf((NewSS & 3) != CPL, 10, NewSS & 0xfffc))
        return;
      Value Stack = 0;
      uint32_t StackAddress = 0;
      if (!descriptor(NewSS, 10, Stack, StackAddress))
        return;
      Rules::checkData(*this, true, NewSS, Stack, CPL, 10);
      if (R.Status != X86PageWalkStatus::Completed)
        return;
      SS = cache(NewSS, Stack);
      if (auto Error =
              validateX86LegacySegmentCache(X86SegmentRegister::SS, SS)) {
        Failure = std::move(Error);
        return;
      }
      if (!accessed(StackAddress, Stack))
        return;
      // Target stack address size, independently of gate and TSS widths.
      SP = SS.Attributes & 0x4000
               ? uint32_t(NewSP)
               : (S.Core.GPR[4] & 0xffff0000) | uint16_t(NewSP);
    }
    const bool HasError = x86EventHasErrorCode(E.Kind, E.Vector);
    const unsigned Count = (Inner ? 5 : 3) + HasError;
    const uint32_t Mask = SS.Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
    const uint32_t Bottom = (SP - Count * Width) & Mask;
    auto Range = checkX86LegacySegmentAccess(
        SS, {X86SegmentRegister::SS, X86SegmentAccessKind::Write, Bottom,
             Count * Width, O.Segments.FullLimitChoice});
    if (!Range) {
      Failure = Range.takeError();
      return;
    }
    if (faultIf(bool(Range->FaultVector), 12,
                Inner ? SS.Selector & 0xfffc : 0) ||
        faultIf(IP > CS.Limit, 13, 0))
      return;
    const uint32_t Flags = E.Kind == Kind::Software ? S.EFlags & ~0x10000u
                           : E.Kind == Kind::Fault  ? S.EFlags | 0x10000u
                                                    : S.EFlags;
    const uint32_t Values[]{
        S.Core.Segments[2].Selector, S.Core.GPR[4], Flags,
        S.Core.Segments[1].Selector, E.SavedEIP,    E.ErrorCode};
    for (unsigned I = Inner ? 0 : 2; I != (HasError ? 6u : 5u); ++I) {
      SP = (SP & ~Mask) | ((SP - Width) & Mask);
      Value V = Values[I];
      if (!access(SS.Base + (SP & Mask), Width, V, true, false, CPL))
        return;
    }
    S.Core.Segments[1] = CS;
    S.Core.Segments[2] = SS;
    S.Core.CPL = CPL;
    S.Core.GPR[4] = SP;
    S.EIP = IP;
    S.EFlags &= ~(0x34100u | (Type & 1 ? 0 : 0x200u));
  }
};

} // namespace

llvm::Expected<X86LegacyExecutionResult>
deliverX86LegacyEvent(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                      const X86LegacyEvent &Input,
                      const X86LegacyExecutionOptions &Options) {
  if (!validEvent(Input))
    return invalid("invalid legacy event classification or frame data");
  if (Input.Kind == Kind::NMI && State.STIBlocked && !State.SSBlocked &&
      !State.NMIBlocked && !Options.NMIBlockedBySTI)
    return invalid("legacy NMI under STI requires a bound inhibition policy");
  if (auto Error = detail::admitLegacyExecution(State, Memory, Options))
    return std::move(Error);
  X86LegacyExecutionResult R;
  const bool Debug = Input.Vector == 1 &&
                     (Input.Kind == Kind::Fault || Input.Kind == Kind::Trap ||
                      Input.Kind == Kind::InstructionBreakpoint);
  if ((Input.Kind == Kind::Interrupt &&
       (!(State.EFlags & 0x200) || State.STIBlocked || State.SSBlocked)) ||
      (Input.Kind == Kind::NMI &&
       (State.NMIBlocked || State.SSBlocked ||
        (State.STIBlocked && Options.NMIBlockedBySTI.value_or(false)))) ||
      (Debug && State.SSBlocked) ||
      (Input.Kind == Kind::InstructionBreakpoint && (State.EFlags & 0x10000))) {
    R.Status = X86LegacyExecutionStatus::EventDeferred;
    return R;
  }
  R.EventAccepted = true;
  State.STIBlocked = State.SSBlocked = false;
  if (Input.Kind == Kind::NMI)
    State.NMIBlocked = true;
  if (Input.Kind == Kind::Fault && Input.Vector == 14)
    State.CR2 = Input.FaultAddress;
  const auto FaultEIP =
      Input.Kind == Kind::Software ? State.EIP : Input.SavedEIP;
  auto Event = Input;
  for (;;) {
    ++R.DeliveryAttempts;
    auto Got = Entry(State, Memory, Event, Options).run();
    if (!Got)
      return Got.takeError();
    R.SelfModifications.insert(R.SelfModifications.end(),
                               Got->SelfModifications.begin(),
                               Got->SelfModifications.end());
    if (Got->Status == X86PageWalkStatus::BackingFault) {
      R.Status = X86LegacyExecutionStatus::BackingFault;
      R.BackingFault = Got->BackingFault;
      return R;
    }
    if (Got->Status == X86PageWalkStatus::Completed) {
      R.Status = X86LegacyExecutionStatus::EventDelivered;
      R.DeliveredVector = Event.Vector;
      auto Controls = Memory.controls();
      Controls.RFlags = State.EFlags;
      if (auto Error = Memory.bindControls(Controls))
        return std::move(Error);
      return R;
    }
    R.Exception = Got->Exception;
    const auto Vector = Got->Exception->Vector;
    if (Event.Kind == Kind::Abort && Event.Vector == 8 &&
        (x86ContributoryException(Vector) || x86PageClassException(Vector))) {
      R.Status = X86LegacyExecutionStatus::Shutdown;
      return R;
    }
    const bool Double =
        Event.Kind == Kind::Fault && x86DoubleFaultPair(Event.Vector, Vector);
    Event = {FaultEIP, Double ? 0 : Got->Exception->ErrorCode,
             Double ? 0 : uint32_t(Got->Exception->CR2.value_or(0)),
             uint8_t(Double ? 8 : Vector), Double ? Kind::Abort : Kind::Fault};
  }
}

} // namespace neverd::translate
