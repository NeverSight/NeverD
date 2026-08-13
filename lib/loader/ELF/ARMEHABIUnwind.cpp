//===- ARMEHABIUnwind.cpp - ARM EHABI unwind opcode decoding --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the EHABI unwind opcode stream -- the byte program an index entry
/// or an `.ARM.extab` descriptor carries -- into the target-neutral
/// \ref UnwindOperation sequence the rest of the loader consumes.
///
//===----------------------------------------------------------------------===//

#include "ARMEHABIDetail.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::arm_ehabi {
namespace {

/// Accumulates decoded operations and their position in the opcode stream.
class OpcodeBuilder {
public:
  explicit OpcodeBuilder(std::vector<UnwindOperation> &Out) : Operations(Out) {}

  UnwindOperation &add(UnwindOperationKind Kind, size_t Offset, size_t Bytes) {
    Operations.emplace_back();
    UnwindOperation &Op = Operations.back();
    Op.Kind = Kind;
    Op.CodeOffset = static_cast<uint32_t>(Offset);
    Op.SlotCount = static_cast<uint8_t>(Bytes);
    return Op;
  }

private:
  std::vector<UnwindOperation> &Operations;
};

bool isMaskableRegister(uint16_t Reg) { return Reg < 32; }

/// Record that \p Op restores every register \p Mask names, where bit \p I of
/// the mask stands for register \p FirstRegister + I.
///
/// Every EHABI register opcode is a pop, so all of them move the stack pointer
/// by as many words as they name.  The distinction the normalized kinds draw
/// is only between one register and several, which is what tells a consumer
/// whether \ref UnwindOperation::Register alone describes the operation.
void setPoppedRegisters(UnwindOperation &Op, UnwindRegisterClass Class,
                        uint16_t FirstRegister, uint32_t Mask,
                        uint64_t SlotBytes) {
  Op.RegisterClass = Class;
  uint16_t Lowest = 0;
  bool HaveLowest = false;
  unsigned Count = 0;
  for (unsigned Bit = 0; Bit < 32; ++Bit) {
    if ((Mask & (uint32_t(1) << Bit)) == 0)
      continue;
    const uint16_t Reg = static_cast<uint16_t>(FirstRegister + Bit);
    if (!isMaskableRegister(Reg))
      continue;
    Op.RegisterMask |= uint32_t(1) << Reg;
    ++Count;
    if (!HaveLowest) {
      Lowest = Reg;
      HaveLowest = true;
    }
  }
  Op.Register = Lowest;
  Op.StackOffset = Count * SlotBytes;
  Op.Kind = Count > 1 ? UnwindOperationKind::SaveRegisterPairPreIndexed
                      : UnwindOperationKind::SaveRegisterPreIndexed;
}

/// Mask of \p Count registers starting at bit zero.
uint32_t contiguousMask(unsigned Count) {
  return Count >= 32 ? 0xFFFFFFFFu : ((uint32_t(1) << Count) - 1);
}

} // namespace

namespace detail {

bool decodeUnwindOpcodes(llvm::ArrayRef<uint8_t> Bytes,
                         std::vector<UnwindOperation> &Out,
                         bool &RefusesToUnwind) {
  OpcodeBuilder Builder(Out);
  size_t I = 0;
  const size_t N = Bytes.size();
  auto next = [&](uint8_t &Value) {
    if (I >= N)
      return false;
    Value = Bytes[I++];
    return true;
  };

  while (I < N) {
    const size_t Offset = I;
    uint8_t B0 = 0;
    if (!next(B0))
      break;

    if ((B0 & 0xC0) == 0x00) { // 00xxxxxx: vsp = vsp + (x << 2) + 4
      Builder.add(UnwindOperationKind::AllocateStack, Offset, 1).StackOffset =
          (uint64_t(B0 & 0x3F) << 2) + 4;
      continue;
    }
    if ((B0 & 0xC0) == 0x40) { // 01xxxxxx: vsp = vsp - (x << 2) - 4
      Builder.add(UnwindOperationKind::DeallocateStack, Offset, 1).StackOffset =
          (uint64_t(B0 & 0x3F) << 2) + 4;
      continue;
    }

    if ((B0 & 0xF0) == 0x80) { // 1000iiii iiiiiiii: pop r4-r15 under a mask
      uint8_t B1 = 0;
      if (!next(B1))
        return false;
      const uint32_t Mask = (uint32_t(B0 & 0x0F) << 8) | B1;
      if (Mask == 0) {
        // The one reserved encoding: an entry that says outright that the
        // frame may not be unwound through, spelled in opcodes rather than in
        // the index.
        RefusesToUnwind = true;
        UnwindOperation &Op =
            Builder.add(UnwindOperationKind::Opaque, Offset, 2);
        Op.OperandBytes = {B0, B1};
        continue;
      }
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 2);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose,
                         kFirstPoppedReg, Mask, kWordSize);
      continue;
    }

