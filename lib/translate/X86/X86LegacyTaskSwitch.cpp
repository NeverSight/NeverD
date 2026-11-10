//===- X86LegacyTaskSwitch.cpp - Protected task publication --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered TSS RAM effects, incoming task commit and retained fault contexts.
//===----------------------------------------------------------------------===//

#include "X86LegacyTaskSwitch.h"

#include "neverd/ir/X86/X86SegmentDescriptorRules.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace neverd::translate::detail {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

bool sameCache(const X86LegacySegmentCache &A, const X86LegacySegmentCache &B) {
  return std::tie(A.Base, A.Limit, A.Selector, A.Attributes, A.Unusable) ==
         std::tie(B.Base, B.Limit, B.Selector, B.Attributes, B.Unusable);
}

bool sameControls(const X86PagingState &A, const X86PagingState &B) {
  return std::tie(A.CR0, A.CR3, A.CR4, A.EFER, A.RFlags, A.PKRU, A.PKRS,
                  A.PDPTE) == std::tie(B.CR0, B.CR3, B.CR4, B.EFER, B.RFlags,
                                       B.PKRU, B.PKRS, B.PDPTE);
}

struct TaskState {
  X86LegacySegmentState Core;
  X86LegacySegmentCache TR;
  X86PagingState Controls;
  std::optional<X86LegacyDebugState> Debug;
  uint32_t EIP, Flags;

  TaskState(const X86LegacyExecutionState &S, const X86PagedMemory &M)
      : Core(S.Core), TR(S.TR), Controls(M.controls()), Debug(S.Debug),
        EIP(S.EIP), Flags(S.EFlags) {}

  bool matches(const X86LegacyExecutionState &S, const X86PagedMemory &M,
               bool CheckDebug) const {
    if (Core.GPR != S.Core.GPR || Core.CPL != S.Core.CPL ||
        Core.GDTRBase != S.Core.GDTRBase ||
        Core.GDTRLimit != S.Core.GDTRLimit ||
        !sameCache(Core.LDTR, S.Core.LDTR) || !sameCache(TR, S.TR) ||
        EIP != S.EIP || Flags != S.EFlags ||
        !sameControls(Controls, M.controls()))
      return false;
    for (unsigned I = 0; I != Core.Segments.size(); ++I)
      if (!sameCache(Core.Segments[I], S.Core.Segments[I]))
        return false;
    if (CheckDebug &&
        (bool(Debug) != bool(S.Debug) ||
         (Debug && std::tie(Debug->DR, Debug->DR6, Debug->DR7, Debug->DebugCTL,
                            Debug->PendingData) !=
                       std::tie(S.Debug->DR, S.Debug->DR6, S.Debug->DR7,
                                S.Debug->DebugCTL, S.Debug->PendingData))))
      return false;
    return true;
  }
};

} // namespace

struct X86LegacyTaskContinuation {
  struct Access {
    uint32_t Address;
    uint64_t Value;
    uint8_t Bytes;
    bool Write, Incoming, Probe = false;
  };
  TaskState Entry, Expected;
  X86LegacyTaskRequest Request;
  llvm::SmallVector<uint8_t> Instruction;
  llvm::SmallVector<Access> Accesses;
  std::optional<std::array<uint64_t, 4>> PDPTE;
  uint64_t EntryDebugControl = 0;
  uint8_t DataBreakpoints = 0;
  X86LegacyTaskBinding Binding;
  X86LegacyDebugProfile DebugProfile;
  std::optional<X86FullSegmentLimitChoice> FullLimitChoice;
  std::shared_ptr<const X86LegacyTaskContinuation> PriorFault;
  bool Committed = false;
  bool Fault = false;
  bool ModeUnavailable = false;

