//===- X86LegacySegmentExecution.cpp - Protected selector transfers ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind legacy instruction operands to shared descriptor and paging rules.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86LegacySegmentExecution.h"

#include "X86LegacySegmentEncoding.h"

#include "neverd/ir/X86/X86SegmentDescriptorRules.h"

#include "llvm/Support/Errc.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

class SegmentExecution {
public:
  using Value = uint64_t;
  using Rules = neverd::detail::X86SegmentDescriptorRules<SegmentExecution>;

  SegmentExecution(X86LegacySegmentState &State, X86PagedMemory &Memory,
                   const X86LegacySegmentExecutionOptions &Options)
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

  void faultIf(bool Bad, uint8_t Vector, Value Error) {
    if (Bad && R.Status == X86PageWalkStatus::Completed) {
      R.Status = X86PageWalkStatus::Exception;
      R.Exception = X86MemoryException{Vector, uint32_t(Error), {}};
    }
  }

  void execute(const detail::X86LegacySegmentInstruction &F) {
    uint32_t Offset = F.Displacement;
    if (F.Base)
      Offset += S.GPR[*F.Base];
    if (F.Index)
      Offset += S.GPR[*F.Index] * F.Scale;
    const bool Stack = F.Push || F.Pop;
    const uint32_t AddressMask =
        Stack ? (cache(X86SegmentRegister::SS).Attributes & 0x4000 ? UINT32_MAX
                                                                   : UINT16_MAX)
        : F.AddressBytes == 4 ? UINT32_MAX
                              : UINT16_MAX;
    if (F.Push)
      Offset -= F.Bytes;
    Offset &= AddressMask;
    const uint32_t NextSP = (S.GPR[4] & ~AddressMask) |
                            ((F.Pop ? Offset + F.Bytes : Offset) & AddressMask);
    if (F.Read) {
      Value Selector = cache(F.Segment).Selector;
      if (F.Register)
        writeRegister(*F.Register, F.Bytes, Selector);
      else if (!operand(F.MemorySegment, Offset, 2, true, Selector))
        return;
      if (F.Push)
        S.GPR[4] = NextSP;
      return;
    }

    Value Selector = 0, Pointer = 0;
    if (F.Far) {
      if (!operand(F.MemorySegment, Offset, F.Bytes, false, Pointer) ||
          !operand(F.MemorySegment, (Offset + F.Bytes) & AddressMask, 2, false,
                   Selector))
        return;
    } else if (F.Register) {
      Selector = uint16_t(S.GPR[*F.Register]);
    } else if (!operand(F.MemorySegment, Offset, 2, false, Selector)) {
      return;
    }
    auto NextCache = cache(F.Segment);
    if (!loadSegment(F.Segment, uint16_t(Selector), NextCache))
      return;
    cache(F.Segment) = NextCache;
    if (F.Far)
      writeRegister(*F.Register, F.Bytes, Pointer);
    if (F.Pop)
      S.GPR[4] = NextSP;
    R.LoadsSSWithInhibition = F.Segment == X86SegmentRegister::SS && !F.Far;
  }

  llvm::Expected<X86LegacySegmentExecutionResult> finish() {
    if (Failure)
      return std::move(*Failure);
    return std::move(R);
  }

private:
  X86LegacySegmentState &S;
  X86PagedMemory &M;
  const X86LegacySegmentExecutionOptions &O;
  X86LegacySegmentExecutionResult R;
  std::optional<llvm::Error> Failure;

  X86LegacySegmentCache &cache(X86SegmentRegister Segment) {
    return S.Segments[unsigned(Segment)];
  }

  void writeRegister(unsigned Register, unsigned Bytes, uint32_t Value) {
    S.GPR[Register] =
        Bytes == 4 ? Value : (S.GPR[Register] & 0xffff0000) | uint16_t(Value);
  }

  bool result(llvm::Expected<X86MemoryAccessResult> Access) {
    if (!Access) {
      Failure = Access.takeError();
      return false;
    }
    if (Access->Status != X86PageWalkStatus::Completed) {
      static_cast<X86MemoryAccessResult &>(R) = std::move(*Access);
      return false;
    }
    return true;
  }

