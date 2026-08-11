//===- COFFUnwindARM.cpp - ARM and ARM64 unwind code decoding -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFUnwindARM.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/ARMWinEH.h"

#include <algorithm>

namespace neverd::coff_loader {
namespace {

//===----------------------------------------------------------------------===//
// Shared helpers
//===----------------------------------------------------------------------===//

/// ARM64 register numbers the unwind codes name relative to a fixed base.
constexpr uint16_t kFirstSavedIntReg = 19; // x19
constexpr uint16_t kFramePointer = 29;     // x29
constexpr uint16_t kLinkRegister = 30;     // x30, spelled `lr`
constexpr uint16_t kFirstSavedFPReg = 8;   // d8

/// ARM32 register numbers.
constexpr uint16_t kARMFramePointer = 11; // r11
constexpr uint16_t kARMStackPointer = 13; // r13, spelled `sp`
constexpr uint16_t kARMLinkRegister = 14; // r14, spelled `lr`

/// Every ARM64 instruction is one word, and every unwind code that stands
/// against an instruction therefore stands against four bytes.
constexpr uint8_t kARM64InstructionSize = 4;

/// A register number is only representable in \ref UnwindOperation's mask if
/// it fits the 32 bits the mask has.  Every register file these codes name is
/// smaller than that, so a number that does not fit came from a malformed
/// code rather than from a register that exists.
bool isMaskableRegister(uint16_t Reg) { return Reg < 32; }

void addRegister(UnwindOperation &Op, uint16_t Reg) {
  if (isMaskableRegister(Reg))
    Op.RegisterMask |= uint32_t(1) << Reg;
}

/// Record that \p Op acts on the single register \p Reg of \p Class.
void setRegister(UnwindOperation &Op, UnwindRegisterClass Class,
                 uint16_t Reg) {
  Op.RegisterClass = Class;
  Op.Register = Reg;
  addRegister(Op, Reg);
}

/// Record that \p Op acts on \p First and \p Second, which need not be
/// adjacent: `save_lrpair` pairs a callee-saved register with `lr`.
void setRegisterPair(UnwindOperation &Op, UnwindRegisterClass Class,
                     uint16_t First, uint16_t Second) {
  Op.RegisterClass = Class;
  Op.Register = std::min(First, Second);
  addRegister(Op, First);
  addRegister(Op, Second);
}

/// Record that \p Op acts on the inclusive range [\p First, \p Last].
void setRegisterRange(UnwindOperation &Op, UnwindRegisterClass Class,
                      uint16_t First, uint16_t Last) {
  Op.RegisterClass = Class;
  Op.Register = First;
  for (uint16_t Reg = First; Reg <= Last; ++Reg)
    addRegister(Op, Reg);
}

/// Accumulates operations and the running prologue length.
class DecodeBuilder {
public:
  explicit DecodeBuilder(ARMUnwindDecode &Result) : Result(Result) {}

  UnwindOperation &add(UnwindOperationKind Kind, uint32_t CodeOffset,
                       uint8_t CodeLength, uint8_t InstructionSize) {
    UnwindOperation Op;
    Op.Kind = Kind;
    Op.CodeOffset = CodeOffset;
    Op.SlotCount = CodeLength;
    Op.InstructionSize = InstructionSize;
    Result.PrologueSize += InstructionSize;
    Result.Operations.push_back(std::move(Op));
    return Result.Operations.back();
  }

  void degrade(ExceptionParseStatus Status, std::string Message) {
    Result.Status = mergeExceptionParseStatus(Result.Status, Status);
    Result.Diagnostics.push_back(std::move(Message));
  }

private:
  ARMUnwindDecode &Result;
};

std::string atOffset(const char *What, size_t Offset) {
  return std::string(What) + " at unwind-code offset " + std::to_string(Offset);
}

//===----------------------------------------------------------------------===//
// ARM64 unwind codes
//===----------------------------------------------------------------------===//

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
      // Neither terminator stands against a prologue instruction: `end` maps
      // to the `ret` an epilogue finishes with, and a prologue has none.
      Builder.add(B0 == 0xE4u ? UnwindOperationKind::End
                              : UnwindOperationKind::EndChained,
                  Offset, 1, 0);
      Terminated = true;
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

//===----------------------------------------------------------------------===//
// ARM32 unwind codes
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Packed unwind data
//===----------------------------------------------------------------------===//

ARMUnwindDecode expandARM64PackedUnwind(uint32_t PackedWord) {
  ARMUnwindDecode Result;
  DecodeBuilder Builder(Result);
  const llvm::ARM::WinEH::RuntimeFunctionARM64 RF(
      llvm::support::ulittle32_t(0), llvm::support::ulittle32_t(PackedWord));
  const llvm::ARM::WinEH::RuntimeFunctionFlag Flag = RF.Flag();
  if (Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed &&
      Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "packed unwind expansion asked for a word that is not "
                    "packed unwind data");
    return Result;
  }