  X86LegacyTaskContinuation(const X86LegacyExecutionState &S,
                            const X86PagedMemory &M,
                            const X86LegacyTaskRequest &R,
                            llvm::ArrayRef<uint8_t> Bytes,
                            const X86LegacyExecutionOptions &Options)
      : Entry(S, M), Expected(Entry), Request(R),
        Instruction(Bytes.begin(), Bytes.end()),
        EntryDebugControl(S.Debug ? S.Debug->DebugCTL : 0),
        Binding(*Options.Tasks), DebugProfile(*Options.Debug),
        FullLimitChoice(Options.Segments.FullLimitChoice),
        PriorFault(S.PendingTask) {}
};

namespace {

class TaskSwitch {
public:
  using Value = uint64_t;
  using Rules = neverd::detail::X86SegmentDescriptorRules<TaskSwitch>;

  TaskSwitch(X86LegacyExecutionState &State, X86PagedMemory &Memory,
             const X86LegacyExecutionOptions &Options,
             std::shared_ptr<X86LegacyTaskContinuation> Pending)
      : S(State), M(Memory), O(Options), P(std::move(Pending)), Q(P->Request) {}

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

  bool faultIf(bool Bad, uint8_t Vector, Value Error) {
    if (Bad && R.Status == X86PageWalkStatus::Completed) {
      R.Status = X86PageWalkStatus::Exception;
      R.Exception = X86MemoryException{
          Vector, uint32_t(Error) | uint32_t(Q.External), {}};
    }
    return Bad;
  }

  llvm::Expected<X86LegacyTaskResult> run() {
    execute();
    if (Failure)
      return std::move(*Failure);
    R.Committed = Incoming;
    if (R.ModeUnavailable || R.Status == X86PageWalkStatus::BackingFault ||
        (Incoming && R.Status == X86PageWalkStatus::Exception)) {
      P->Committed = Incoming;
      P->Fault = R.Status == X86PageWalkStatus::Exception;
      P->ModeUnavailable = R.ModeUnavailable;
      P->Expected = TaskState(S, M);
      if (Incoming)
        P->PriorFault.reset();
      S.PendingTask = P;
    } else {
      S.PendingTask = Incoming ? nullptr : P->PriorFault;
    }
    return std::move(R);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyExecutionOptions &O;
  std::shared_ptr<X86LegacyTaskContinuation> P;
  const X86LegacyTaskRequest &Q;
  X86LegacyTaskResult R;
  std::optional<llvm::Error> Failure;
  size_t AccessIndex = 0;
  bool Incoming = false;

  bool accept(const X86MemoryAccessResult &Got) {
    R.SelfModifications.insert(R.SelfModifications.end(),
                               Got.SelfModifications.begin(),
                               Got.SelfModifications.end());
    R.Status = Got.Status;
    R.Exception = Got.Exception;
    R.BackingFault = Got.BackingFault;
    if (Got.Exception && Got.Exception->CR2)
      S.CR2 = *Got.Exception->CR2;
    return Got.Status == X86PageWalkStatus::Completed;
  }

  bool bind(const X86PagingState &Controls) {
    if (auto Error = M.bindControls(Controls)) {
      Failure = std::move(Error);
      return false;
    }
    return true;
  }

  bool access(uint32_t Address, unsigned Bytes, Value &V, bool Write = false,
              bool Stack = false) {
    if (AccessIndex < P->Accesses.size()) {
      const auto &Previous = P->Accesses[AccessIndex++];
      if (Previous.Address != Address || Previous.Bytes != Bytes ||
          Previous.Write != Write || Previous.Incoming != Incoming ||
          Previous.Probe || (Write && Previous.Value != V)) {
        Failure = invalid("task continuation disagrees with its access phase");
        return false;
      }
      if (!Write)
        V = Previous.Value;
      return true;
    }
    std::array<uint8_t, 8> Buffer{};
    for (unsigned I = 0; I != Bytes; ++I)
      Buffer[I] = V >> (I * 8);
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Bytes);
    const X86LinearAccess A{Address,
                            Write ? MemoryAccessKind::Write
                                  : MemoryAccessKind::Read,
                            Stack ? S.Core.CPL : uint8_t{0},
                            !Stack,
                            Stack,
                            true};
    auto Got = Write ? M.store(A, Span) : M.load(A, Span);
    if (!Got) {
      Failure = Got.takeError();
      return false;
    }
    if (!accept(*Got))
      return false;
    if (!Write) {
      V = 0;
      for (unsigned I = 0; I != Bytes; ++I)
        V |= Value(Buffer[I]) << (I * 8);
    }
    P->Accesses.push_back({Address, V, uint8_t(Bytes), Write, Incoming});
    ++AccessIndex;
    return true;
  }

