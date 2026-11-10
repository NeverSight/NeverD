//===- X86LegacyFarTransfer.cpp - Protected far branches -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Protected direct far branches and 16/32-bit call gates over paged RAM.
//===----------------------------------------------------------------------===//

#include "X86LegacyFarTransfer.h"

#include "neverd/ir/X86/X86SegmentDescriptorRules.h"

#include "llvm/Support/Errc.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate {
namespace {

class FarTransfer {
public:
  using Value = uint64_t;
  using Rules = neverd::detail::X86SegmentDescriptorRules<FarTransfer>;

  FarTransfer(X86LegacyExecutionState &State, X86PagedMemory &Memory,
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

  llvm::Expected<detail::X86LegacyTaskResult>
  run(const detail::X86LegacyFarTransfer &F, uint32_t NextIP,
      llvm::ArrayRef<uint8_t> Bytes) {
    Original = Bytes;
    execute(F, NextIP);
    if (Failure)
      return std::move(*Failure);
    return std::move(R);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyExecutionOptions &O;
  detail::X86LegacyTaskResult R;
  std::optional<llvm::Error> Failure;
  llvm::ArrayRef<uint8_t> Original;

  bool accept(llvm::Expected<X86MemoryAccessResult> Got) {
    if (!Got) {
      Failure = Got.takeError();
      return false;
    }
    R.SelfModifications.insert(R.SelfModifications.end(),
                               Got->SelfModifications.begin(),
                               Got->SelfModifications.end());
    if (Got->Status == X86PageWalkStatus::Completed)
      return true;
    R.Status = Got->Status;
    R.Exception = Got->Exception;
    R.BackingFault = Got->BackingFault;
    return false;
  }

  bool access(uint32_t Address, unsigned Bytes, Value &V, bool Write = false,
              bool Implicit = true, uint8_t CPL = 0, bool Stack = false) {
    const bool Misaligned = !Implicit && CPL == 3 &&
                            (M.controls().CR0 & 0x40000) &&
                            (S.EFlags & 0x40000) && (Address & (Bytes - 1));
    if (Misaligned &&
        O.Segments.AlignmentPriority == X86AlignmentPriority::BeforePaging)
      return !faultIf(true, 17, 0);
    std::array<uint8_t, 8> Buffer{};
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Bytes);
    const X86LinearAccess A{
        Address, Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
        CPL,     Implicit,
        Stack,   true};
    if (Write) {
      auto Store = M.prepareStore(A, Bytes);
      if (!Store) {
        Failure = Store.takeError();
        return false;
      }
      if (!accept(static_cast<X86MemoryAccessResult &>(*Store)) ||
          faultIf(Misaligned, 17, 0))
        return false;
      for (unsigned I = 0; I != Bytes; ++I)
        Buffer[I] = V >> (I * 8);
      M.commitStore(*Store, Span);
    } else {
      if (!accept(M.load(A, Span)) || faultIf(Misaligned, 17, 0))
        return false;
      V = 0;
      for (unsigned I = 0; I != Bytes; ++I)
        V |= Value(Buffer[I]) << (I * 8);
    }
    return true;
  }

  bool operand(X86SegmentRegister Segment, uint32_t Offset, unsigned Bytes,
               Value &V) {
    auto Range = checkX86LegacySegmentAccess(
        S.Core.Segments[unsigned(Segment)],
        {Segment, X86SegmentAccessKind::Read, Offset, Bytes,
         O.Segments.FullLimitChoice});
    if (!Range) {
      Failure = Range.takeError();
      return false;
    }
    if (Range->FaultVector)
      return !faultIf(true, *Range->FaultVector, 0);
    return access(Range->Linear, Bytes, V, false, false, S.Core.CPL,
                  Segment == X86SegmentRegister::SS);
  }

  bool descriptor(uint16_t Selector, uint8_t Fault, Value &D,
                  uint32_t &Address) {
    if (faultIf(!(Selector & 0xfffc), Fault, 0))
      return false;
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

  void execute(const detail::X86LegacyFarTransfer &F, uint32_t NextIP) {
    if (faultIf(F.Locked, 6, 0))
      return;
    Value IP = F.Offset, Selector = F.Selector;
    if (F.Memory) {
      const uint32_t Offset = F.Address.offset(S.Core.GPR);
      const uint32_t Mask =
          F.Address.AddressBytes == 4 ? UINT32_MAX : UINT16_MAX;
      if (!operand(F.Address.MemorySegment, Offset, F.Bytes, IP) ||
          !operand(F.Address.MemorySegment, (Offset + F.Bytes) & Mask, 2,
                   Selector))
        return;
    }
    Value Code = 0;
    uint32_t CodeAddress = 0;
    if (!descriptor(Selector, 13, Code, CodeAddress))
      return;
    const unsigned Type = (Code >> 40) & 31;
    if (Type == 1 || Type == 3 || Type == 5 || Type == 9 || Type == 11) {
      if (Type == 5 &&
          (faultIf(((Code >> 45) & 3) < S.Core.CPL ||
                       ((Code >> 45) & 3) < (Selector & 3),
                   13, Selector & 0xfffc) ||
           faultIf(!(Code & (Value{1} << 47)), 11, Selector & 0xfffc)))
        return;
      detail::X86LegacyTaskRequest Request{
          F.Call ? detail::X86LegacyTaskReason::Call
                 : detail::X86LegacyTaskReason::Jump};
      Request.Selector = Type == 5 ? uint16_t(Code >> 16) : uint16_t(Selector);
      Request.SavedEIP = NextIP;
      Request.SavedFlags = S.EFlags;
      Request.ThroughGate = Type == 5;
      if (Type != 5)
        Request.Descriptor = Code;
      auto Got = detail::switchLegacyTask(S, M, Request, Original, O);
      if (!Got) {
        Failure = Got.takeError();
        return;
      }
      Got->SelfModifications.insert(Got->SelfModifications.begin(),
                                    R.SelfModifications.begin(),
                                    R.SelfModifications.end());
      R = std::move(*Got);
      return;
    }
    const bool Gate = Type == 4 || Type == 12;
    unsigned Width = F.Bytes, Parameters = 0;
    if (Gate) {
      const unsigned DPL = (Code >> 45) & 3;
      if (faultIf(DPL < S.Core.CPL || DPL < (Selector & 3), 13,
                  Selector & 0xfffc) ||
          faultIf(!(Code & (Value{1} << 47)), 11, Selector & 0xfffc))
        return;
      Width = Type == 12 ? 4 : 2;
      Parameters = (Code >> 32) & 31;
      IP =
          uint16_t(Code) | (Width == 4 ? uint32_t(Code >> 32) & 0xffff0000 : 0);
      Selector = uint16_t(Code >> 16);
      if (!descriptor(Selector, 13, Code, CodeAddress))
        return;
    }
    const uint16_t Attributes = Code >> 40;
    const uint8_t DPL = (Attributes >> 5) & 3;
    const bool Conforming = Attributes & 4;
    const bool InvalidPrivilege =
        Conforming || (Gate && F.Call)
            ? DPL > S.Core.CPL
            : DPL != S.Core.CPL || (!Gate && (Selector & 3) > S.Core.CPL);
    if (faultIf((Attributes & 0x18) != 0x18 || InvalidPrivilege, 13,
                Selector & 0xfffc) ||
        faultIf(!(Attributes & 0x80), 11, Selector & 0xfffc))
      return;
    if (Attributes & 0x2000) {
      Failure = llvm::createStringError(
          llvm::errc::not_supported,
          "legacy far target has unsupported reserved L bit");
      return;
    }
    const uint8_t CPL = Conforming ? S.Core.CPL : DPL;
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
      Value NewSS = 0, NewSP = 0, Stack = 0;
      uint32_t StackAddress = 0;
      if (!access(TR.Base + Offset, SPBytes, NewSP) ||
          !access(TR.Base + Offset + SPBytes, 2, NewSS))
        return;
      if (faultIf((NewSS & 3) != CPL, 10, NewSS & 0xfffc) ||
          !descriptor(NewSS, 10, Stack, StackAddress))
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
      SP = SS.Attributes & 0x4000
               ? uint32_t(NewSP)
               : (S.Core.GPR[4] & 0xffff0000) | uint16_t(NewSP);
    }
    if (F.Call) {
      const uint32_t Mask = SS.Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
      const unsigned Count = Inner ? 4 + Parameters : 2;
      auto Range = checkX86LegacySegmentAccess(
          SS, {X86SegmentRegister::SS, X86SegmentAccessKind::Write,
               (SP - Count * Width) & Mask, Count * Width,
               O.Segments.FullLimitChoice});
      if (!Range) {
        Failure = Range.takeError();
        return;
      }
      if (faultIf(bool(Range->FaultVector), 12,
                  Inner ? SS.Selector & 0xfffc : 0))
        return;
      const auto Push = [&](Value V) {
        SP = (SP & ~Mask) | ((SP - Width) & Mask);
        return access(SS.Base + (SP & Mask), Width, V, true, false, CPL, true);
      };
      if (Inner) {
        if (!Push(S.Core.Segments[2].Selector) || !Push(S.Core.GPR[4]))
          return;
        const uint32_t OldMask =
            S.Core.Segments[2].Attributes & 0x4000 ? UINT32_MAX : UINT16_MAX;
        // The CPU copies the highest argument first, after publishing old
        // SS:SP. Each completed copy survives a later source/destination fault.
        for (unsigned I = Parameters; I != 0; --I) {
          Value Argument = 0;
          if (!operand(X86SegmentRegister::SS,
                       (S.Core.GPR[4] + (I - 1) * Width) & OldMask, Width,
                       Argument) ||
              !Push(Argument))
            return;
        }
      }
      if (!Push(S.Core.Segments[1].Selector) || !Push(NextIP))
        return;
    }
    if (faultIf(IP > CS.Limit, 13, 0))
      return;
    S.Core.Segments[1] = CS;
    S.Core.Segments[2] = SS;
    S.Core.CPL = CPL;
    S.Core.GPR[4] = SP;
    S.EIP = IP;
  }
};

} // namespace

std::optional<detail::X86LegacyFarTransfer>
detail::decodeLegacyFarTransfer(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  X86LegacyEncoding E(Bytes, Code32);
  if (E.Group1Prefix == 0xf2 || E.Group1Prefix == 0xf3)
    return std::nullopt;
  X86LegacyFarTransfer F;
  F.Bytes = E.OperandBytes;
  F.Locked = E.Group1Prefix == 0xf0;
  const auto Opcode = E.byte();
  if (Opcode == 0x9a || Opcode == 0xea) {
    F.Call = Opcode == 0x9a;
    F.Offset = E.value(F.Bytes);
    F.Selector = E.value(2);
  } else if (Opcode == 0xff) {
    const unsigned Extension = E.modRM(F.Address);
    if ((Extension != 3 && Extension != 5) || F.Address.Register)
      return std::nullopt;
    F.Call = Extension == 3;
    F.Memory = true;
  } else {
    return std::nullopt;
  }
  return E.done() ? std::optional(F) : std::nullopt;
}

llvm::Expected<detail::X86LegacyTaskResult> detail::executeLegacyFarTransfer(
    X86LegacyExecutionState &State, X86PagedMemory &Memory,
    const X86LegacyFarTransfer &Instruction, uint32_t NextIP,
    llvm::ArrayRef<uint8_t> Bytes, const X86LegacyExecutionOptions &Options) {
  return FarTransfer(State, Memory, Options).run(Instruction, NextIP, Bytes);
}

} // namespace neverd::translate
