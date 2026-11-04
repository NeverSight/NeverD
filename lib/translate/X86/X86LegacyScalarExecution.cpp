//===- X86LegacyScalarExecution.cpp - Restartable protected scalars -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Stage scalar state, retain page effects and commit one final RAM store.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalar.h"

#include "llvm/ADT/APInt.h"

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::translate::detail {
namespace {

using Op = X86LegacyScalarOperation;

enum class MemoryOperation { Read, ReadForUpdate, PrepareStore, Store };

class ScalarExecution {
public:
  ScalarExecution(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                  const X86LegacyExecutionOptions &Options)
      : S(State), M(Memory), O(Options) {}

  void execute(const X86LegacyScalarInstruction &F, uint32_t Fallthrough) {
    S.EIP = Fallthrough;
    uint32_t A = 0, B = 0;
    const unsigned N = F.Bytes;
    if (F.Operation == Op::ByteSwap) {
      A = S.Core.GPR[*F.Destination.Register];
      writeRegister(*F.Destination.Register, 4,
                    (A >> 24) | ((A >> 8) & 0xff00) | ((A << 8) & 0xff0000) |
                        (A << 24));
      return;
    }
    if (F.Operation == Op::TableLookup) {
      auto Source = F.Source;
      Source.Displacement = uint8_t(S.Core.GPR[0]);
      if (read(Source, 1, B))
        writeRegister(0, 1, B);
      return;
    }
    if (F.Operation == Op::Leave) {
      writeRegister(4, stack().AddressBytes, S.Core.GPR[5]);
      if (read(stack(), N, A)) {
        adjustStack(N);
        writeRegister(5, N, A);
      }
      return;
    }
    if (F.Operation == Op::SetCondition) {
      write(F.Destination, 1, condition(F.Condition));
      return;
    }
    if (F.Operation == Op::ConditionalMove) {
      // The native source access precedes the condition, even when false.
      if (read(F.Source, N, B) && condition(F.Condition))
        writeRegister(*F.Destination.Register, N, B);
      return;
    }
    if (F.Operation == Op::Loop || F.Operation == Op::CounterZeroJump) {
      const uint32_t Mask = F.CounterBytes == 2 ? UINT16_MAX : UINT32_MAX;
      uint32_t Count = S.Core.GPR[1] & Mask;
      bool Take = !Count;
      if (F.Operation == Op::Loop) {
        Count = (Count - 1) & Mask;
        writeRegister(1, F.CounterBytes, Count);
        Take = Count &&
               (F.Condition == 2 || bool(S.EFlags & 64) == bool(F.Condition));
      }
      if (Take)
        branch(Fallthrough + *F.Source.Immediate, N);
      return;
    }
    if (F.Operation == Op::FlagsToAH || F.Operation == Op::AHToFlags) {
      constexpr uint32_t StatusMask = kX86LegacyArithmeticFlagsMask & ~0x800u;
      if (F.Operation == Op::FlagsToAH)
        writeRegister(4, 1, (S.EFlags & StatusMask) | 2);
      else
        S.EFlags =
            (S.EFlags & ~StatusMask) | ((S.Core.GPR[0] >> 8) & StatusMask);
      return;
    }
    if (F.Operation == Op::ClearFlag || F.Operation == Op::SetFlag ||
        F.Operation == Op::ComplementFlag) {
      const uint32_t Mask = *F.Source.Immediate;
      S.EFlags = F.Operation == Op::ClearFlag ? S.EFlags & ~Mask
                 : F.Operation == Op::SetFlag ? S.EFlags | Mask
                                              : S.EFlags ^ Mask;
      return;
    }
    if (F.Operation == Op::SignHigh) {
      const uint32_t Sign = uint32_t(1) << (N * 8 - 1);
      writeRegister(*F.Destination.Register, N,
                    S.Core.GPR[*F.Source.Register] & Sign ? UINT32_MAX : 0);
      return;
    }
    if (F.Operation == Op::Address) {
      writeRegister(*F.Destination.Register, N, F.Source.offset(S.Core.GPR));
      return;
    }
    if (F.Operation == Op::Move || F.Operation == Op::MoveSigned) {
      if (!read(F.Source, F.SourceBytes, B))
        return;
      if (F.Operation == Op::MoveSigned)
        B = F.SourceBytes == 1 ? int8_t(B) : int16_t(B);
      write(F.Destination, N, B);
      return;
    }
    if (F.division()) {
      divide(F);
      return;
    }
    if (F.multiplication()) {
      multiply(F);
      return;
    }
    if (F.Operation == Op::Push) {
      if (!read(F.Source, N, B))
        return;
      adjustStack(-N);
      write(stack(), N, B);
      return;
    }
    if (F.Operation == Op::Pop || F.Operation == Op::Return) {
      if (!read(stack(), N, A))
        return;
      if (F.Operation == Op::Return && !branch(A, N))
        return;
      // POP memory addresses use the incremented ESP, including when the
      // explicit address size differs from the current stack-address size.
      adjustStack(N + F.Discard);
      if (F.Operation == Op::Pop)
        write(F.Destination, N, A);
      return;
    }
    if (F.Operation == Op::Call || F.Operation == Op::Jump ||
        F.Operation == Op::ConditionalJump) {
      if (F.Operation == Op::ConditionalJump && !condition(F.Condition))
        return;
      if (!read(F.Source, N, A))
        return;
      if (F.Operation == Op::Call) {
        adjustStack(-N);
        if (!memory(stack(), N, Fallthrough, MemoryOperation::PrepareStore))
          return;
        if (O.Calls == X86LegacyCallProfile::StoreBeforeTargetLimit)
          commitStore(N, Fallthrough);
        if (branch(A + (F.Relative ? Fallthrough : 0), N) &&
            O.Calls == X86LegacyCallProfile::CheckBeforeTargetLimit)
          commitStore(N, Fallthrough);
      } else {
        branch(A + (F.Relative ? Fallthrough : 0), N);
      }
      return;
    }

    const bool Compare = F.Operation == Op::Compare || F.Operation == Op::Test;
    if (!read(F.Destination, N, A, !Compare))
      return;
    if (F.Operation == Op::Increment || F.Operation == Op::Decrement) {
      B = 1;
    } else if (F.Operation == Op::Negate) {
      B = A;
      A = 0;
    } else if (F.Operation != Op::Complement && !read(F.Source, N, B)) {
      return;
    }
    const uint32_t Mask = UINT32_MAX >> (32 - N * 8);
    const uint32_t Sign = (Mask >> 1) + 1;
    const bool Subtract =
        F.Operation == Op::Subtract || F.Operation == Op::SubBorrow ||
        F.Operation == Op::Compare || F.Operation == Op::Decrement ||
        F.Operation == Op::Negate;
    const bool CarryIn =
        (F.Operation == Op::AddCarry || F.Operation == Op::SubBorrow) &&
        (S.EFlags & 1);
    const uint64_t Right = uint64_t(B) + CarryIn;
    const uint64_t Wide = Subtract ? uint64_t(A) - Right : uint64_t(A) + Right;
    uint32_t Value = uint32_t(Wide) & Mask;
    if (F.Operation == Op::Or)
      Value = A | B;
    if (F.Operation == Op::And || F.Operation == Op::Test)
      Value = A & B;
    if (F.Operation == Op::Xor)
      Value = A ^ B;
    if (F.Operation == Op::Complement)
      Value = ~A & Mask;
    if (!Compare && !write(F.Destination, N, Value))
      return;
    if (F.Operation == Op::Complement)
      return;
    const bool Carry =
        !F.logical() && (Subtract ? uint64_t(A) < Right : Wide > Mask);
    const bool Overflow =
        !F.logical() && ((Subtract ? A ^ B : ~(A ^ B)) & (A ^ Value) & Sign);
    uint32_t Flags =
        (Carry ? 1 : 0) | (!(std::popcount(uint8_t(Value)) & 1) ? 4 : 0) |
        (F.logical() ? (*O.LogicalAuxiliaryCarry ? 16 : 0)
                     : (A ^ B ^ Value) & 16) |
        (!Value ? 64 : 0) | (Value & Sign ? 128 : 0) | (Overflow ? 0x800 : 0);
    if (F.Operation == Op::Increment || F.Operation == Op::Decrement)
      Flags = (Flags & ~1u) | (S.EFlags & 1);
    S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) | Flags;
  }

  llvm::Expected<X86MemoryAccessResult> finish() {
    if (Failure)
      return std::move(*Failure);
    return std::move(Result);
  }

private:
  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyExecutionOptions &O;
  X86MemoryAccessResult Result;
  std::optional<llvm::Error> Failure;
  std::optional<X86PagedMemory::PreparedStore> Store;

  void multiply(const X86LegacyScalarInstruction &F) {
    uint32_t Source;
    if (!read(F.Source, F.Bytes, Source))
      return;
    const unsigned Bits = F.Bytes * 8;
    const bool LowOnly = F.Operation == Op::SignedMultiplyLow;
    const bool Signed = F.Operation != Op::Multiply;
    const uint32_t Left =
        LowOnly
            ? F.MultiplyImmediate.value_or(S.Core.GPR[*F.Destination.Register])
            : S.Core.GPR[0];
    const llvm::APInt A(Bits, Left), B(Bits, Source);
    const auto Product = Signed ? A.sext(2 * Bits) * B.sext(2 * Bits)
                                : A.zext(2 * Bits) * B.zext(2 * Bits);
    const uint64_t Raw = Product.getZExtValue();
    const uint32_t Low = uint32_t(Raw) & (UINT32_MAX >> (32 - Bits));
    const bool Overflow =
        Signed ? !Product.isSignedIntN(Bits) : Product.getActiveBits() > Bits;
    if (LowOnly) {
      writeRegister(*F.Destination.Register, F.Bytes, Low);
    } else if (F.Bytes == 1) {
      writeRegister(0, 2, uint32_t(Raw));
    } else {
      writeRegister(0, F.Bytes, Low);
      writeRegister(2, F.Bytes, uint32_t(Raw >> Bits));
    }
    const uint32_t Flags =
        (Overflow ? 0x801 : 0) | (!(std::popcount(Low & 0xff) & 1) ? 4 : 0) |
        (Low & (uint32_t(1) << (Bits - 1)) ? 128 : 0) |
        (O.Multiply == X86LegacyMultiplyProfile::ProductSignParityZero && !Low
             ? 64
             : 0);
    S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) | Flags;
  }

  void divide(const X86LegacyScalarInstruction &F) {
    uint32_t Source;
    if (!read(F.Source, F.Bytes, Source))
      return;
    const unsigned Bits = F.Bytes * 8;
    const uint32_t Mask = UINT32_MAX >> (32 - Bits);
    const uint64_t Raw =
        F.Bytes == 1
            ? uint16_t(S.Core.GPR[0])
            : (uint64_t(S.Core.GPR[2] & Mask) << Bits) | (S.Core.GPR[0] & Mask);
    const bool Signed = F.Operation == Op::SignedDivide;
    const llvm::APInt Dividend(2 * Bits, Raw);
    const auto Right = Signed ? llvm::APInt(Bits, Source).sext(2 * Bits)
                              : llvm::APInt(2 * Bits, Source);
    if (!Source ||
        (Signed && Dividend.isMinSignedValue() && Right.isAllOnes())) {
      fault(0);
      return;
    }
    llvm::APInt Quotient(2 * Bits, 0), Remainder(2 * Bits, 0);
    if (Signed)
      llvm::APInt::sdivrem(Dividend, Right, Quotient, Remainder);
    else
      llvm::APInt::udivrem(Dividend, Right, Quotient, Remainder);
    if (Signed ? !Quotient.isSignedIntN(Bits)
               : Quotient.getActiveBits() > Bits) {
      fault(0);
      return;
    }
    // Both results publish only after the operand and quotient checks. AH/AL
    // can themselves supply the divisor, so all inputs were captured first.
    writeRegister(0, F.Bytes, uint32_t(Quotient.getZExtValue()));
    writeRegister(F.Bytes == 1 ? 4 : 2, F.Bytes,
                  uint32_t(Remainder.getZExtValue()));
    S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) |
               *O.DivisionArithmeticFlags;
  }

  bool fault(uint8_t Vector) {
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception = X86MemoryException{Vector, 0, {}};
    return false;
  }

  bool accept(llvm::Expected<X86MemoryAccessResult> R) {
    if (!R) {
      Failure = R.takeError();
      return false;
    }
    Result.SelfModifications.insert(Result.SelfModifications.end(),
                                    R->SelfModifications.begin(),
                                    R->SelfModifications.end());
    Result.Status = R->Status;
    Result.Exception = R->Exception;
    Result.BackingFault = R->BackingFault;
    return Result.Status == X86PageWalkStatus::Completed;
  }

  void writeRegister(unsigned Reg, unsigned Bytes, uint32_t V) {
    const unsigned Shift = Bytes == 1 && Reg >= 4 ? 8 : 0;
    auto &GPR = S.Core.GPR[Bytes == 1 ? Reg & 3 : Reg];
    const uint32_t Mask = (UINT32_MAX >> (32 - Bytes * 8)) << Shift;
    GPR = (GPR & ~Mask) | ((V << Shift) & Mask);
  }

  X86LegacyScalarOperand stack() const {
    X86LegacyScalarOperand A;
    A.MemorySegment = X86SegmentRegister::SS;
    A.AddressBytes = S.Core.Segments[2].Attributes & 0x4000 ? 4 : 2;
    A.Base = 4;
    return A;
  }

  void adjustStack(uint32_t Amount) {
    const uint32_t Mask = stack().AddressBytes == 4 ? UINT32_MAX : UINT16_MAX;
    auto &SP = S.Core.GPR[4];
    SP = (SP & ~Mask) | ((SP + Amount) & Mask);
  }

  bool branch(uint32_t Target, unsigned Bytes) {
    Target = Bytes == 2 ? uint16_t(Target) : Target;
    if (Target > S.Core.Segments[1].Limit)
      return fault(13);
    S.EIP = Target;
    return true;
  }

  bool condition(unsigned Code) const {
    const bool CF = S.EFlags & 1, PF = S.EFlags & 4, ZF = S.EFlags & 64,
               SF = S.EFlags & 128, OF = S.EFlags & 0x800;
    const bool Conditions[]{OF, CF, ZF,       CF || ZF,
                            SF, PF, SF != OF, ZF || (SF != OF)};
    return Conditions[Code >> 1] != bool(Code & 1);
  }

  void commitStore(unsigned Bytes, uint32_t V) {
    std::array<uint8_t, 4> Buffer{};
    for (unsigned I = 0; I != Bytes; ++I)
      Buffer[I] = V >> (I * 8);
    M.commitStore(*Store, llvm::ArrayRef(Buffer).take_front(Bytes));
  }

  bool memory(const X86LegacyScalarOperand &A, unsigned Bytes, uint32_t &V,
              MemoryOperation Operation) {
    if (Store && Operation == MemoryOperation::Store) {
      commitStore(Bytes, V);
      return true;
    }
    const bool Write = Operation != MemoryOperation::Read;
    const auto Segment = A.MemorySegment;
    const auto &Cache = S.Core.Segments[unsigned(Segment)];
    auto Checked = checkX86LegacySegmentAccess(
        Cache,
        {Segment,
         Write ? X86SegmentAccessKind::Write : X86SegmentAccessKind::Read,
         A.offset(S.Core.GPR), Bytes, O.Segments.FullLimitChoice});
    if (!Checked) {
      Failure = Checked.takeError();
      return false;
    }
    if (Checked->FaultVector)
      return fault(*Checked->FaultVector);
    const bool AC = S.Core.CPL == 3 && (M.controls().CR0 & 0x40000) &&
                    (S.EFlags & 0x40000) && (Checked->Linear & (Bytes - 1));
    if (AC &&
        O.Segments.AlignmentPriority == X86AlignmentPriority::BeforePaging)
      return fault(17);
    X86LinearAccess Access{Checked->Linear,
                           Write ? MemoryAccessKind::Write
                                 : MemoryAccessKind::Read,
                           S.Core.CPL,
                           false,
                           Segment == X86SegmentRegister::SS,
                           true};
    std::array<uint8_t, 4> Buffer{};
    auto Span = llvm::MutableArrayRef(Buffer).take_front(Bytes);
    if (Write) {
      auto Prepared = M.prepareStore(Access, Bytes);
      if (!Prepared) {
        Failure = Prepared.takeError();
        return false;
      }
      if (!accept(static_cast<X86MemoryAccessResult &>(*Prepared)))
        return false;
      if (AC)
        return fault(17);
      Store = std::move(*Prepared);
      if (Operation == MemoryOperation::PrepareStore)
        return true;
      if (Operation == MemoryOperation::Store) {
        commitStore(Bytes, V);
        return true;
      }
      if (!accept(M.loadPreparedStore(*Store, Span)))
        return false;
    } else if (!accept(M.load(Access, Span))) {
      return false;
    }
    if (AC)
      return fault(17);
    V = 0;
    for (unsigned I = 0; I != Bytes; ++I)
      V |= uint32_t(Buffer[I]) << (I * 8);
    return true;
  }

  bool read(const X86LegacyScalarOperand &A, unsigned Bytes, uint32_t &V,
            bool Update = false) {
    if (A.Immediate) {
      V = *A.Immediate;
    } else if (A.Register) {
      const unsigned Reg = *A.Register;
      V = S.Core.GPR[Bytes == 1 ? Reg & 3 : Reg];
      if (Bytes == 1 && Reg >= 4)
        V >>= 8;
    } else {
      return memory(A, Bytes, V,
                    Update ? MemoryOperation::ReadForUpdate
                           : MemoryOperation::Read);
    }
    V &= UINT32_MAX >> (32 - Bytes * 8);
    return true;
  }

  bool write(const X86LegacyScalarOperand &A, unsigned Bytes, uint32_t V) {
    if (!A.Register)
      return memory(A, Bytes, V, MemoryOperation::Store);
    writeRegister(*A.Register, Bytes, V);
    return true;
  }
};

} // namespace

llvm::Expected<X86MemoryAccessResult>
executeLegacyScalar(X86LegacyExecutionState &Staged, X86PagedMemory &Memory,
                    const X86LegacyScalarInstruction &Instruction,
                    uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &Options) {
  ScalarExecution Execution(Staged, Memory, Options);
  Execution.execute(Instruction, Fallthrough);
  return Execution.finish();
}

} // namespace neverd::translate::detail