  bool store(uint32_t Address, unsigned Bytes, Value V) {
    return access(Address, Bytes, V, true);
  }

  bool probe(uint32_t Address, unsigned Bytes, bool Write) {
    if (AccessIndex < P->Accesses.size()) {
      const auto &Previous = P->Accesses[AccessIndex++];
      if (Previous.Address != Address || Previous.Bytes != Bytes ||
          Previous.Write != Write || Previous.Incoming != Incoming ||
          !Previous.Probe) {
        Failure =
            invalid("task continuation disagrees with its residency check");
        return false;
      }
      return true;
    }
    auto Got = M.probe(
        {Address, Write ? MemoryAccessKind::Write : MemoryAccessKind::Read, 0,
         true, false, true},
        Bytes);
    if (!Got) {
      Failure = Got.takeError();
      return false;
    }
    if (!accept(*Got))
      return false;
    P->Accesses.push_back({Address, 0, uint8_t(Bytes), Write, Incoming, true});
    ++AccessIndex;
    return true;
  }

  bool descriptor(uint16_t Selector, uint8_t Vector, Value &D,
                  uint32_t &Address, bool Global = false) {
    const auto &Core = Incoming ? S.Core : P->Entry.Core;
    const bool Local = Selector & 4;
    if (faultIf(!(Selector & 0xfffc) || (Global && Local) ||
                    (Local && Core.LDTR.Unusable) ||
                    (Selector & 0xfff8u) + 7 >
                        (Local ? Core.LDTR.Limit : Core.GDTRLimit),
                Vector, Selector & 0xfffc))
      return false;
    Address = (Local ? Core.LDTR.Base : Core.GDTRBase) + (Selector & 0xfff8);
    return access(Address, 8, D);
  }

  X86LegacySegmentCache cache(uint16_t Selector, Value D) {
    return {uint32_t(Rules::base(*this, D)), uint32_t(Rules::limit(*this, D)),
            Selector, uint16_t((D >> 40) & 0xf0ff), false};
  }

  bool accessed(uint32_t Address, Value D) {
    return (D & (Value{1} << 40)) || store(Address, 8, D | (Value{1} << 40));
  }

  bool loadLDT(uint16_t Selector) {
    auto &LDT = S.Core.LDTR;
    LDT.Selector = Selector;
    LDT.Unusable = true;
    if (!(Selector & 0xfffc))
      return true;
    Value D = 0;
    uint32_t Address = 0;
    if (!descriptor(Selector, 10, D, Address, true) ||
        faultIf(((D >> 40) & 0x9f) != 0x82, 10, Selector & 0xfffc))
      return false;
    LDT = cache(Selector, D);
    return true;
  }

