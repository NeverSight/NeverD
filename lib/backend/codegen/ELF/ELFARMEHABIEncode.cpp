//===- ELFARMEHABIEncode.cpp - ARM EHABI record encoding -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Writes the three things an EHABI rewrite has to produce: a `prel31`
/// displacement, the opcode byte program that describes a frame, and the
/// `.ARM.extab` descriptor that carries the program together with its
/// personality routine and language data.
///
//===----------------------------------------------------------------------===//

#include "ELFARMEHABIPatchDetail.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Errc.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd {

using namespace elf_arm_ehabi_detail;

namespace {

/// The widest frame adjustment the six-bit `vsp` codes reach, and the smallest
/// the `uleb128` form starts at.  Between them lies a gap the encoding leaves
/// to two of the six-bit codes in succession.
constexpr uint64_t kMaxShortStackAdjust = 0x100;
constexpr uint64_t kMinULEBStackAdjust = 0x204;

/// ARM register numbers the opcodes name.
constexpr uint16_t kFirstMaskedRegister = 4; // r4, where the pop masks start
constexpr uint16_t kStackPointer = 13;       // r13, spelled `sp`
constexpr uint16_t kLinkRegister = 14;       // r14, spelled `lr`
constexpr uint16_t kProgramCounter = 15;     // r15, spelled `pc`

/// The most registers each family of pop opcode can name.
constexpr unsigned kMaxShortGeneralCount = 8;
constexpr unsigned kMaxDoubleCount = 16;
constexpr unsigned kMaxShortDoubleCount = 8;

/// Where the short and the two-byte double-register forms start.
constexpr uint16_t kFirstShortDouble = 8;
constexpr uint16_t kFirstHighDouble = 16;
constexpr uint16_t kMaxDoubleRegister = 31;

/// The generic descriptor has the widest opcode area: three header bytes and
/// 255 words after them.  No model can carry a longer byte program.
constexpr size_t kMaxOpcodeProgramBytes =
    kGenericOpcodeBytes + kMaxExtraWords * kWordSize;

llvm::Error opcodeError(const llvm::Twine &Message) {
  return patchError("cannot encode unwind opcodes: " + Message);
}

void pushULEB(std::vector<uint8_t> &Out, uint64_t Value) {
  do {
    uint8_t Byte = static_cast<uint8_t>(Value & 0x7F);
    Value >>= 7;
    if (Value != 0)
      Byte |= 0x80;
    Out.push_back(Byte);
  } while (Value != 0);
}

/// `00xxxxxx` and `01xxxxxx` move `vsp` by a whole number of words, at most
/// \ref kMaxShortStackAdjust at a time.  The wide `uleb128` form exists only in
/// the growing direction, so the shrinking one is spelled as repeated codes,
/// and so is the gap between the two forms.
llvm::Error encodeStackAdjust(uint64_t Offset, bool Growing,
                              std::vector<uint8_t> &Out) {
  if (Offset == 0 || (Offset % kWordSize) != 0)
    return opcodeError("stack adjustment of " + llvm::Twine(Offset) +
                       " bytes is not a whole number of words");

  const uint8_t Base = Growing ? 0x00 : 0x40;
  if (!Growing) {
    const uint64_t OpcodeCount = 1 + (Offset - 1) / kMaxShortStackAdjust;
    // One byte must remain for the final finish even when this is the only
    // operation.  Reject before the repeated short form can become a
    // user-controlled, effectively unbounded loop.
    if (Out.size() >= kMaxOpcodeProgramBytes ||
        OpcodeCount > kMaxOpcodeProgramBytes - Out.size() - 1)
      return opcodeError("stack adjustment of " + llvm::Twine(Offset) +
                         " bytes exceeds every EHABI descriptor");
  }
  while (Offset != 0) {
    if (Offset <= kMaxShortStackAdjust) {
      Out.push_back(static_cast<uint8_t>(Base | ((Offset - 4) >> 2)));
      return llvm::Error::success();
    }
    if (Growing && Offset >= kMinULEBStackAdjust) {
      Out.push_back(0xB2);
      pushULEB(Out, (Offset - kMinULEBStackAdjust) >> 2);
      return llvm::Error::success();
    }
    Out.push_back(
        static_cast<uint8_t>(Base | ((kMaxShortStackAdjust - 4) >> 2)));
    Offset -= kMaxShortStackAdjust;
  }
  return llvm::Error::success();
}

/// Pop the general-purpose registers \p Mask names, lowest first.
///
/// The two halves are separate opcodes because they were separate fields in
/// the encoding, and the low one goes first: a push stores in increasing
/// register order at increasing addresses, so r0 sits at the bottom of the
/// frame and is the first register the matching pop restores.
llvm::Error encodeGeneralPops(uint32_t Mask, std::vector<uint8_t> &Out) {
  if ((Mask >> (kProgramCounter + 1)) != 0)
    return opcodeError("a register above r15 has no pop opcode");
  if (Mask == 0)
    return opcodeError("a register pop that names no register");

  const uint32_t Low = Mask & 0x000F;
  const uint32_t High = (Mask >> kFirstMaskedRegister) & 0x0FFF;
  if (Low != 0) {
    Out.push_back(0xB1);
    Out.push_back(static_cast<uint8_t>(Low));
  }
  if (High == 0)
    return llvm::Error::success();

  const uint32_t LinkBit = uint32_t(1)
                           << (kLinkRegister - kFirstMaskedRegister);
  const uint32_t Run = High & ~LinkBit;
  const unsigned Count = static_cast<unsigned>(llvm::popcount(Run));
  const bool IsRun = Run != 0 && Count <= kMaxShortGeneralCount &&
                     Run == ((uint32_t(1) << Count) - 1);
  if (IsRun) {
    const uint8_t Form = (High & LinkBit) != 0 ? 0xA8 : 0xA0;
    Out.push_back(static_cast<uint8_t>(Form | (Count - 1)));
    return llvm::Error::success();
  }
  Out.push_back(static_cast<uint8_t>(0x80 | (High >> 8)));
  Out.push_back(static_cast<uint8_t>(High & 0xFF));
  return llvm::Error::success();
}

/// Pop the double registers \p Mask names.
///
/// `FSTMFDX` writes a spare word after the registers and `VPUSH` does not,
/// which is the whole difference between the two families of opcode and is
/// recoverable only from how far the frame moved.
llvm::Error encodeDoublePops(uint32_t Mask, uint64_t StackOffset,
                             std::vector<uint8_t> &Out) {
  if (Mask == 0)
    return opcodeError("a register pop that names no register");
  const unsigned First = static_cast<unsigned>(llvm::countr_zero(Mask));
  const unsigned Count = static_cast<unsigned>(llvm::popcount(Mask));
  if (Mask != static_cast<uint32_t>(((uint64_t(1) << Count) - 1) << First))
    return opcodeError("double registers from d" + llvm::Twine(First) +
                       " up are not popped as one run");
  if (Count > kMaxDoubleCount || First > kMaxDoubleRegister)
    return opcodeError("more double registers than one opcode can name");

  const uint64_t Registers = uint64_t(Count) * 8;
  if (StackOffset != Registers && StackOffset != Registers + kWordSize)
    return opcodeError("a double-register pop that moves the stack by " +
                       llvm::Twine(StackOffset) +
                       " bytes matches neither pop instruction");
  const bool SpareWord = StackOffset != Registers;

  if (First == kFirstShortDouble && Count <= kMaxShortDoubleCount) {
    Out.push_back(
        static_cast<uint8_t>((SpareWord ? 0xB8 : 0xD0) | (Count - 1)));
    return llvm::Error::success();
  }
  if (First < kFirstHighDouble) {
    Out.push_back(SpareWord ? 0xB3 : 0xC9);
    Out.push_back(static_cast<uint8_t>((First << 4) | (Count - 1)));
    return llvm::Error::success();
  }
  if (SpareWord)
    return opcodeError("d16 and up have no spare-word pop opcode");
  Out.push_back(0xC8);
  Out.push_back(
      static_cast<uint8_t>(((First - kFirstHighDouble) << 4) | (Count - 1)));
  return llvm::Error::success();
}

llvm::Error validatePopOperation(const UnwindOperation &Op) {
  const unsigned Count = static_cast<unsigned>(llvm::popcount(Op.RegisterMask));
  if (Count == 0)
    return opcodeError("a register pop that names no register");
  if ((Op.Kind == UnwindOperationKind::SaveRegisterPreIndexed) != (Count == 1))
    return opcodeError("a single-register pop and its register mask disagree");
  if ((Op.Kind == UnwindOperationKind::SaveRegisterPairPreIndexed) !=
      (Count > 1))
    return opcodeError("a multi-register pop and its register mask disagree");
  const uint16_t First =
      static_cast<uint16_t>(llvm::countr_zero(Op.RegisterMask));
  if (Op.Register != First)
    return opcodeError("a register pop and its lowest register disagree");
  if (Op.RegisterClass == UnwindRegisterClass::GeneralPurpose &&
      Op.StackOffset != uint64_t(Count) * kWordSize)
    return opcodeError("a general-register pop moves the stack by " +
                       llvm::Twine(Op.StackOffset) + " bytes instead of " +
                       llvm::Twine(uint64_t(Count) * kWordSize));
  return llvm::Error::success();
}

llvm::Error validateOpcodeProgram(llvm::ArrayRef<uint8_t> Opcodes) {
  if (Opcodes.empty() || Opcodes.back() != kFinishOpcode)
    return opcodeError("an opcode program has no explicit final finish");
  return llvm::Error::success();
}

llvm::Error encodeOperation(const UnwindOperation &Op,
                            std::vector<uint8_t> &Out) {
  switch (Op.Kind) {
  case UnwindOperationKind::AllocateStack:
    return encodeStackAdjust(Op.StackOffset, /*Growing=*/true, Out);
  case UnwindOperationKind::DeallocateStack:
    return encodeStackAdjust(Op.StackOffset, /*Growing=*/false, Out);
  case UnwindOperationKind::SetStackPointerFromRegister:
    // 13 is sp itself, which would say nothing, and 15 is pc, which cannot
    // hold a stack pointer.  Both are reserved as prefixes.
    if (Op.RegisterClass != UnwindRegisterClass::GeneralPurpose ||
        Op.Register > kProgramCounter || Op.Register == kStackPointer ||
        Op.Register == kProgramCounter ||
        Op.RegisterMask != (uint32_t(1) << Op.Register))
      return opcodeError("r" + llvm::Twine(Op.Register) +
                         " is not a consistent general-register stack "
                         "pointer source");
    Out.push_back(static_cast<uint8_t>(0x90 | Op.Register));
    return llvm::Error::success();
  case UnwindOperationKind::SaveRegisterPreIndexed:
  case UnwindOperationKind::SaveRegisterPairPreIndexed:
    if (llvm::Error Err = validatePopOperation(Op))
      return Err;
    switch (Op.RegisterClass) {
    case UnwindRegisterClass::GeneralPurpose:
      return encodeGeneralPops(Op.RegisterMask, Out);
    case UnwindRegisterClass::FloatingPoint:
      return encodeDoublePops(Op.RegisterMask, Op.StackOffset, Out);
    default:
      return opcodeError("a register file this target's opcodes cannot name");
    }
  case UnwindOperationKind::End:
    Out.push_back(kFinishOpcode);
    return llvm::Error::success();
  case UnwindOperationKind::Opaque:
    // The native bytes of an operation nothing could name are the one honest
    // encoding of it, and reproducing them verbatim is what lets a record read
    // out of an image survive being written back into one.
    if (Op.OperandBytes.empty())
      return opcodeError("an opaque operation that kept no native bytes");
    if (Out.size() >= kMaxOpcodeProgramBytes ||
        Op.OperandBytes.size() > kMaxOpcodeProgramBytes - Out.size() - 1)
      return opcodeError("opaque unwind bytes exceed every EHABI descriptor");
    Out.insert(Out.end(), Op.OperandBytes.begin(), Op.OperandBytes.end());
    return llvm::Error::success();
  default:
    return opcodeError("unwind operation kind " +
                       llvm::Twine(static_cast<unsigned>(Op.Kind)) +
                       " has no ARM EHABI opcode");
  }
}

/// How many words past the header an opcode program of \p Length bytes needs,
/// where the header word has room for \p FirstWordBytes of it.
llvm::Error planOpcodeWords(size_t Length, size_t FirstWordBytes,
                            uint32_t &ExtraWords) {
  const uint64_t Spilled =
      Length > FirstWordBytes ? Length - FirstWordBytes : 0;
  const uint64_t Words = (Spilled + kWordSize - 1) / kWordSize;
  if (Words > kMaxExtraWords)
    return patchError("an .ARM.extab record needs " + llvm::Twine(Words) +
                      " opcode words, more than its count field can hold");
  ExtraWords = static_cast<uint32_t>(Words);
  return llvm::Error::success();
}

/// The opcode byte at \p Index, or the `finish` a descriptor pads its last
/// word with where the program runs out before the word does.
uint8_t opcodeAt(llvm::ArrayRef<uint8_t> Opcodes, size_t Index) {
  return Index < Opcodes.size() ? Opcodes[Index] : kFinishOpcode;
}

/// Pack the \p Count opcode bytes a header word carries into its low bytes,
/// most significant first, which is the order the unwinder executes them in.
uint32_t packOpcodeBytes(llvm::ArrayRef<uint8_t> Opcodes, size_t Count) {
  uint32_t Word = 0;
  for (size_t I = 0; I < Count; ++I)
    Word |= static_cast<uint32_t>(opcodeAt(Opcodes, I))
            << (8 * (Count - 1 - I));
  return Word;
}

void pushWord(std::vector<uint8_t> &Out, uint32_t Word) {
  for (unsigned I = 0; I < kWordSize; ++I)
    Out.push_back(static_cast<uint8_t>(Word >> (8 * I)));
}

/// Append the \p Count opcode words that follow a descriptor's header, four
/// bytes to a word.
void pushOpcodeWords(std::vector<uint8_t> &Out, llvm::ArrayRef<uint8_t> Opcodes,
                     size_t FirstWordBytes, uint32_t Count) {
  for (uint32_t Word = 0; Word < Count; ++Word) {
    const size_t Base = FirstWordBytes + Word * kWordSize;
    uint32_t Value = 0;
    for (unsigned Byte = 0; Byte < kWordSize; ++Byte)
      Value = (Value << 8) | opcodeAt(Opcodes, Base + Byte);
    pushWord(Out, Value);
  }
}

} // namespace

