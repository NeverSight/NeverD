//===- X86LegacyScalarExecution.cpp - Restartable protected scalars -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Stage scalar state, retain page effects and commit one final RAM store.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalar.h"

#include "neverd/ir/X86/X86PopFlagsRules.h"

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
    if (F.Operation == Op::Undefined) {
      fault(6);
      return;
    }
    if (F.Operation == Op::ClearAccessControl ||
        F.Operation == Op::SetAccessControl) {
      if (S.Core.CPL || !M.features().SMAP)
        fault(6);
      else
        S.EFlags = F.Operation == Op::SetAccessControl
                       ? S.EFlags | 0x40000u
                       : S.EFlags & ~uint32_t{0x40000};
      return;
    }
    if (F.Operation == Op::StoreMachineStatus ||
        F.Operation == Op::LoadMachineStatus ||
        F.Operation == Op::ClearTaskSwitch) {
      const bool Store = F.Operation == Op::StoreMachineStatus;
      const auto &Controls = M.controls();
      if (S.Core.CPL && (!Store || (Controls.CR4 & (1u << 11)))) {
        fault(13);
        return;
      }
      if (Store)
        write(F.Destination, N, Controls.CR0);
      else if (F.Operation == Op::ClearTaskSwitch)
        Result.CR0 = Controls.CR0 & ~uint64_t{8};
      else if (read(F.Source, 2, B))
        // Admission guarantees PE=1. LMSW cannot clear it or change CR0[63:4].
        Result.CR0 = (Controls.CR0 & ~uint64_t{14}) | (B & 14);
      return;
    }
    if (F.SystemSelector) {
      using System = X86LegacySystemSelector;
      const auto Operation = *F.SystemSelector;
      const bool Load =
          Operation == System::LoadLDT || Operation == System::LoadTR;
      if (S.Core.CPL && (Load || (M.controls().CR4 & (1u << 11)))) {
        fault(13);
        return;
      }
      if (Load) {
        if (read(F.Source, 2, B))
          accept(
              loadLegacySystemSelector(S, M, Operation == System::LoadTR, B));
      } else {
        write(F.Destination, N,
              Operation == System::StoreLDT ? S.Core.LDTR.Selector
                                            : S.TR.Selector);
      }
      return;
    }
    if (F.DescriptorQuery) {
      if (!read(F.Source, 2, B))
        return;
      auto Query = queryLegacyDescriptor(S.Core, M, *F.DescriptorQuery, B);
      if (!Query) {
        Failure = Query.takeError();
        return;
      }
      if (!accept(static_cast<X86MemoryAccessResult &>(*Query)))
        return;
      if (Query->Visible && F.Destination.Register)
        writeRegister(*F.Destination.Register, N, Query->Value);
      S.EFlags = (S.EFlags & ~64u) | (Query->Visible ? 64 : 0);
      return;
    }
    if ((F.Operation == Op::CountPopulation && !O.IntegerExtensions->POPCNT) ||
        (F.carryChain() && !O.IntegerExtensions->ADX) ||
        (F.BMI && !F.BMI->enabled(*O.IntegerExtensions))) {
      fault(6);
      return;
    }
    if (F.BMI) {
      X86LegacyScalarOperand Source;
      static_cast<X86LegacyAddress &>(Source) = F.BMI->Source;
      if (read(Source, 4, B))
        executeLegacyBMI(*F.BMI, S.Core.GPR, S.EFlags, B,
                         O.BMIUndefinedFlags.value_or(0));
      return;
    }
    if (F.Adjustment) {
      decimalAdjust(*F.Adjustment);
      return;
    }
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
    if (F.Operation == Op::Bound) {
      if (!read(F.Destination, N, A) || !read(F.Source, N, B))
        return;
      const auto Signed = [N](uint32_t V) -> int32_t {
        return N == 2 ? int16_t(V) : int32_t(V);
      };
      // Original CPUs compare the lower field before accessing the upper.
      // Each field has its own address-size wrap and segment/page checks.
      if (Signed(A) < Signed(B)) {
        fault(5);
        return;
      }
      auto Upper = F.Source;
      Upper.Displacement += N;
      if (read(Upper, N, B) && Signed(A) > Signed(B))
        fault(5);
      return;
    }
    if (F.Operation == Op::AdjustPrivilege) {
      if (!read(F.Destination, 2, A) || !read(F.Source, 2, B))
        return;
      const bool Adjust = (A & 3) < (B & 3);
      // Read-only segments and pages are valid when the RPL needs no change.
      // A required write is checked only after the original word was read.
      if (Adjust && !write(F.Destination, 2, (A & ~3u) | (B & 3)))
        return;
      S.EFlags = (S.EFlags & ~64u) | (Adjust ? 64 : 0);
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
    if (F.shifting()) {
      shift(F);
      return;
    }
    if (F.doubleShifting()) {
      doubleShift(F);
      return;
    }
    if (F.bitTesting()) {
      bitTest(F);
      return;
    }
    if (F.bitCounting()) {
      if (!read(F.Source, N, B))
        return;
      const unsigned Bits = N * 8;
      const uint32_t Count = F.Operation == Op::CountPopulation
                                 ? std::popcount(B)
                             : F.Operation == Op::CountLeadingZeros
                                 ? std::countl_zero(B) - (32 - Bits)
                             : B ? std::countr_zero(B)
                                 : Bits;
      writeRegister(*F.Destination.Register, N, Count);
      const bool Population = F.Operation == Op::CountPopulation;
      S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) |
                 (Population ? 0 : *O.BitCountUndefinedFlags) |
                 (!Count ? 64 : 0) | (!Population && !B ? 1 : 0);
      return;
    }
    if (F.carryChain()) {
      if (!read(F.Destination, 4, A) || !read(F.Source, 4, B))
        return;
      const uint32_t Flag = F.Operation == Op::CarryChainOF ? 0x800 : 1;
      const uint64_t Sum = uint64_t(A) + B + bool(S.EFlags & Flag);
      writeRegister(*F.Destination.Register, 4, uint32_t(Sum));
      S.EFlags = (S.EFlags & ~Flag) | (Sum >> 32 ? Flag : 0);
      return;
    }
    if (F.bitScanning()) {
      if (!read(F.Source, N, B))
        return;
      const uint32_t Value = !B ? O.BitScanChoices->ZeroResult
                             : F.Operation == Op::BitScanForward
                                 ? std::countr_zero(B)
                                 : 31 - std::countl_zero(B);
      writeRegister(*F.Destination.Register, N, Value);
      S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) |
                 O.BitScanChoices->Flags | (!B ? 64 : 0);
      return;
    }
    if (F.Operation == Op::CompareExchangePair) {
      uint64_t Original;
      if (!memory(F.Destination, 8, Original, MemoryOperation::ReadForUpdate))
        return;
      const bool Equal =
          Original == ((uint64_t(S.Core.GPR[2]) << 32) | S.Core.GPR[0]);
      const uint64_t Value =
          Equal ? (uint64_t(S.Core.GPR[1]) << 32) | S.Core.GPR[3] : Original;
      commitStore(8, Value);
      if (!Equal) {
        S.Core.GPR[0] = uint32_t(Original);
        S.Core.GPR[2] = uint32_t(Original >> 32);
      }
      S.EFlags = (S.EFlags & ~64u) | (Equal ? 64 : 0);
      return;
    }
    if (F.Operation == Op::PushFlags) {
      adjustStack(-N);
      write(stack(), N, S.EFlags & ~uint32_t{0x30000});
      return;
    }
    if (F.Operation == Op::PopFlags) {
      if (!read(stack(), N, A))
        return;
      const auto Mask = x86PopFlagsMask(S.Core.CPL, S.EFlags, N);
      S.EFlags = (S.EFlags & ~Mask) | (A & Mask);
      adjustStack(N);
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

    const bool CompareExchange = F.Operation == Op::CompareExchange;
    const bool Compare = F.Operation == Op::Compare ||
                         F.Operation == Op::Test || CompareExchange;
    if (F.ReadSourceFirst && !read(F.Source, N, B))
      return;
    if (!read(F.Destination, N, A, !Compare || CompareExchange))
      return;
    if (F.Operation == Op::Increment || F.Operation == Op::Decrement) {
      B = 1;
    } else if (F.Operation == Op::Negate) {
      B = A;
      A = 0;
    } else if (F.Operation != Op::Complement && !F.ReadSourceFirst &&
               !read(F.Source, N, B)) {
      return;
    }
    const uint32_t Mask = UINT32_MAX >> (32 - N * 8);
    if (F.Operation == Op::Exchange) {
      if (write(F.Destination, N, B))
        writeRegister(*F.Source.Register, N, A);
      return;
    }
    if (CompareExchange) {
      const uint32_t Expected = S.Core.GPR[0] & Mask;
      // Even failure writes through the authenticated original address. The
      // accumulator and source may alias the destination or its address base.
      if (!write(F.Destination, N, Expected == A ? B : A))
        return;
      if (Expected != A)
        writeRegister(0, N, A);
      B = A;
      A = Expected;
    }
    const uint32_t Sign = (Mask >> 1) + 1;
    const bool Subtract =
        F.Operation == Op::Subtract || F.Operation == Op::SubBorrow ||
        F.Operation == Op::Compare || F.Operation == Op::Decrement ||
        F.Operation == Op::Negate || CompareExchange;
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
    // The destination wins when both XADD operands name the same register.
    // A memory destination keeps its prepared address if the source is a base.
    if (F.Operation == Op::ExchangeAdd)
      writeRegister(*F.Source.Register, N, A);
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

  llvm::Expected<X86LegacyScalarResult> finish() {
    if (Failure)
      return std::move(*Failure);
    return std::move(Result);
  }

private:
  void decimalAdjust(const X86DecimalAdjustInstruction &F) {
    using Adjust = X86DecimalAdjustOperation;
    if (F.raisesDivideError()) {
      fault(0);
      return;
    }
    const uint32_t AX = uint16_t(S.Core.GPR[0]);
    const uint8_t AL = AX, AH = AX >> 8;
    uint32_t Value = AX, Flags = *O.DecimalUndefinedFlags;
    unsigned Bytes = 2;
    if (F.Operation == Adjust::AAA || F.Operation == Adjust::AAS) {
      const bool Carry = (AL & 15) > 9 || (S.EFlags & 16);
      if (Carry)
        Value = F.Operation == Adjust::AAA ? AX + 0x106 : AX - 0x106;
      Value &= 0xff0f;
      Flags |= Carry ? 0x11 : 0;
    } else {
      if (F.Operation == Adjust::AAM) {
        Value = (uint32_t(AL / F.Base) << 8) | (AL % F.Base);
      } else if (F.Operation == Adjust::AAD) {
        Value = uint8_t(AL + AH * F.Base);
      } else {
        const bool Subtract = F.Operation == Adjust::DAS;
        const bool Low = (AL & 15) > 9 || (S.EFlags & 16);
        const bool High = AL > 0x99 || (S.EFlags & 1);
        const unsigned Delta = (Low ? 6 : 0) + (High ? 0x60 : 0);
        Value = uint8_t(Subtract ? AL - Delta : AL + Delta);
        Flags |= (Low ? 16 : 0) | (High || (Subtract && Low && AL < 6) ? 1 : 0);
        Bytes = 1;
      }
      const uint8_t Result = Value;
      Flags |= (!(std::popcount(Result) & 1) ? 4 : 0) | (!Result ? 64 : 0) |
               (Result & 0x80);
    }
    writeRegister(0, Bytes, Value);
    S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) | Flags;
  }

  X86LegacyExecutionState &S;
  X86PagedMemory &M;
  const X86LegacyExecutionOptions &O;
  X86LegacyScalarResult Result;
  std::optional<llvm::Error> Failure;
  std::optional<X86PagedMemory::PreparedStore> Store;

  void bitTest(const X86LegacyScalarInstruction &F) {
    uint32_t Index, Value;
    if (!read(F.Source, F.Bytes, Index))
      return;
    auto Destination = F.Destination;
    if (!Destination.Register && !F.Source.Immediate) {
      const int32_t SignedIndex =
          F.Bytes == 2 ? int32_t(int16_t(Index)) : int32_t(Index);
      // Floor division selects the preceding word for a negative bit index.
      // The final effective offset still wraps at the address-size boundary.
      Destination.Displacement +=
          (SignedIndex >> (F.Bytes == 2 ? 4 : 5)) * int32_t(F.Bytes);
    }
    const bool Update = F.Operation != Op::BitTest;
    if (!read(Destination, F.Bytes, Value, Update))
      return;
    const uint32_t Bit = uint32_t(1) << (Index & (F.Bytes * 8 - 1));
    const bool Carry = Value & Bit;
    if (F.Operation == Op::BitSet)
      Value |= Bit;
    else if (F.Operation == Op::BitReset)
      Value &= ~Bit;
    else if (F.Operation == Op::BitComplement)
      Value ^= Bit;
    if (Update && !write(Destination, F.Bytes, Value))
      return;
    S.EFlags = (S.EFlags & ~(kX86LegacyBitTestUndefinedFlagsMask | 1u)) |
               *O.BitTestUndefinedFlags | unsigned(Carry);
  }

  void doubleShift(const X86LegacyScalarInstruction &F) {
    const unsigned Count = F.ShiftImmediate.value_or(S.Core.GPR[1]) & 31;
    uint32_t A, B;
    if (!read(F.Source, F.Bytes, B) || !read(F.Destination, F.Bytes, A, true))
      return;
    const unsigned Bits = F.Bytes * 8;
    const uint32_t Mask = UINT32_MAX >> (32 - Bits);
    const uint32_t Sign = (Mask >> 1) + 1;
    uint32_t Value = A, Flags = S.EFlags & kX86LegacyArithmeticFlagsMask;
    if (Count > Bits) {
      Value = O.DoubleShiftUndefined->Result;
      Flags = O.DoubleShiftUndefined->Flags;
    } else if (Count) {
      const bool Left = F.Operation == Op::DoubleShiftLeft;
      const uint64_t Pair =
          Left ? (uint64_t(A) << Bits) | B : (uint64_t(B) << Bits) | A;
      Value = (Pair >> (Left ? Bits - Count : Count)) & Mask;
      const bool Carry = (A >> (Left ? Bits - Count : Count - 1)) & 1;
      Flags = unsigned(Carry) | (!(std::popcount(uint8_t(Value)) & 1) ? 4 : 0) |
              (O.DoubleShiftUndefined->Flags & 16) | (!Value ? 64 : 0) |
              (Value & Sign ? 128 : 0) |
              (Count == 1 ? ((A ^ Value) & Sign ? 0x800 : 0)
                          : O.DoubleShiftUndefined->Flags & 0x800);
    }
    if (write(F.Destination, F.Bytes, Value))
      S.EFlags = (S.EFlags & ~kX86LegacyArithmeticFlagsMask) | Flags;
  }

  void shift(const X86LegacyScalarInstruction &F) {
    uint32_t Count, Value;
    if (!read(F.Source, 1, Count) || !read(F.Destination, F.Bytes, Value, true))
      return;
    Count &= 31;
    const unsigned Bits = F.Bytes * 8;
    const uint32_t Mask = UINT32_MAX >> (32 - Bits);
    const uint32_t Sign = (Mask >> 1) + 1;
    const uint32_t Original = Value;
    const bool Left = F.Operation == Op::ShiftLeft ||
                      F.Operation == Op::RotateLeft ||
                      F.Operation == Op::RotateCarryLeft;
    const bool ThroughCarry = F.Operation == Op::RotateCarryLeft ||
                              F.Operation == Op::RotateCarryRight;
    const bool Rotate = ThroughCarry || F.Operation == Op::RotateLeft ||
                        F.Operation == Op::RotateRight;
    const unsigned Steps =
        Rotate ? Count % (Bits + unsigned(ThroughCarry)) : Count;
    bool Carry = S.EFlags & 1;
    // Each step stays within the operand width, including byte counts beyond
    // that width. No C++ shift is equal to or larger than its carrier width.
    for (unsigned I = 0; I != Steps; ++I) {
      const bool NextCarry = Value & (Left ? Sign : 1);
      const bool Fill =
          ThroughCarry ? Carry
          : Rotate     ? NextCarry
                       : F.Operation == Op::SignedShiftRight && (Value & Sign);
      Value = Left ? ((Value << 1) | unsigned(Fill)) & Mask
                   : (Value >> 1) | (Fill ? Sign : 0);
      Carry = NextCarry;
    }
    // Zero counts still authenticate the read/write operand and can fault.
    if (!write(F.Destination, F.Bytes, Value) || !Count)
      return;
    if (Rotate && !ThroughCarry)
      Carry = Value & (Left ? 1 : Sign);
    uint32_t Undefined = Count == 1 ? 0 : 0x800;
    uint32_t Changed = 0x801;
    if (!Rotate) {
      Undefined |= 16;
      if (F.Operation != Op::SignedShiftRight && Count >= Bits)
        Undefined |= 1;
      Changed = kX86LegacyArithmeticFlagsMask;
    }
    const bool Overflow = Left ? bool(Value & Sign) != Carry
                          : F.Operation == Op::SignedShiftRight ? false
                          : F.Operation == Op::ShiftRight
                              ? bool(Original & Sign)
                          : F.Operation == Op::RotateCarryRight
                              ? bool(Original & Sign) != bool(S.EFlags & 1)
                              : bool(Value & Sign) != bool(Value & (Sign >> 1));
    uint32_t Flags = (Carry ? 1 : 0) | (Overflow ? 0x800 : 0);
    if (!Rotate)
      Flags |= (!(std::popcount(uint8_t(Value)) & 1) ? 4 : 0) |
               (!Value ? 64 : 0) | (Value & Sign ? 128 : 0);
    Flags = (Flags & ~Undefined) | (*O.ShiftUndefinedFlags & Undefined);
    S.EFlags = (S.EFlags & ~Changed) | Flags;
  }

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
    Result.TakenBranch = true;
    return true;
  }

  bool condition(unsigned Code) const {
    const bool CF = S.EFlags & 1, PF = S.EFlags & 4, ZF = S.EFlags & 64,
               SF = S.EFlags & 128, OF = S.EFlags & 0x800;
    const bool Conditions[]{OF, CF, ZF,       CF || ZF,
                            SF, PF, SF != OF, ZF || (SF != OF)};
    return Conditions[Code >> 1] != bool(Code & 1);
  }

  void commitStore(unsigned Bytes, uint64_t V) {
    std::array<uint8_t, 8> Buffer{};
    for (unsigned I = 0; I != Bytes; ++I)
      Buffer[I] = V >> (I * 8);
    M.commitStore(*Store, llvm::ArrayRef(Buffer).take_front(Bytes));
  }

  template <typename UInt>
  bool memory(const X86LegacyScalarOperand &A, unsigned Bytes, UInt &V,
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
    std::array<uint8_t, 8> Buffer{};
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
      V |= UInt(Buffer[I]) << (I * 8);
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

llvm::Expected<X86LegacyScalarResult>
executeLegacyScalar(X86LegacyExecutionState &Staged, X86PagedMemory &Memory,
                    const X86LegacyScalarInstruction &Instruction,
                    uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &Options) {
  ScalarExecution Execution(Staged, Memory, Options);
  Execution.execute(Instruction, Fallthrough);
  return Execution.finish();
}

} // namespace neverd::translate::detail