  bool loadSegment(unsigned Index) {
    auto &C = S.Core.Segments[Index];
    const uint16_t Selector = C.Selector;
    if (Index != 1 && Index != 2 && !(Selector & 0xfffc))
      return true;
    Value D = 0;
    uint32_t Address = 0;
    if (!descriptor(Selector, 10, D, Address))
      return false;
    const unsigned Attributes = D >> 40;
    const unsigned DPL = (Attributes >> 5) & 3;
    const bool Code = Attributes & 8, Conforming = Code && (Attributes & 4);
    const bool ValidType = (Attributes & 16) && !(Attributes & 0x2000) &&
                           (Index == 1   ? Code
                            : Index == 2 ? !Code && (Attributes & 2)
                                         : !Code || (Attributes & 2));
    const bool Privilege =
        Index == 1 ? (Conforming ? DPL <= S.Core.CPL : DPL == S.Core.CPL)
        : Index == 2
            ? DPL == S.Core.CPL && (Selector & 3) == S.Core.CPL
            : Conforming || (DPL >= S.Core.CPL && DPL >= (Selector & 3));
    if (faultIf(!ValidType || !Privilege, 10, Selector & 0xfffc) ||
        faultIf(!(Attributes & 0x80), Index == 2 ? 12 : 11,
                Selector & 0xfffc) ||
        !accessed(Address, D))
      return false;
    C = cache(Selector, D | (Value{1} << 40));
    return true;
  }

  bool saveOutgoing() {
    const auto &E = P->Entry;
    const bool Wide = (E.TR.Attributes & 15) == 11;
    const unsigned Width = Wide ? 4 : 2;
    const uint32_t Base = E.TR.Base;
    const uint32_t Flags = Q.Reason == X86LegacyTaskReason::Return
                               ? Q.SavedFlags & ~0x4000u
                               : Q.SavedFlags;
    if (!store(Base + (Wide ? 32 : 14), Width, Q.SavedEIP) ||
        !store(Base + (Wide ? 36 : 16), Width, Flags))
      return false;
    for (unsigned I = 0; I != 8; ++I)
      if (!store(Base + (Wide ? 40 : 18) + I * Width, Width, E.Core.GPR[I]))
        return false;
    for (unsigned I = 0; I != (Wide ? 6 : 4); ++I)
      if (!store(Base + (Wide ? 72 : 34) + I * Width, 2,
                 E.Core.Segments[I].Selector))
        return false;
    // CR3, LDTR and the privilege stacks are static TSS inputs, not saves.
    return true;
  }