namespace elf_arm_ehabi_detail {

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "elf arm ehabi patch: " + Message);
}

uint64_t decodePrel31(uint32_t Word, uint64_t FieldVA) {
  const int32_t Displacement = static_cast<int32_t>(Word << 1) >> 1;
  return (FieldVA + static_cast<uint64_t>(static_cast<int64_t>(Displacement))) &
         0xFFFFFFFFull;
}

llvm::Error encodePrel31(uint64_t FieldVA, uint64_t TargetVA, uint32_t &Out) {
  const uint64_t AddressLimit = uint64_t(1) << 32;
  if (FieldVA >= AddressLimit || TargetVA >= AddressLimit)
    return patchError("0x" + llvm::utohexstr(FieldVA) + " and 0x" +
                      llvm::utohexstr(TargetVA) +
                      " are not both addresses this target can hold");
  const int64_t Displacement =
      static_cast<int64_t>(TargetVA) - static_cast<int64_t>(FieldVA);
  if (Displacement < kPrel31Min || Displacement > kPrel31Max)
    return patchError("0x" + llvm::utohexstr(TargetVA) +
                      " is out of prel31 range of the field at 0x" +
                      llvm::utohexstr(FieldVA));
  Out = static_cast<uint32_t>(Displacement) & ~kCompactBit;
  return llvm::Error::success();
}

