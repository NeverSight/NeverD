//===- X86LegacyFrameReturn.cpp - Faultable protected returns ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Protected 16/32-bit IRET and RETF over segmented, paged guest RAM.
//===----------------------------------------------------------------------===//

#include "X86LegacyExecutionInternal.h"

#include "neverd/ir/X86/X86ReturnRules.h"
#include "neverd/ir/X86/X86SegmentDescriptorRules.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate {
namespace {

class FrameReturn {
public:
  using Value = uint64_t;
  using Descriptors = neverd::detail::X86SegmentDescriptorRules<FrameReturn>;
  using Returns = neverd::detail::X86ReturnRules<FrameReturn>;

  FrameReturn(X86LegacyExecutionState &State, X86PagedMemory &Memory,
              const X86LegacyExecutionOptions &Options)
      : S(State), M(Memory), O(Options) {}

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
      R.Exception = X86MemoryException{Vector, uint32_t(Error), {}};
    }
    return Bad;
  }

  llvm::Expected<X86MemoryAccessResult> run(unsigned Bytes, uint16_t Discard,
                                            bool Interrupt) {
    execute(Bytes, Discard, Interrupt);
    if (Failure)
      return std::move(*Failure);
    return std::move(R);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyExecutionOptions &O;
  X86MemoryAccessResult R;
  std::optional<llvm::Error> Failure;

  void unsupported(const char *Message) {
    Failure = llvm::createStringError(llvm::errc::not_supported, Message);
  }

  bool access(uint32_t Address, unsigned Bytes, Value &V, bool Implicit,
              bool Write = false) {
    const bool Misaligned = !Implicit && S.Core.CPL == 3 &&
                            (M.controls().CR0 & 0x40000) &&
                            (S.EFlags & 0x40000) && (Address & (Bytes - 1));
    if (Misaligned &&
        O.Segments.AlignmentPriority == X86AlignmentPriority::BeforePaging) {
      faultIf(true, 17, 0);
      return false;
    }
    std::array<uint8_t, 8> Buffer{};
    for (unsigned I = 0; I != Bytes; ++I)
      Buffer[I] = V >> (I * 8);
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Bytes);
    const X86LinearAccess A{
        Address,    Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        S.Core.CPL, Implicit,
        !Implicit,  true};
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
      return false;
    }
    if (Misaligned) {
      faultIf(true, 17, 0);
      return false;
    }
    if (!Write) {
      V = 0;
      for (unsigned I = 0; I != Bytes; ++I)
        V |= Value(Buffer[I]) << (I * 8);
    }
    return true;
  }

  bool frame(unsigned Offset, unsigned Bytes, Value &V) {
    const auto &SS = S.Core.Segments[2];
    const uint32_t Mask = SS.Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
    auto Range = checkX86LegacySegmentAccess(
        SS,
        {X86SegmentRegister::SS, X86SegmentAccessKind::Read,
         (S.Core.GPR[4] + Offset) & Mask, Bytes, O.Segments.FullLimitChoice});
    if (!Range) {
      Failure = Range.takeError();
      return false;
    }
    if (faultIf(bool(Range->FaultVector), 12, 0))
      return false;
    return access(Range->Linear, Bytes, V, false);
  }

  bool descriptor(uint16_t Selector, Value &D, uint32_t &Address) {
    if (faultIf(!(Selector & 0xfffc), 13, 0))
      return false;
    const bool Local = Selector & 4;
    const auto &C = S.Core;
    if (faultIf((Local && C.LDTR.Unusable) ||
                    (Selector & 0xfff8u) + 7 >
                        (Local ? C.LDTR.Limit : C.GDTRLimit),
                13, Selector & 0xfffc))
      return false;
    Address = (Local ? C.LDTR.Base : C.GDTRBase) + (Selector & 0xfff8);
    return access(Address, 8, D, true);
  }

  bool accessed(uint32_t Address, Value D) {
    if (D & (Value{1} << 40))
      return true;
    D |= Value{1} << 40;
    return access(Address, 8, D, true, true);
  }

  X86LegacySegmentCache cache(uint16_t Selector, Value D) {
    return {uint32_t(Descriptors::base(*this, D)),
            uint32_t(Descriptors::limit(*this, D)), Selector,
            uint16_t(Descriptors::attributes(*this, D)), false};
  }

  void execute(unsigned Bytes, uint16_t Discard, bool Interrupt) {
    if (Interrupt)
      S.NMIBlocked = false;
    Value Flags = 0, Selector = 0, IP = 0;
    if (Interrupt && !frame(2 * Bytes, Bytes, Flags))
      return;
    if (Interrupt && S.Core.CPL == 0 && (Flags & 0x20000)) {
      unsupported("legacy IRET to VM86 requires virtual-mode state");
      return;
    }
    // Selector accesses are words even in dword frame slots. Their padding
    // does not participate in segment, page or alignment checks.
    if (!frame(Bytes, 2, Selector) || !frame(0, Bytes, IP))
      return;
    Value Code = 0;
    uint32_t CodeAddress = 0;
    if (!descriptor(Selector, Code, CodeAddress))
      return;
    Returns::checkCode(*this, Selector, Code, S.Core.CPL, false);
    if (R.Status != X86PageWalkStatus::Completed)
      return;
    const auto CS = cache(Selector, Code);
    if (CS.Attributes & 0x2000) {
      unsupported("legacy return code has unsupported reserved L bit");
      return;
    }
    if (!accessed(CodeAddress, Code))
      return;
    const uint8_t CPL = Selector & 3;
    const bool Outer = CPL > S.Core.CPL;
    auto SS = S.Core.Segments[2];
    Value SP = S.Core.GPR[4] + (Interrupt ? 3 : 2) * Bytes;
    if (Outer) {
      const unsigned Offset = (Interrupt ? 3 : 2) * Bytes + Discard;
      Value StackSelector = 0, Stack = 0;
      uint32_t StackAddress = 0;
      if (!frame(Offset + Bytes, 2, StackSelector) || !frame(Offset, Bytes, SP))
        return;
      if (!descriptor(StackSelector, Stack, StackAddress))
        return;
      Descriptors::checkData(*this, true, StackSelector, Stack, CPL);
      if (R.Status != X86PageWalkStatus::Completed)
        return;
      SS = cache(StackSelector, Stack);
      if (auto Error =
              validateX86LegacySegmentCache(X86SegmentRegister::SS, SS)) {
        Failure = std::move(Error);
        return;
      }
      if (!accessed(StackAddress, Stack))
        return;
    }
    if (faultIf(IP > CS.Limit, 13, 0))
      return;
    const uint32_t Mask = SS.Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
    const uint32_t NewSP = (S.Core.GPR[4] & ~Mask) | ((SP + Discard) & Mask);
    const uint32_t NewFlags =
        Interrupt
            ? Returns::interruptFlags(*this, S.EFlags, Flags, S.Core.CPL, Bytes)
            : S.EFlags & ~0x10000u;
    if (Outer)
      for (unsigned I : {0u, 3u, 4u, 5u}) {
        auto &C = S.Core.Segments[I];
        if (Returns::invalidateData(*this, C.Selector, C.Attributes, CPL)) {
          C.Selector = 0;
          C.Unusable = true;
        }
      }
    S.Core.Segments[1] = CS;
    S.Core.Segments[2] = SS;
    S.Core.CPL = CPL;
    S.Core.GPR[4] = NewSP;
    S.EIP = IP;
    S.EFlags = NewFlags;
  }
};

} // namespace

llvm::Expected<X86MemoryAccessResult>
detail::returnLegacyFrame(X86LegacyExecutionState &State,
                          X86PagedMemory &Memory, unsigned Bytes,
                          uint16_t Discard, bool Interrupt,
                          const X86LegacyExecutionOptions &Options) {
  return FrameReturn(State, Memory, Options).run(Bytes, Discard, Interrupt);
}

} // namespace neverd::translate