  void execute() {
    if (!bind(P->Entry.Controls))
      return;
    const bool Return = Q.Reason == X86LegacyTaskReason::Return;
    const bool Nested = Q.Reason == X86LegacyTaskReason::Call ||
                        Q.Reason == X86LegacyTaskReason::Interrupt;
    Value Selector = Q.Selector;
    if (Return &&
        faultIf(P->Entry.TR.Limit < 1, 10, P->Entry.TR.Selector & 0xfffc))
      return;
    if (Return && !access(P->Entry.TR.Base, 2, Selector))
      return;
    const uint8_t InvalidTarget = Return ? 10 : 13;
    Value D = Q.Descriptor.value_or(0);
    uint32_t Address = P->Entry.Core.GDTRBase + (Selector & 0xfff8);
    if (Q.Descriptor) {
      if (faultIf((Selector & 4) || !(Selector & 0xfffc), InvalidTarget,
                  Selector & 0xfffc))
        return;
    } else if (!descriptor(Selector, InvalidTarget, D, Address, true)) {
      return;
    }
    const unsigned Type = (D >> 40) & 31;
    const bool Wide = Type == 9 || Type == 11;
    if (faultIf(Type != (Return ? 3u : 1u) && Type != (Return ? 11u : 9u),
                InvalidTarget, Selector & 0xfffc) ||
        faultIf(!Return && !Q.ThroughGate &&
                    (((D >> 45) & 3) < P->Entry.Core.CPL ||
                     ((D >> 45) & 3) < (Selector & 3)),
                13, Selector & 0xfffc) ||
        faultIf(!(D & (Value{1} << 47)), 11, Selector & 0xfffc))
      return;
    auto TR = cache(Selector, D | (Value{2} << 40));
    if (faultIf(TR.Limit < (Wide ? 103u : 43u), 10, Selector & 0xfffc))
      return;
    if (!Wide && !P->Binding.WordGPRHigh) {
      Failure = invalid("word task requires explicit incoming GPR high halves");
      return;
    }
    const bool OldWide = (P->Entry.TR.Attributes & 15) == 11;
    if (faultIf(P->Entry.TR.Limit < (OldWide ? 95u : 41u), 10,
                P->Entry.TR.Selector & 0xfffc))
      return;
    // Residency/permission checks precede busy bits and outgoing saves.
    // This RAM profile retains each successful probe across host suspension.
    if (!probe(TR.Base, Wide ? 104 : 44, false) ||
        (Nested && !probe(TR.Base, 2, true)) ||
        !probe(P->Entry.TR.Base + (OldWide ? 32 : 14), OldWide ? 64 : 28,
               true) ||
        (!Nested &&
         !probe(P->Entry.Core.GDTRBase + (P->Entry.TR.Selector & 0xfff8), 8,
                true)) ||
        (!Return && !probe(Address, 8, true)))
      return;
    if (!Nested) {
      Value Old = 0;
      uint32_t OldAddress = 0;
      if (!descriptor(P->Entry.TR.Selector, 10, Old, OldAddress, true) ||
          !store(OldAddress, 8, Old & ~(Value{2} << 40)))
        return;
    }
    if (!saveOutgoing() ||
        (Nested && !store(TR.Base, 2, P->Entry.TR.Selector)) ||
        (!Return && !store(Address, 8, D | (Value{2} << 40))))
      return;

    const unsigned Width = Wide ? 4 : 2;
    Value IP = 0, Flags = 0, Root = P->Entry.Controls.CR3, LDT = 0, Trap = 0;
    std::array<uint32_t, 8> GPR{};
    std::array<uint16_t, 6> Segments{};
    if ((Wide && !access(TR.Base + 28, 4, Root)) ||
        !access(TR.Base + (Wide ? 32 : 14), Width, IP) ||
        !access(TR.Base + (Wide ? 36 : 16), Width, Flags))
      return;
    for (unsigned I = 0; I != GPR.size(); ++I) {
      Value V = 0;
      if (!access(TR.Base + (Wide ? 40 : 18) + I * Width, Width, V))
        return;
      GPR[I] = Wide
                   ? uint32_t(V)
                   : uint32_t((*P->Binding.WordGPRHigh)[I]) << 16 | uint16_t(V);
    }
    for (unsigned I = 0; I != (Wide ? 6 : 4); ++I) {
      Value V = 0;
      if (!access(TR.Base + (Wide ? 72 : 34) + I * Width, 2, V))
        return;
      Segments[I] = V;
    }
    if (!access(TR.Base + (Wide ? 96 : 42), 2, LDT) ||
        (Wide && !access(TR.Base + 100, 2, Trap)))
      return;
    auto Controls = P->Entry.Controls;
    Controls.CR0 |= 8;
    bool InvalidPDPTE = false;
    if (Wide && (Controls.CR0 & 0x80000000u)) {
      Controls.CR3 = Root;
      if ((Controls.CR4 & 0x20) && Root != P->Entry.Controls.CR3) {
        if (P->PDPTE) {
          Controls.PDPTE = *P->PDPTE;
        } else {
          auto Got = loadX86PAEPDPTE(M.physicalMemory(), M.features(), Root,
                                     Controls.PDPTE);
          if (!Got) {
            Failure = Got.takeError();
            return;
          }
          if (Got->Status == X86PageWalkStatus::BackingFault) {
            R.Status = Got->Status;
            R.BackingFault = Got->BackingFault;
            return;
          }
          if (Got->Status == X86PageWalkStatus::Exception) {
            // A task load has a different failure owner from MOV CR3. The
            // bound task profile publishes the new task, an empty PDPTE bank
            // and #TS(0); it never propagates MOV-CR's pre-commit #GP.
            Controls.PDPTE = {};
            InvalidPDPTE = true;
          } else {
            P->PDPTE = Controls.PDPTE;
          }
        }
      }
    }
    Controls.RFlags = (Flags & 0x3f7fd7u) | 2 | (Nested ? 0x4000 : 0);
    if (!bind(Controls))
      return;

    // The incoming register/selector image now owns subsequent faults. The
    // cache loads below may fail; those failures do not undo this publication.
    Incoming = true;
    S.Core = P->Entry.Core;
    S.Core.GPR = GPR;
    S.Core.CPL = Controls.RFlags & 0x20000 ? 3 : Segments[1] & 3;
    for (unsigned I = 0; I != Segments.size(); ++I) {
      S.Core.Segments[I].Selector = Segments[I];
      S.Core.Segments[I].Unusable = true;
    }
    S.Core.LDTR.Selector = LDT;
    S.Core.LDTR.Unusable = true;
    S.TR = TR;
    S.EIP = IP;
    S.EFlags = Controls.RFlags;
    S.Halted = S.STIBlocked = S.SSBlocked = false;
    x86CommitTaskDebug(*S.Debug);
    // The protected executor cannot fetch VM86 instructions or deliver its
    // faults. Preserve the published image at a visible mode boundary before
    // attempting its LDT/segment loads, rather than reporting completion.
    if (S.EFlags & 0x20000) {
      R.ModeUnavailable = true;
      return;
    }
    if (faultIf(InvalidPDPTE, 10, 0))
      return;
    if (!loadLDT(LDT))
      return;
    for (unsigned I : {1u, 2u, 0u, 3u, 4u, 5u})
      if (!loadSegment(I))
        return;
    if (Q.ErrorCode) {
      auto &SP = S.Core.GPR[4];
      const auto &SS = S.Core.Segments[2];
      const uint32_t Mask = SS.Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
      auto Checked = checkX86LegacySegmentAccess(
          SS, {X86SegmentRegister::SS, X86SegmentAccessKind::Write,
               (SP - Width) & Mask, Width, O.Segments.FullLimitChoice});
      if (!Checked) {
        Failure = Checked.takeError();
        return;
      }
      Value Error = *Q.ErrorCode;
      if (faultIf(bool(Checked->FaultVector), 12, 0) ||
          !access(Checked->Linear, Width, Error, true, true))
        return;
      SP = (SP & ~Mask) | ((SP - Width) & Mask);
    }
    if (faultIf(S.EIP > S.Core.Segments[1].Limit, 13, 0))
      return;
    if (AccessIndex != P->Accesses.size()) {
      Failure = invalid("task continuation has unconsumed RAM effects");
      return;
    }
    R.DebugTrap = Trap & 1;
  }
};

llvm::Error validateBinding(const X86LegacyExecutionState &S,
                            const X86PagedMemory &M,
                            const X86LegacyExecutionOptions &O) {
  if (!O.Tasks || O.Tasks->Profile != X86LegacyTaskProfile::OrderedTSS ||
      !S.Debug || !O.Debug || !O.Segments.FullLimitChoice)
    return invalid("task-switch requires ordered TSS, debug-state and "
                   "full-limit bindings");
  const auto &TR =
      S.PendingTask && !S.PendingTask->Fault ? S.PendingTask->Entry.TR : S.TR;
  const unsigned Type = TR.Attributes & 15;
  if (TR.Unusable || (TR.Selector & 4) || !(TR.Selector & 0xfff8) ||
      (TR.Attributes & 0x90) != 0x80 || (Type != 3 && Type != 11) ||
      (M.controls().EFER & 0x400))
    return invalid("task-switch requires an authenticated legacy busy TSS");
  return llvm::Error::success();
}

} // namespace