    if ((B0 & 0xF0) == 0x90) { // 1001nnnn: vsp = r[nnnn]
      const uint16_t Reg = B0 & 0x0F;
      // 13 is sp itself, which would say nothing, and 15 is pc, which cannot
      // hold a stack pointer.  Both are reserved as prefixes rather than as
      // register numbers.
      if (Reg == 13 || Reg == 15) {
        Builder.add(UnwindOperationKind::Opaque, Offset, 1).OperandBytes = {B0};
        continue;
      }
      UnwindOperation &Op = Builder.add(
          UnwindOperationKind::SetStackPointerFromRegister, Offset, 1);
      Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
      Op.Register = Reg;
      Op.RegisterMask = uint32_t(1) << Reg;
      continue;
    }

    if ((B0 & 0xF8) == 0xA0) { // 10100nnn: pop r4-r[4+nnn]
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 1);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose,
                         kFirstPoppedReg, contiguousMask((B0 & 0x07) + 1),
                         kWordSize);
      continue;
    }
    if ((B0 & 0xF8) == 0xA8) { // 10101nnn: pop r4-r[4+nnn], r14
      const uint32_t Mask = contiguousMask((B0 & 0x07) + 1) |
                            (uint32_t(1) << (kLinkRegister - kFirstPoppedReg));
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 1);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose,
                         kFirstPoppedReg, Mask, kWordSize);
      continue;
    }

    if (B0 == 0xB0) { // finish
      Builder.add(UnwindOperationKind::End, Offset, 1);
      return true;
    }

    if (B0 == 0xB1) { // 10110001 0000iiii: pop r0-r3 under a mask
      uint8_t B1 = 0;
      if (!next(B1))
        return false;
      if (B1 == 0 || (B1 & 0xF0) != 0) {
        UnwindOperation &Op =
            Builder.add(UnwindOperationKind::Opaque, Offset, 2);
        Op.OperandBytes = {B0, B1};
        continue;
      }
      UnwindOperation &Op = Builder.add(UnwindOperationKind::Opaque, Offset, 2);
      setPoppedRegisters(Op, UnwindRegisterClass::GeneralPurpose, 0, B1 & 0x0F,
                         kWordSize);
      continue;
    }

    if (B0 == 0xB2) { // 10110010 uleb128: vsp = vsp + 0x204 + (uleb << 2)
      uint64_t Value = 0;
      unsigned Shift = 0;
      bool Complete = false;
      const size_t Start = I;
      while (I < N) {
        const uint8_t Byte = Bytes[I++];
        if (Shift < 64)
          Value |= uint64_t(Byte & 0x7F) << Shift;
        Shift += 7;
        if ((Byte & 0x80) == 0) {
          Complete = true;
          break;
        }
      }
      if (!Complete)
        return false;
      Builder.add(UnwindOperationKind::AllocateStack, Offset, 1 + (I - Start))
          .StackOffset = 0x204 + (Value << 2);
      continue;
    }

    // 10110011 sssscccc and 10111nnn: pop VFP double registers with the
    // `FSTMFDX` layout, which writes a spare word after the registers.
    if (B0 == 0xB3 || (B0 & 0xF8) == 0xB8) {
      unsigned First = 8;
      unsigned Count = 0;
      size_t Bytes2 = 1;
      if (B0 == 0xB3) {
        uint8_t B1 = 0;
        if (!next(B1))
          return false;
        First = B1 >> 4;
        Count = (B1 & 0x0F) + 1;
        Bytes2 = 2;
      } else {
        Count = (B0 & 0x07) + 1;
      }
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset, Bytes2);
      setPoppedRegisters(Op, UnwindRegisterClass::FloatingPoint,
                         static_cast<uint16_t>(First), contiguousMask(Count),
                         8);
      Op.StackOffset += kWordSize;
      continue;
    }

    // 11001000/11001001 sssscccc and 11010nnn: the same pops with the `VPUSH`
    // layout, which writes no spare word.
    if (B0 == 0xC8 || B0 == 0xC9 || (B0 & 0xF8) == 0xD0) {
      unsigned First = 8;
      unsigned Count = 0;
      size_t Bytes2 = 1;
      if (B0 == 0xC8 || B0 == 0xC9) {
        uint8_t B1 = 0;
        if (!next(B1))
          return false;
        First = (B1 >> 4) + (B0 == 0xC8 ? 16u : 0u);
        Count = (B1 & 0x0F) + 1;
        Bytes2 = 2;
      } else {
        Count = (B0 & 0x07) + 1;
      }
      UnwindOperation &Op =
          Builder.add(UnwindOperationKind::Opaque, Offset, Bytes2);
      setPoppedRegisters(Op, UnwindRegisterClass::FloatingPoint,
                         static_cast<uint16_t>(First), contiguousMask(Count),
                         8);
      continue;
    }

    // 11000110 and 11000111 take a second byte; the rest of the Intel Wireless
    // MMX space and every spare encoding do not.  Both are kept verbatim: a
    // consumer that meets one is better served by the bytes than by a guess.
    if (B0 == 0xC6 || B0 == 0xC7) {
      uint8_t B1 = 0;
      if (!next(B1))
        return false;
      Builder.add(UnwindOperationKind::Opaque, Offset, 2).OperandBytes = {B0,
                                                                          B1};
      continue;
    }
    Builder.add(UnwindOperationKind::Opaque, Offset, 1).OperandBytes = {B0};
  }
  return true;
}

} // namespace detail
} // namespace neverd::arm_ehabi
