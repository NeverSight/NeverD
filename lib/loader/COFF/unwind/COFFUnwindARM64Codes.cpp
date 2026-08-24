//===- COFFUnwindARM64Codes.cpp - ARM64 unwind code decoding -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFUnwindARM.h"

#include "COFFUnwindARMDetail.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd::coff_loader {
namespace {

using arm_unwind_detail::atOffset;
using arm_unwind_detail::DecodeBuilder;
using arm_unwind_detail::kARM64InstructionSize;
using arm_unwind_detail::kFirstSavedFPReg;
using arm_unwind_detail::kFirstSavedIntReg;
using arm_unwind_detail::kFramePointer;
using arm_unwind_detail::kLinkRegister;
using arm_unwind_detail::setRegister;
using arm_unwind_detail::setRegisterPair;

/// Where an operation's registers came to rest, measured from the stack
/// pointer the instruction leaves behind.  A pre-indexed store moves sp and
/// then writes at the bottom of what it just claimed, so its registers sit at
/// zero however large the decrement was.
uint64_t landingOffset(const UnwindOperation &Op) {
  switch (Op.Kind) {
  case UnwindOperationKind::SaveRegisterPreIndexed:
  case UnwindOperationKind::SaveRegisterPairPreIndexed:
    return 0;
  default:
    return Op.StackOffset;
  }
}

/// Resolve every `save_next` in \p Operations.
///
/// The code carries no operand: it means "the pair above the one saved just
/// before me, one slot up".  Because the array runs from the last prologue
/// instruction back towards the first, the instruction "just before me" in the
/// prologue is the *following* entry in the array.  A run of them therefore
/// resolves from its tail towards its head, which is why this is a separate
/// backward pass rather than something the forward decode can do in place.
void resolveSaveNextRun(std::vector<UnwindOperation> &Operations,
                        DecodeBuilder &Builder) {
  if (Operations.empty())
    return;
  for (size_t I = Operations.size(); I-- > 0;) {
    UnwindOperation &Op = Operations[I];
    if (Op.Kind != UnwindOperationKind::SaveNextPair)
      continue;
    if (I + 1 >= Operations.size()) {
      Builder.degrade(
          ExceptionParseStatus::Partial,
          atOffset("save_next is the last code in its sequence", Op.CodeOffset));
      continue;
    }
    const UnwindOperation &Anchor = Operations[I + 1];
    const bool AnchorIsPair =
        Anchor.Kind == UnwindOperationKind::SaveRegisterPair ||
        Anchor.Kind == UnwindOperationKind::SaveRegisterPairPreIndexed ||
        Anchor.Kind == UnwindOperationKind::SaveNextPair;
    if (!AnchorIsPair || Anchor.RegisterClass == UnwindRegisterClass::None) {
      Builder.degrade(ExceptionParseStatus::Partial,
                      atOffset("save_next does not extend a register-pair save",
                               Op.CodeOffset));
      continue;
    }
    // A q-register pair occupies twice the stack a pair from either of the
    // other files does, so the slot the next pair lands in depends on the file.
    const uint64_t PairBytes =
        Anchor.RegisterClass == UnwindRegisterClass::Vector ? 32 : 16;
    const uint16_t First = static_cast<uint16_t>(Anchor.Register + 2);
    setRegisterPair(Op, Anchor.RegisterClass, First,
                    static_cast<uint16_t>(First + 1));
    Op.StackOffset = landingOffset(Anchor) + PairBytes;
  }
}

/// Decode the two payload bytes of `save_any_reg` (`0xE7`).
///
/// The code was added for the Arm64EC entry thunks, which must preserve the
/// registers x64 treats as non-volatile but ARM64 does not.  It is the only
/// code that can name an arbitrary register, a full 128-bit `q` register, or a
/// register the ordinary calling convention lets a function clobber.
void decodeSaveAnyRegister(UnwindOperation &Op, uint8_t Payload0,
                           uint8_t Payload1, DecodeBuilder &Builder,
                           size_t Offset) {
  if ((Payload0 & 0x80u) != 0)
    Builder.degrade(ExceptionParseStatus::Partial,
                    atOffset("save_any_reg sets a reserved bit", Offset));

  const bool IsPair = (Payload0 & 0x40u) != 0;
  const bool PreIndexed = (Payload0 & 0x20u) != 0;
  const uint16_t Reg = Payload0 & 0x1Fu;
  const uint8_t ClassBits = Payload1 >> 6;
  const uint64_t Immediate = Payload1 & 0x3Fu;

  UnwindRegisterClass Class;
  switch (ClassBits) {
  case 0:
    Class = UnwindRegisterClass::GeneralPurpose;
    break;
  case 1:
    Class = UnwindRegisterClass::FloatingPoint;
    break;
  case 2:
    Class = UnwindRegisterClass::Vector;
    break;
  default:
    Builder.degrade(
        ExceptionParseStatus::Partial,
        atOffset("save_any_reg names a reserved register file", Offset));
    Op.Kind = UnwindOperationKind::Opaque;
    Op.OperandBytes = {Payload0, Payload1};
    return;
  }

  if (IsPair)
    setRegisterPair(Op, Class, Reg, static_cast<uint16_t>(Reg + 1));
  else
    setRegister(Op, Class, Reg);

  if (PreIndexed) {
    // A pre-indexed `save_any_reg` always scales by 16, whatever it saves,
    // because the stack pointer it decrements must stay 16-byte aligned.
    Op.Kind = IsPair ? UnwindOperationKind::SaveRegisterPairPreIndexed
                     : UnwindOperationKind::SaveRegisterPreIndexed;
    Op.StackOffset = (Immediate + 1) * 16;
    return;
  }
  // A pair, or a single q register, is stored 16 bytes wide and so is scaled
  // that way; a single x or d register is stored 8 bytes wide.
  const uint64_t Scale =
      IsPair || Class == UnwindRegisterClass::Vector ? 16 : 8;
  Op.Kind = IsPair ? UnwindOperationKind::SaveRegisterPair
                   : UnwindOperationKind::SaveRegister;
  Op.StackOffset = Immediate * Scale;
}

} // namespace