llvm::Expected<X86LegacyTaskResult>
switchLegacyTask(X86LegacyExecutionState &S, X86PagedMemory &M,
                 const X86LegacyTaskRequest &Request,
                 llvm::ArrayRef<uint8_t> Instruction,
                 const X86LegacyExecutionOptions &Options) {
  if (S.PendingTask && !S.PendingTask->Fault)
    return invalid("resume the retained task before starting another switch");
  if (auto Error = validateBinding(S, M, Options))
    return std::move(Error);
  auto Pending = std::make_shared<X86LegacyTaskContinuation>(
      S, M, Request, Instruction, Options);
  return TaskSwitch(S, M, Options, std::move(Pending)).run();
}

llvm::Expected<X86LegacyTaskResult>
resumeLegacyTask(X86LegacyExecutionState &S, X86PagedMemory &M,
                 llvm::ArrayRef<uint8_t> Instruction,
                 const X86LegacyExecutionOptions &Options) {
  if (!S.PendingTask || S.PendingTask->Fault ||
      llvm::ArrayRef(S.PendingTask->Instruction) != Instruction)
    return invalid("resume requires the original suspended task instruction");
  if (auto Error = validateLegacyTaskState(S, M))
    return std::move(Error);
  if (auto Error = validateBinding(S, M, Options))
    return std::move(Error);
  if (Options.Tasks->WordGPRHigh != S.PendingTask->Binding.WordGPRHigh ||
      Options.Debug != S.PendingTask->DebugProfile ||
      Options.Segments.FullLimitChoice != S.PendingTask->FullLimitChoice)
    return invalid("task resume requires the original execution bindings");
  auto Pending = std::make_shared<X86LegacyTaskContinuation>(*S.PendingTask);
  return TaskSwitch(S, M, Options, std::move(Pending)).run();
}