  const uint32_t RegI = RF.RegI();
  const uint32_t RegF = RF.RegF();
  const uint32_t CR = RF.CR();
  const bool HomesParameters = RF.H();
  const uint64_t FrameBytes = uint64_t(RF.FrameSize()) * 16;

  // The canonical frame is laid out as a fixed sequence of areas, and the
  // sizes below are what decide where each save lands.  `IntBytes` counts the
  // callee-saved integer area including the link register when the function
  // saves it without chaining; `FPBytes` the double area, which holds RegF+1
  // registers because a lone one cannot be encoded this way.
  uint64_t IntBytes = uint64_t(RegI) * 8;
  if (CR == 0x01)
    IntBytes += 8;
  uint64_t FPBytes = uint64_t(RegF) * 8;
  if (RegF != 0)
    FPBytes += 8;
  const uint64_t SavedBytes =
      (IntBytes + FPBytes + (HomesParameters ? 64 : 0) + 0xF) & ~uint64_t(0xF);
  if (SavedBytes > FrameBytes) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "packed unwind data saves more than its frame holds");
    return Result;
  }
  const uint64_t LocalBytes = FrameBytes - SavedBytes;

  if (RegF == 1)
    Builder.degrade(ExceptionParseStatus::Partial,
                    "packed unwind data claims a single saved double "
                    "register, which the encoding cannot express");

  auto addOp = [&](UnwindOperationKind Kind) -> UnwindOperation & {
    // Packed data has no unwind-code array to point into, so the operations
    // are numbered by position rather than by an offset that does not exist.
    return Builder.add(Kind, static_cast<uint32_t>(Result.Operations.size()), 0,
                       kARM64InstructionSize);
  };

  // Step 1: sign the return address before anything can overwrite lr.
  if (CR == 0x02) {
    UnwindOperation &Op = addOp(UnwindOperationKind::SignReturnAddress);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kLinkRegister);
  }

  // Steps 2 and 3: the callee-saved integer registers, and the link register
  // when it is saved alongside them rather than with the frame pointer.  An
  // odd count with CR==01 puts lr in the last pair instead of on its own.
  const bool LinkRegisterPairsWithLast = CR == 0x01 && (RegI % 2) == 1;
  uint64_t IntOffset = 0;
  for (uint32_t Saved = 0; Saved < RegI;) {
    const bool IsFirst = Saved == 0;
    const bool IsLast = Saved + 1 == RegI;
    const uint16_t First = static_cast<uint16_t>(kFirstSavedIntReg + Saved);
    const bool Paired = !IsLast || LinkRegisterPairsWithLast;
    UnwindOperationKind Kind;
    if (IsFirst)
      Kind = Paired ? UnwindOperationKind::SaveRegisterPairPreIndexed
                    : UnwindOperationKind::SaveRegisterPreIndexed;
    else
      Kind = Paired ? UnwindOperationKind::SaveRegisterPair
                    : UnwindOperationKind::SaveRegister;
    UnwindOperation &Op = addOp(Kind);
    if (Paired) {
      const uint16_t Second = IsLast ? kLinkRegister
                                     : static_cast<uint16_t>(First + 1);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, First, Second);
      Saved += IsLast ? 1 : 2;
    } else {
      setRegister(Op, UnwindRegisterClass::GeneralPurpose, First);
      Saved += 1;
    }
    // The first save carries the whole allocation, so the registers land at
    // the bottom of the saved area and every later save is measured from it.
    Op.StackOffset = IsFirst ? SavedBytes : IntOffset;
    IntOffset += Paired ? 16 : 8;
  }
  if (CR == 0x01 && !LinkRegisterPairsWithLast) {
    UnwindOperationKind Kind = RegI == 0
                                   ? UnwindOperationKind::SaveRegisterPreIndexed
                                   : UnwindOperationKind::SaveRegister;
    UnwindOperation &Op = addOp(Kind);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kLinkRegister);
    Op.StackOffset = RegI == 0 ? SavedBytes : IntBytes - 8;
  }

  // Step 4: the callee-saved double registers.  When nothing before them has
  // moved the stack pointer, the first of them carries the allocation.
  //
  // A zero RegF means no double register was saved at all, not one: the field
  // counts them from a base of two because the encoding has no room for the
  // single-register case, which is why it is excluded rather than rounded.
  const bool FPCarriesAllocation = RegI == 0 && CR != 0x01 && RegF != 0;
  const uint32_t SavedFPRegs = RegF == 0 ? 0 : RegF + 1;
  uint64_t FPOffset = IntBytes;
  for (uint32_t Saved = 0; Saved < SavedFPRegs;) {
    const bool IsFirst = Saved == 0;
    const bool IsLast = Saved + 1 == SavedFPRegs;
    const uint16_t First = static_cast<uint16_t>(kFirstSavedFPReg + Saved);
    UnwindOperationKind Kind;
    if (IsFirst && FPCarriesAllocation)
      Kind = IsLast ? UnwindOperationKind::SaveRegisterPreIndexed
                    : UnwindOperationKind::SaveRegisterPairPreIndexed;
    else
      Kind = IsLast ? UnwindOperationKind::SaveRegister
                    : UnwindOperationKind::SaveRegisterPair;
    UnwindOperation &Op = addOp(Kind);
    if (IsLast) {
      setRegister(Op, UnwindRegisterClass::FloatingPoint, First);
      Saved += 1;
    } else {
      setRegisterPair(Op, UnwindRegisterClass::FloatingPoint, First,
                      static_cast<uint16_t>(First + 1));
      Saved += 2;
    }
    Op.StackOffset =
        IsFirst && FPCarriesAllocation ? SavedBytes : FPOffset;
    FPOffset += IsLast ? 8 : 16;
  }

  // Step 5: homing the incoming integer parameters.  The native encoding
  // spells these as plain nops because the unwinder does not have to undo
  // them, but the stores are real and a frame reader needs to know where the
  // incoming arguments went.
  if (HomesParameters) {
    const uint64_t HomeBase = IntBytes + FPBytes;
    for (uint16_t Pair = 0; Pair < 4; ++Pair) {
      UnwindOperation &Op = addOp(UnwindOperationKind::SaveRegisterPair);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose,
                      static_cast<uint16_t>(Pair * 2),
                      static_cast<uint16_t>(Pair * 2 + 1));
      Op.StackOffset = HomeBase + uint64_t(Pair) * 16;
    }
  }

  // Step 6: the local area, and the frame chain when the function keeps one.
  // A chained frame puts `<x29,lr>` at the bottom of the locals so the chain
  // can be walked from x29 alone.
  const bool Chained = CR == 0x02 || CR == 0x03;
  auto addAllocation = [&](uint64_t Bytes) {
    UnwindOperation &Op = addOp(UnwindOperationKind::AllocateStack);
    Op.StackOffset = Bytes;
  };
  // A single `sub` reaches 4080 bytes; past that the prologue splits the
  // allocation in two rather than falling back to an `.xdata` record.
  constexpr uint64_t kMaxSingleAllocation = 4080;
  if (Chained) {
    if (LocalBytes <= 512) {
      UnwindOperation &Op =
          addOp(UnwindOperationKind::SaveRegisterPairPreIndexed);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer,
                      kLinkRegister);
      Op.StackOffset = LocalBytes;
    } else {
      if (LocalBytes > kMaxSingleAllocation) {
        addAllocation(kMaxSingleAllocation);
        addAllocation(LocalBytes - kMaxSingleAllocation);
      } else {
        addAllocation(LocalBytes);
      }
      UnwindOperation &Op = addOp(UnwindOperationKind::SaveRegisterPair);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer,
                      kLinkRegister);
      Op.StackOffset = 0;
    }
    UnwindOperation &Op = addOp(UnwindOperationKind::SetFramePointer);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer);
  } else if (LocalBytes > kMaxSingleAllocation) {
    addAllocation(kMaxSingleAllocation);
    addAllocation(LocalBytes - kMaxSingleAllocation);
  } else if (LocalBytes != 0) {
    addAllocation(LocalBytes);
  }

  return Result;
}