  bool memory(const X86LinearAccess &Access, unsigned Bytes, Value &V,
              bool Misaligned = false) {
    std::array<uint8_t, 8> Buffer{};
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Bytes);
    if (Misaligned &&
        O.AlignmentPriority == X86AlignmentPriority::BeforePaging) {
      faultIf(true, 17, 0);
      return false;
    }
    if (Access.Kind == MemoryAccessKind::Write) {
      auto Store = M.prepareStore(Access, Bytes);
      if (!Store) {
        Failure = Store.takeError();
        return false;
      }
      if (!result(static_cast<X86MemoryAccessResult &>(*Store)))
        return false;
      if (Misaligned) {
        faultIf(true, 17, 0);
        return false;
      }
      for (unsigned I = 0; I != Bytes; ++I)
        Buffer[I] = V >> (I * 8);
      M.commitStore(*Store, Span);
      return true;
    }
    if (!result(M.load(Access, Span)))
      return false;
    if (Misaligned) {
      faultIf(true, 17, 0);
      return false;
    }
    V = 0;
    for (unsigned I = 0; I != Bytes; ++I)
      V |= Value(Buffer[I]) << (I * 8);
    return true;
  }

  bool operand(X86SegmentRegister Segment, uint32_t Offset, unsigned Bytes,
               bool Write, Value &V) {
    auto Address = checkX86LegacySegmentAccess(
        cache(Segment),
        {Segment,
         Write ? X86SegmentAccessKind::Write : X86SegmentAccessKind::Read,
         Offset, Bytes, O.FullLimitChoice});
    if (!Address) {
      Failure = Address.takeError();
      return false;
    }
    if (Address->FaultVector) {
      faultIf(true, *Address->FaultVector, 0);
      return false;
    }
    const bool AC = S.CPL == 3 && (M.controls().CR0 & (1u << 18)) &&
                    (M.controls().RFlags & (1u << 18));
    return memory({Address->Linear,
                   Write ? MemoryAccessKind::Write : MemoryAccessKind::Read,
                   S.CPL, false, Segment == X86SegmentRegister::SS, true},
                  Bytes, V, AC && (Address->Linear & (Bytes - 1)));
  }

  bool loadSegment(X86SegmentRegister Segment, uint16_t Selector,
                   X86LegacySegmentCache &Next) {
    const bool Stack = Segment == X86SegmentRegister::SS;
    if (!(Selector & 0xfffc)) {
      if (Stack) {
        faultIf(true, 13, 0);
        return false;
      }
      Next.Selector = Selector;
      Next.Unusable = true;
      return true;
    }
    const bool Local = Selector & 4;
    const uint32_t Index = Selector & 0xfff8;
    const uint32_t Limit = Local ? S.LDTR.Limit : S.GDTRLimit;
    if ((Local && S.LDTR.Unusable) || Index + 7 > Limit) {
      faultIf(true, 13, Selector & 0xfffc);
      return false;
    }
    const uint32_t Address = (Local ? S.LDTR.Base : S.GDTRBase) + Index;
    Value Descriptor = 0;
    if (!memory({Address, MemoryAccessKind::Read, S.CPL, true, false, true}, 8,
                Descriptor))
      return false;
    Rules::checkData(*this, Stack, Selector, Descriptor, S.CPL);
    if (R.Status != X86PageWalkStatus::Completed)
      return false;
    Next = {uint32_t(Rules::base(*this, Descriptor)),
            uint32_t(Rules::limit(*this, Descriptor)), Selector,
            uint16_t(Rules::attributes(*this, Descriptor)), false};
    if (auto Error = validateX86LegacySegmentCache(Segment, Next)) {
      Failure = std::move(Error);
      return false;
    }
    if (!(Descriptor & (uint64_t{1} << 40))) {
      Descriptor |= uint64_t{1} << 40;
      if (!memory({Address, MemoryAccessKind::Write, S.CPL, true, false, true},
                  8, Descriptor))
        return false;
    }
    return true;
  }
};

} // namespace

llvm::Error validateX86LegacySegmentState(const X86LegacySegmentState &State,
                                          const X86PagingState &Controls) {
  if (!(Controls.CR0 & 1) || (Controls.EFER & 0x400) ||
      (Controls.RFlags & 0x20000) || State.CPL > 3)
    return invalid(
        "legacy segment execution requires bound protected-mode state");
  for (unsigned I = 0; I != State.Segments.size(); ++I)
    if (auto Error = validateX86LegacySegmentCache(
            static_cast<X86SegmentRegister>(I), State.Segments[I]))
      return std::move(Error);
  if (!State.LDTR.Unusable && ((State.LDTR.Attributes & 0x9f) != 0x82 ||
                               (State.LDTR.Attributes & 0x2f00)))
    return invalid("legacy segment execution requires a loaded LDT cache");
  const auto &CS = State.Segments[unsigned(X86SegmentRegister::CS)];
  if ((CS.Selector & 3) != State.CPL)
    return invalid("legacy segment execution CPL disagrees with CS");
  return llvm::Error::success();
}

llvm::Expected<X86LegacySegmentExecutionResult>
executeX86LegacySegmentInstruction(
    X86LegacySegmentState &State, X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Instruction,
    const X86LegacySegmentExecutionOptions &Options) {
  const auto &Controls = Memory.controls();
  if (auto Error = validateX86LegacySegmentState(State, Controls))
    return std::move(Error);
  if (Options.Profile != X86LegacySegmentProfile::WordStackOffsetFirst ||
      (Options.AlignmentPriority &&
       *Options.AlignmentPriority != X86AlignmentPriority::BeforePaging &&
       *Options.AlignmentPriority != X86AlignmentPriority::AfterPaging) ||
      (Options.FullLimitChoice &&
       *Options.FullLimitChoice != X86FullSegmentLimitChoice::Fault &&
       *Options.FullLimitChoice != X86FullSegmentLimitChoice::Wrap))
    return invalid("legacy segment execution requires a bound profile");
  if (State.CPL == 3 && (Controls.CR0 & (1u << 18)) &&
      (Controls.RFlags & (1u << 18)) && !Options.AlignmentPriority)
    return invalid("legacy segment execution requires an alignment priority");
  const auto &CS = State.Segments[unsigned(X86SegmentRegister::CS)];
  const auto F = detail::decodeLegacySegmentInstruction(Instruction,
                                                        CS.Attributes & 0x4000);
  if (!F)
    return invalid("unsupported or malformed legacy segment instruction");
  SegmentExecution Execution(State, Memory, Options);
  Execution.execute(*F);
  return Execution.finish();
}

} // namespace neverd::translate
