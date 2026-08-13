//===- COFFUnwindARM32Codes.cpp - ARM32 unwind code decoding -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFUnwindARM.h"

#include "COFFUnwindARMDetail.h"

#include "llvm/ADT/bit.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::coff_loader {
namespace {

using arm_unwind_detail::addRegister;
using arm_unwind_detail::atOffset;
using arm_unwind_detail::DecodeBuilder;
using arm_unwind_detail::kARMLinkRegister;
using arm_unwind_detail::setRegister;
using arm_unwind_detail::setRegisterRange;

} // namespace

ARMUnwindDecode decodeARM32UnwindCodes(llvm::ArrayRef<uint8_t> Codes,
                                       uint32_t StartOffset) {
  ARMUnwindDecode Result;
  DecodeBuilder Builder(Result);
  if (StartOffset >= Codes.size()) {
    Builder.degrade(ExceptionParseStatus::Partial,
                    "unwind-code start index " + std::to_string(StartOffset) +
                        " is past the end of a " +
                        std::to_string(Codes.size()) + "-byte code array");
    return Result;
  }

  size_t Pos = StartOffset;
  bool Terminated = false;
  while (Pos < Codes.size() && !Terminated) {
    const size_t Offset = Pos;
    const uint8_t B0 = Codes[Pos];
    auto take = [&](size_t Length) -> bool {
      if (Codes.size() - Pos < Length) {
        Builder.degrade(ExceptionParseStatus::Partial,
                        atOffset("unwind code is truncated", Offset));
        return false;
      }
      Pos += Length;
      return true;
    };
    // The trailing bytes of a multi-byte code are read most significant first,
    // so the whole code reads as one big-endian number.
    auto codeValue = [&](size_t Length) -> uint32_t {
      uint32_t Value = 0;
      for (size_t I = 0; I < Length; ++I)
        Value = (Value << 8) | Codes[Offset + I];
      return Value;
    };

    if (B0 <= 0x7Fu) { // 00-7F: add sp,sp,#(x*4)
      if (!take(1))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::AllocateStack, Offset, 1, 2);
      Op.StackOffset = uint64_t(B0 & 0x7Fu) * 4;
    } else if (B0 <= 0xBFu) { // 80-BF xx: pop {r0-r12, lr}
      if (!take(2))
        break;
      const uint32_t Code = codeValue(2);
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SaveRegisterPreIndexed, Offset, 2, 4);
      Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
      Op.RegisterMask = Code & 0x1FFFu;
      if (Code & 0x2000u)
        addRegister(Op, kARMLinkRegister);
      Op.Register = Op.RegisterMask
                        ? static_cast<uint16_t>(llvm::countr_zero(
                              Op.RegisterMask))
                        : 0;
      Op.StackOffset = uint64_t(llvm::popcount(Op.RegisterMask)) * 4;
      if (llvm::popcount(Op.RegisterMask) > 1)
        Op.Kind = UnwindOperationKind::SaveRegisterPairPreIndexed;
    } else if (B0 <= 0xCFu) { // C0-CF: mov sp,rX
      if (!take(1))
        break;
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SetStackPointerFromRegister, Offset, 1, 2);
      setRegister(Op, UnwindRegisterClass::GeneralPurpose, B0 & 0x0Fu);
    } else if (B0 <= 0xDFu) { // D0-DF: pop {r4-rX, lr}
      if (!take(1))
        break;
      // The two ranges differ only in the width of the instruction they stand
      // against, which is what decides how far into the prologue they sit.
      const bool IsWide = B0 >= 0xD8u;
      const uint16_t Last =
          static_cast<uint16_t>((B0 & 0x03u) + (IsWide ? 8 : 4));
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SaveRegisterPairPreIndexed, Offset, 1,
          IsWide ? 4 : 2);
      setRegisterRange(Op, UnwindRegisterClass::GeneralPurpose, 4, Last);
      if (B0 & 0x04u)
        addRegister(Op, kARMLinkRegister);
      Op.StackOffset = uint64_t(llvm::popcount(Op.RegisterMask)) * 4;
    } else if (B0 <= 0xE7u) { // E0-E7: vpop {d8-dX}
      if (!take(1))
        break;
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SaveRegisterPairPreIndexed, Offset, 1, 4);
      setRegisterRange(Op, UnwindRegisterClass::FloatingPoint, 8,
                       static_cast<uint16_t>((B0 & 0x07u) + 8));
      Op.StackOffset = uint64_t(llvm::popcount(Op.RegisterMask)) * 8;
    } else if (B0 <= 0xEBu) { // E8-EB xx: addw sp,sp,#(x*4)
      if (!take(2))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::AllocateStack, Offset, 2, 4);
      Op.StackOffset = uint64_t(codeValue(2) & 0x03FFu) * 4;
    } else if (B0 <= 0xEDu) { // EC-ED xx: pop {r0-r7, lr}
      if (!take(2))
        break;
      const uint32_t Code = codeValue(2);
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SaveRegisterPairPreIndexed, Offset, 2, 2);
      Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
      Op.RegisterMask = Code & 0x00FFu;
      if (Code & 0x0100u)
        addRegister(Op, kARMLinkRegister);
      Op.Register = Op.RegisterMask
                        ? static_cast<uint16_t>(llvm::countr_zero(
                              Op.RegisterMask))
                        : kARMLinkRegister;
      Op.StackOffset = uint64_t(llvm::popcount(Op.RegisterMask)) * 4;
    } else if (B0 == 0xEEu) { // EE xx
      if (!take(2))
        break;
      const uint8_t Payload = Codes[Offset + 1];
      if (Payload <= 0x0Fu) {
        UnwindOperation &Op = Builder.add(UnwindOperationKind::CustomStackFrame,
                                          Offset, 2, 2);
        Op.OpInfo = Payload;
      } else {
        UnwindOperation &Op =
            Builder.add(UnwindOperationKind::Opaque, Offset, 2, 2);
        Op.OperandBytes = {B0, Payload};
        Builder.degrade(ExceptionParseStatus::Partial,
                        atOffset("reserved unwind code", Offset));
      }
    } else if (B0 == 0xEFu) { // EF xx: ldr lr,[sp],#(x*4)
      if (!take(2))
        break;
      const uint8_t Payload = Codes[Offset + 1];
      if (Payload <= 0x0Fu) {
        UnwindOperation &Op = Builder.add(
            UnwindOperationKind::LoadReturnAddress, Offset, 2, 4);
        setRegister(Op, UnwindRegisterClass::GeneralPurpose, kARMLinkRegister);
        Op.StackOffset = uint64_t(Payload & 0x0Fu) * 4;
      } else {
        UnwindOperation &Op =
            Builder.add(UnwindOperationKind::Opaque, Offset, 2, 4);
        Op.OperandBytes = {B0, Payload};
        Builder.degrade(ExceptionParseStatus::Partial,
                        atOffset("reserved unwind code", Offset));
      }
    } else if (B0 <= 0xF4u) { // F0-F4: reserved, of unstated instruction width
      if (!take(1))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset, 1, 0);
      Op.OperandBytes = {B0};
      Builder.degrade(ExceptionParseStatus::Partial,
                      atOffset("reserved unwind code", Offset));
    } else if (B0 == 0xF5u || B0 == 0xF6u) { // vpop {dS-dE}
      if (!take(2))
        break;
      // The two codes cover the two halves of the register file: `F6` names
      // d16-d31, which only a target with 32 double registers has.
      const uint16_t Base = B0 == 0xF6u ? 16 : 0;
      const uint8_t Payload = Codes[Offset + 1];
      const uint16_t First = static_cast<uint16_t>((Payload >> 4) + Base);
      const uint16_t Last = static_cast<uint16_t>((Payload & 0x0Fu) + Base);
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SaveRegisterPairPreIndexed, Offset, 2, 4);
      if (First > Last) {
        Op.Kind = UnwindOperationKind::Opaque;
        Op.OperandBytes = {B0, Payload};
        Builder.degrade(
            ExceptionParseStatus::Partial,
            atOffset("vpop names a range that ends before it starts", Offset));
      } else {
        setRegisterRange(Op, UnwindRegisterClass::FloatingPoint, First, Last);
        Op.StackOffset = uint64_t(Last - First + 1) * 8;
      }
    } else if (B0 >= 0xF7u && B0 <= 0xFAu) { // add sp,sp,#(x*4), wide forms
      // F7 and F9 carry a 16-bit immediate, F8 and FA a 24-bit one; the pairs
      // differ only in whether the instruction they stand against is 16 or 32
      // bits wide.
      const bool IsLongImmediate = B0 == 0xF8u || B0 == 0xFAu;
      const size_t Length = IsLongImmediate ? 4 : 3;
      const uint8_t InstructionSize = B0 >= 0xF9u ? 4 : 2;
      if (!take(Length))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::AllocateStack, Offset,
                      static_cast<uint8_t>(Length), InstructionSize);
      Op.StackOffset =
          uint64_t(codeValue(Length) &
                   (IsLongImmediate ? 0x00FFFFFFu : 0x0000FFFFu)) *
          4;
    } else if (B0 == 0xFBu || B0 == 0xFCu) { // nop
      if (!take(1))
        break;
      Builder.add(UnwindOperationKind::Nop, Offset, 1, B0 == 0xFBu ? 2 : 4);
    } else if (B0 == 0xFDu || B0 == 0xFEu) { // end preceded by a nop
      if (!take(1))
        break;
      // The nop belongs to the epilogue these codes terminate; a prologue read
      // from offset zero stops here without having executed it.
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::End, Offset, 1, 0);
      Op.OpInfo = B0 == 0xFDu ? 2 : 4;
      Terminated = true;
    } else { // FF: end
      if (!take(1))
        break;
      Builder.add(UnwindOperationKind::End, Offset, 1, 0);
      Terminated = true;
    }
  }

  if (!Terminated && Result.Status == ExceptionParseStatus::Complete)
    Builder.degrade(ExceptionParseStatus::Partial,
                    "unwind codes end without a terminator");
  return Result;
}

} // namespace neverd::coff_loader