bool legacyTaskFaultContext(const X86LegacyExecutionState &S) {
  return S.PendingTask && S.PendingTask->Fault && S.PendingTask->Committed;
}

bool legacyTaskInstructionPending(const X86LegacyExecutionState &S) {
  return S.PendingTask && !S.PendingTask->Fault &&
         !S.PendingTask->Request.EventOwner;
}

bool legacyTaskEventPending(const X86LegacyExecutionState &S) {
  return S.PendingTask && !S.PendingTask->Fault &&
         S.PendingTask->Request.EventOwner;
}

bool legacyTaskModeUnavailable(const X86LegacyExecutionState &S) {
  return S.PendingTask && S.PendingTask->ModeUnavailable;
}

uint64_t legacyTaskInstructionKey(const X86LegacyExecutionState &S) {
  const auto &E = S.PendingTask->Entry;
  return uint64_t(uint32_t(E.Core.Segments[1].Base + E.EIP)) |
         (uint64_t(bool(E.Core.Segments[1].Attributes & 0x4000)) << 32);
}

uint32_t legacyTaskEntryFlags(const X86LegacyExecutionState &S) {
  return S.PendingTask->Entry.Flags;
}

uint64_t legacyTaskEntryDebugControl(const X86LegacyExecutionState &S) {
  return S.PendingTask->EntryDebugControl;
}

uint8_t legacyTaskWatchpoints(const X86LegacyExecutionState &S) {
  return S.PendingTask->DataBreakpoints;
}

void retainLegacyTaskWatchpoints(X86LegacyExecutionState &S, uint8_t Matches) {
  if (!S.PendingTask || S.PendingTask->Fault)
    return;
  auto P = std::make_shared<X86LegacyTaskContinuation>(*S.PendingTask);
  P->DataBreakpoints = Matches;
  S.PendingTask = std::move(P);
}

llvm::Error validateLegacyTaskState(const X86LegacyExecutionState &S,
                                    const X86PagedMemory &M) {
  if (!S.PendingTask ||
      !S.PendingTask->Expected.matches(S, M, !S.PendingTask->Fault))
    return invalid(
        "task continuation requires its retained CPU and paging state");
  return llvm::Error::success();
}

void completeLegacyTaskFault(X86LegacyExecutionState &S) {
  if (legacyTaskFaultContext(S))
    S.PendingTask.reset();
}

} // namespace neverd::translate::detail