ARMUnwindDecode decodeARM64UnwindCodes(llvm::ArrayRef<uint8_t> Codes,
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
    // Every code past the first byte must fit, and a code that does not is the
    // end of what can be believed rather than of what was written.
    auto take = [&](size_t Length) -> bool {
      if (Codes.size() - Pos < Length) {
        Builder.degrade(ExceptionParseStatus::Partial,
                        atOffset("unwind code is truncated", Offset));
        return false;
      }
      Pos += Length;
      return true;
    };
    const uint8_t B1 = Codes.size() - Pos > 1 ? Codes[Pos + 1] : 0;

    if ((B0 & 0xE0u) == 0x00u) { // 000xxxxx  alloc_s
      if (!take(1))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::AllocateStack,
                                        Offset, 1, kARM64InstructionSize);
      Op.StackOffset = uint64_t(B0 & 0x1Fu) * 16;
    } else if ((B0 & 0xE0u) == 0x20u) { // 001zzzzz  save_r19r20_x
      if (!take(1))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::SaveRegisterPairPreIndexed, Offset,
                      1, kARM64InstructionSize);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose,
                      kFirstSavedIntReg, kFirstSavedIntReg + 1);
      Op.StackOffset = uint64_t(B0 & 0x1Fu) * 8;
    } else if ((B0 & 0xC0u) == 0x40u) { // 01zzzzzz  save_fplr
      if (!take(1))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegisterPair,
                                        Offset, 1, kARM64InstructionSize);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer,
                      kLinkRegister);
      Op.StackOffset = uint64_t(B0 & 0x3Fu) * 8;
    } else if ((B0 & 0xC0u) == 0x80u) { // 10zzzzzz  save_fplr_x
      if (!take(1))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::SaveRegisterPairPreIndexed, Offset,
                      1, kARM64InstructionSize);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer,
                      kLinkRegister);
      Op.StackOffset = (uint64_t(B0 & 0x3Fu) + 1) * 8;
    } else if ((B0 & 0xF8u) == 0xC0u) { // 11000xxx'xxxxxxxx  alloc_m
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::AllocateStack,
                                        Offset, 2, kARM64InstructionSize);
      Op.StackOffset = ((uint64_t(B0 & 0x07u) << 8) | B1) * 16;
    } else if ((B0 & 0xFCu) == 0xC8u) { // 110010xx'xxzzzzzz  save_regp
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegisterPair,
                                        Offset, 2, kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x03u) << 2) | (B1 >> 6));
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose,
                      kFirstSavedIntReg + X, kFirstSavedIntReg + X + 1);
      Op.StackOffset = uint64_t(B1 & 0x3Fu) * 8;
    } else if ((B0 & 0xFCu) == 0xCCu) { // 110011xx'xxzzzzzz  save_regp_x
      if (!take(2))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::SaveRegisterPairPreIndexed, Offset,
                      2, kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x03u) << 2) | (B1 >> 6));
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose,
                      kFirstSavedIntReg + X, kFirstSavedIntReg + X + 1);
      Op.StackOffset = (uint64_t(B1 & 0x3Fu) + 1) * 8;
    } else if ((B0 & 0xFCu) == 0xD0u) { // 110100xx'xxzzzzzz  save_reg
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegister,
                                        Offset, 2, kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x03u) << 2) | (B1 >> 6));
      setRegister(Op, UnwindRegisterClass::GeneralPurpose,
                  kFirstSavedIntReg + X);
      Op.StackOffset = uint64_t(B1 & 0x3Fu) * 8;
    } else if ((B0 & 0xFEu) == 0xD4u) { // 1101010x'xxxzzzzz  save_reg_x
      if (!take(2))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::SaveRegisterPreIndexed, Offset, 2,
                      kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x01u) << 3) | (B1 >> 5));
      setRegister(Op, UnwindRegisterClass::GeneralPurpose,
                  kFirstSavedIntReg + X);
      Op.StackOffset = (uint64_t(B1 & 0x1Fu) + 1) * 8;
    } else if ((B0 & 0xFEu) == 0xD6u) { // 1101011x'xxzzzzzz  save_lrpair
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegisterPair,
                                        Offset, 2, kARM64InstructionSize);
      // The pair is `<x(19+2*X), lr>`: only every second callee-saved register
      // can be paired with the link register.
      const uint16_t X = uint16_t(((B0 & 0x01u) << 2) | (B1 >> 6));
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose,
                      kFirstSavedIntReg + 2 * X, kLinkRegister);
      Op.StackOffset = uint64_t(B1 & 0x3Fu) * 8;
    } else if ((B0 & 0xFEu) == 0xD8u) { // 1101100x'xxzzzzzz  save_fregp
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegisterPair,
                                        Offset, 2, kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x01u) << 2) | (B1 >> 6));
      setRegisterPair(Op, UnwindRegisterClass::FloatingPoint,
                      kFirstSavedFPReg + X, kFirstSavedFPReg + X + 1);
      Op.StackOffset = uint64_t(B1 & 0x3Fu) * 8;
    } else if ((B0 & 0xFEu) == 0xDAu) { // 1101101x'xxzzzzzz  save_fregp_x
      if (!take(2))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::SaveRegisterPairPreIndexed, Offset,
                      2, kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x01u) << 2) | (B1 >> 6));
      setRegisterPair(Op, UnwindRegisterClass::FloatingPoint,
                      kFirstSavedFPReg + X, kFirstSavedFPReg + X + 1);
      Op.StackOffset = (uint64_t(B1 & 0x3Fu) + 1) * 8;
    } else if ((B0 & 0xFEu) == 0xDCu) { // 1101110x'xxzzzzzz  save_freg
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegister,
                                        Offset, 2, kARM64InstructionSize);
      const uint16_t X = uint16_t(((B0 & 0x01u) << 2) | (B1 >> 6));
      setRegister(Op, UnwindRegisterClass::FloatingPoint,
                  kFirstSavedFPReg + X);
      Op.StackOffset = uint64_t(B1 & 0x3Fu) * 8;
    } else if (B0 == 0xDEu) { // 11011110'xxxzzzzz  save_freg_x
      if (!take(2))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::SaveRegisterPreIndexed, Offset, 2,
                      kARM64InstructionSize);
      setRegister(Op, UnwindRegisterClass::FloatingPoint,
                  kFirstSavedFPReg + (B1 >> 5));
      Op.StackOffset = (uint64_t(B1 & 0x1Fu) + 1) * 8;
    } else if (B0 == 0xDFu) { // 11011111'zzzzzzzz  alloc_z
      if (!take(2))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::AllocateVectorLengthStack, Offset, 2,
                      kARM64InstructionSize);
      // Counted in vector lengths, which are an implementation choice rather
      // than a property of the image, so the byte size stays unknown here.
      Op.StackOffset = B1;
    } else if (B0 == 0xE0u) { // 11100000'x24  alloc_l
      if (!take(4))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::AllocateStack,
                                        Offset, 4, kARM64InstructionSize);
      Op.StackOffset = ((uint64_t(Codes[Offset + 1]) << 16) |
                        (uint64_t(Codes[Offset + 2]) << 8) |
                        uint64_t(Codes[Offset + 3])) *
                       16;
    } else if (B0 == 0xE1u) { // set_fp
      if (!take(1))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SetFramePointer,
                                        Offset, 1, kARM64InstructionSize);
      setRegister(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer);
    } else if (B0 == 0xE2u) { // 11100010'xxxxxxxx  add_fp
      if (!take(2))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::AddFramePointer,
                                        Offset, 2, kARM64InstructionSize);
      setRegister(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer);
      Op.StackOffset = uint64_t(B1) * 8;
    } else if (B0 == 0xE3u) { // nop
      if (!take(1))
        break;
      Builder.add(UnwindOperationKind::Nop, Offset, 1, kARM64InstructionSize);
    } else if (B0 == 0xE4u || B0 == 0xE5u) { // end, end_c
      if (!take(1))
        break;
      // Neither marker stands against a prologue instruction: `end` maps to
      // the `ret` an epilogue finishes with, and a prologue has none.  `end_c`
      // closes only the current chained scope; the parent scope follows in the
      // same byte-code stream and is itself terminated by `end`.
      Builder.add(B0 == 0xE4u ? UnwindOperationKind::End
                              : UnwindOperationKind::EndChained,
                  Offset, 1, 0);
      Terminated = B0 == 0xE4u;
      if (!Terminated)
        Builder.enterChainedParentScope();
    } else if (B0 == 0xE6u) { // save_next
      if (!take(1))
        break;
      // Left unresolved for now: what it names is decided by the code after
      // it, which has not been decoded yet.
      Builder.add(UnwindOperationKind::SaveNextPair, Offset, 1,
                  kARM64InstructionSize);
    } else if (B0 == 0xE7u) { // 11100111'0pxrrrrr'mmoooooo  save_any_reg
      if (!take(3))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SaveRegister,
                                        Offset, 3, kARM64InstructionSize);
      decodeSaveAnyRegister(Op, Codes[Offset + 1], Codes[Offset + 2], Builder,
                            Offset);
    } else if (B0 >= 0xE8u && B0 <= 0xECu) { // MSFT_OP_*
      if (!take(1))
        break;
      // These declare that a trap frame, machine frame, or context record
      // occupies the stack instead of an ordinary frame.  They describe a
      // whole layout rather than one instruction, so they add nothing to the
      // prologue length.
      UnwindOperation &Op = Builder.add(UnwindOperationKind::CustomStackFrame,
                                        Offset, 1, 0);
      Op.OpInfo = static_cast<uint8_t>(B0 - 0xE8u);
    } else if (B0 >= 0xF8u && B0 <= 0xFBu) {
      // The reserved multi-byte codes.  Their lengths are fixed even though
      // their meanings are not, so skipping one keeps the rest of the array
      // readable instead of losing synchronization with it.
      const size_t Length = size_t(B0 - 0xF8u) + 2;
      if (!take(Length))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset,
                      static_cast<uint8_t>(Length), kARM64InstructionSize);
      Op.OperandBytes.assign(Codes.begin() + Offset,
                             Codes.begin() + Offset + Length);
      Builder.degrade(ExceptionParseStatus::Partial,
                      atOffset("reserved multi-byte unwind code", Offset));
    } else if (B0 == 0xFCu) { // pac_sign_lr
      if (!take(1))
        break;
      UnwindOperation &Op = Builder.add(UnwindOperationKind::SignReturnAddress,
                                        Offset, 1, kARM64InstructionSize);
      setRegister(Op, UnwindRegisterClass::GeneralPurpose, kLinkRegister);
    } else {
      if (!take(1))
        break;
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset, 1, 0);
      Op.OperandBytes = {B0};
      Builder.degrade(ExceptionParseStatus::Partial,
                      atOffset("reserved unwind code", Offset));
    }
  }

  resolveSaveNextRun(Result.Operations, Builder);
  if (!Terminated && Result.Status == ExceptionParseStatus::Complete)
    Builder.degrade(ExceptionParseStatus::Partial,
                    "unwind codes end without a terminator");
  return Result;
}

} // namespace neverd::coff_loader