ARMUnwindDecode expandARM32PackedUnwind(uint32_t PackedWord) {
  ARMUnwindDecode Result;
  DecodeBuilder Builder(Result);
  const llvm::ARM::WinEH::RuntimeFunction RF(
      llvm::support::ulittle32_t(0), llvm::support::ulittle32_t(PackedWord));
  const llvm::ARM::WinEH::RuntimeFunctionFlag Flag = RF.Flag();
  if (Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed &&
      Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "packed unwind expansion asked for a word that is not "
                    "packed unwind data");
    return Result;
  }

  const bool SavesVFP = RF.R();
  const bool SavesLinkRegister = RF.L();
  const bool Chained = RF.C();
  const uint8_t Reg = RF.Reg();
  const bool PrologueFolded = llvm::ARM::WinEH::PrologueFolding(RF);

  // The encoding forbids two combinations outright, and a record that uses one
  // was not produced by a conforming toolchain -- reading it as if it were
  // would report a frame the function does not build.
  if (Chained && !SavesLinkRegister) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "chained ARM frame does not save the link register");
    return Result;
  }
  if (RF.Ret() == llvm::ARM::WinEH::ReturnType::RT_POP && !SavesLinkRegister) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "ARM frame returns by popping pc without saving the link "
                    "register");
    return Result;
  }

  auto addOp = [&](UnwindOperationKind Kind,
                   uint8_t InstructionSize) -> UnwindOperation & {
    return Builder.add(Kind, static_cast<uint32_t>(Result.Operations.size()), 0,
                       InstructionSize);
  };

  // Step 1: home the incoming integer parameters.
  if (RF.H()) {
    UnwindOperation &Op =
        addOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 2);
    setRegisterRange(Op, UnwindRegisterClass::GeneralPurpose, 0, 3);
    Op.StackOffset = 16;
  }

  // Step 2: the register push.  Which registers it covers is decided by four
  // interacting flags, and LLVM already derives exactly the mask the runtime
  // uses, including the registers a folded stack adjustment absorbs into the
  // push.
  const auto [GPRMask, VFPMask] =
      llvm::ARM::WinEH::SavedRegisterMask(RF, /*Prologue=*/true);
  if (GPRMask != 0) {
    UnwindOperation &Op =
        addOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 4);
    Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
    Op.RegisterMask = GPRMask;
    Op.Register = static_cast<uint16_t>(llvm::countr_zero(uint32_t(GPRMask)));
    Op.StackOffset = uint64_t(llvm::popcount(uint32_t(GPRMask))) * 4;
  }

  // Step 3: establish the frame chain.  A 16-bit `mov` suffices when nothing
  // but r11 and lr was pushed; otherwise r11 sits above other saves and the
  // prologue has to add its displacement.
  if (Chained) {
    const bool NeedsDisplacement = !SavesVFP || PrologueFolded;
    UnwindOperation &Op =
        addOp(NeedsDisplacement ? UnwindOperationKind::AddFramePointer
                                : UnwindOperationKind::SetFramePointer,
              NeedsDisplacement ? 4 : 2);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kARMFramePointer);
    // r11 is pushed below the link register, so the displacement is whatever
    // was pushed above it.
    if (NeedsDisplacement && GPRMask != 0) {
      const uint32_t Above =
          uint32_t(GPRMask) & ~((uint32_t(1) << (kARMFramePointer + 1)) - 1);
      Op.StackOffset = uint64_t(llvm::popcount(Above)) * 4;
    }
  }

  // Step 4: the double registers, which are always pushed after the integer
  // ones and never absorb a stack adjustment.
  if (VFPMask != 0) {
    UnwindOperation &Op =
        addOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 4);
    Op.RegisterClass = UnwindRegisterClass::FloatingPoint;
    Op.RegisterMask = VFPMask;
    Op.Register = static_cast<uint16_t>(llvm::countr_zero(VFPMask));
    Op.StackOffset = uint64_t(llvm::popcount(VFPMask)) * 8;
  } else if (SavesVFP && Reg != 7) {
    Builder.degrade(ExceptionParseStatus::Partial,
                    "packed unwind data selects the double register file but "
                    "names no registers");
  }

  // Step 5: the explicit stack adjustment, present only when the push did not
  // already absorb it.
  const uint64_t AdjustWords = llvm::ARM::WinEH::StackAdjustment(RF);
  if (AdjustWords != 0 && !PrologueFolded) {
    // The 16-bit `sub sp` form reaches 508 bytes; past that the prologue uses
    // the 32-bit `subw`.
    const uint64_t Bytes = AdjustWords * 4;
    UnwindOperation &Op =
        addOp(UnwindOperationKind::AllocateStack, Bytes <= 508 ? 2 : 4);
    Op.StackOffset = Bytes;
  }

  return Result;
}

} // namespace neverd::coff_loader