llvm::Error encodeARMEHABIIndexWord(const ELFARMEHABIRecord &Record,
                                    uint32_t &Out) {
  if (Record.Model == ELFARMEHABIModel::CantUnwind) {
    Out = kCantUnwind;
    return llvm::Error::success();
  }
  if (Record.Model != ELFARMEHABIModel::Inline)
    return patchError("an out-of-line record has no index word of its own");
  if (Record.PersonalityIndex != 0)
    return patchError(
        "only personality routine 0 fits in an .ARM.exidx index word");
  // The index word holds three opcode bytes and nothing else: no count field
  // to spill into, and no room after the opcodes for a personality's own data.
  if (Record.Opcodes.size() > kInlineOpcodeBytes || !Record.HandlerData.empty())
    return patchError("a record with " + llvm::Twine(Record.Opcodes.size()) +
                      " opcode bytes and " +
                      llvm::Twine(Record.HandlerData.size()) +
                      " bytes of handler data does not fit an index word");
  if (llvm::Error Err = validateOpcodeProgram(Record.Opcodes))
    return Err;

  Out = kCompactBit | packOpcodeBytes(Record.Opcodes, kInlineOpcodeBytes);
  return llvm::Error::success();
}

llvm::Error encodeARMEHABIDescriptor(const ELFARMEHABIRecord &Record,
                                     uint64_t DescriptorVA,
                                     std::vector<uint8_t> &Out) {
  if ((DescriptorVA % kWordSize) != 0)
    return patchError("an .ARM.extab record placed at 0x" +
                      llvm::utohexstr(DescriptorVA) + " is not word aligned");

  uint32_t ExtraWords = 0;
  size_t FirstWordBytes = 0;
  switch (Record.Model) {
  case ELFARMEHABIModel::Compact: {
    if (Record.PersonalityIndex > kMaxPersonalityIndex)
      return patchError("personality routine index " +
                        llvm::Twine(Record.PersonalityIndex) +
                        " is not one ARM defined");
    if (llvm::Error Err = validateOpcodeProgram(Record.Opcodes))
      return Err;
    // Routine 0 has no count field: its whole descriptor is the three opcode
    // bytes beside the index, so a longer program needs one of the others.
    const bool IsRoutineZero = Record.PersonalityIndex == 0;
    FirstWordBytes = IsRoutineZero ? kInlineOpcodeBytes : kCompactOpcodeBytes;
    if (IsRoutineZero && Record.Opcodes.size() > kInlineOpcodeBytes)
      return patchError("personality routine 0 has no room for " +
                        llvm::Twine(Record.Opcodes.size()) + " opcode bytes");
    if (llvm::Error Err =
            planOpcodeWords(Record.Opcodes.size(), FirstWordBytes, ExtraWords))
      return Err;

    uint32_t Word =
        kCompactBit |
        (static_cast<uint32_t>(Record.PersonalityIndex) << kCompactIndexShift) |
        packOpcodeBytes(Record.Opcodes, FirstWordBytes);
    if (!IsRoutineZero)
      Word |= ExtraWords << kCompactExtraWordShift;
    pushWord(Out, Word);
    break;
  }
  case ELFARMEHABIModel::Generic: {
    if (Record.PersonalityVA == 0)
      return patchError(
          "a generic .ARM.extab record names no personality routine");
    if (llvm::Error Err = validateOpcodeProgram(Record.Opcodes))
      return Err;
    FirstWordBytes = kGenericOpcodeBytes;
    if (llvm::Error Err =
            planOpcodeWords(Record.Opcodes.size(), FirstWordBytes, ExtraWords))
      return Err;

    uint32_t Personality = 0;
    if (llvm::Error Err =
            encodePrel31(DescriptorVA, Record.PersonalityVA, Personality))
      return Err;
    pushWord(Out, Personality);
    pushWord(Out, (ExtraWords << kGenericExtraWordShift) |
                      packOpcodeBytes(Record.Opcodes, FirstWordBytes));
    break;
  }
  default:
    return patchError("an inline record has no .ARM.extab descriptor");
  }

  pushOpcodeWords(Out, Record.Opcodes, FirstWordBytes, ExtraWords);
  Out.insert(Out.end(), Record.HandlerData.begin(), Record.HandlerData.end());
  // Whatever length of language data this record ended with, the next one
  // starts on a word boundary.
  while ((Out.size() % kWordSize) != 0)
    Out.push_back(0);
  return llvm::Error::success();
}

} // namespace elf_arm_ehabi_detail

llvm::Expected<std::vector<uint8_t>>
encodeARMEHABIUnwindOpcodes(llvm::ArrayRef<UnwindOperation> Operations) {
  if (Operations.empty() || Operations.back().Kind != UnwindOperationKind::End)
    return opcodeError("an operation sequence has no final End");
  for (const UnwindOperation &Op : Operations.drop_back())
    if (Op.Kind == UnwindOperationKind::End)
      return opcodeError("an operation follows End");

  std::vector<uint8_t> Out;
  for (const UnwindOperation &Op : Operations) {
    if (llvm::Error Err = encodeOperation(Op, Out))
      return std::move(Err);
    if (Out.size() > kMaxOpcodeProgramBytes)
      return opcodeError("an opcode program exceeds every EHABI descriptor");
  }
  return Out;
}

} // namespace neverd
