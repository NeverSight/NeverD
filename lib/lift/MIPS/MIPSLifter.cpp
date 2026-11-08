//===- MIPSLifter.cpp - Classic MIPS integer semantics -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lift classic MIPS integer encodings. Capstone owns disassembly; raw fields
/// preserve operand roles across its aliases and pseudo-instruction names.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/MIPS/MIPSLifter.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/Support/Endian.h"

namespace neverd {

uint32_t MIPSLifter::word(const cs_insn &Instruction) const {
  if (Instruction.size != 4)
    throw UnliftedInstruction(Instruction.address, Instruction.mnemonic,
                              "MIPS requires a four-byte classic encoding");
  return archLittleEndian(Target)
             ? llvm::support::endian::read32le(Instruction.bytes)
             : llvm::support::endian::read32be(Instruction.bytes);
}

bool MIPSLifter::hasDelaySlot(uint32_t Word) {
  const unsigned Opcode = Word >> 26;
  if (Opcode == 0)
    return (Word & 63) == 8 || (Word & 63) == 9;
  if (Opcode == 1) {
    const unsigned RT = (Word >> 16) & 31;
    return RT <= 3 || (RT >= 16 && RT <= 19);
  }
  return (Opcode >= 2 && Opcode <= 7) || (Opcode >= 20 && Opcode <= 23);
}

std::optional<MIPSLifter::Encoding>
MIPSLifter::encoding(const cs_insn &Instruction) const {
  if (Mode == InstructionMode::MicroMIPS)
    return microEncoding(Instruction);
  const uint32_t Word = word(Instruction);
  if (Word >> 26 == 29) {
    if (Mode != InstructionMode::MIPSClassic)
      return std::nullopt;
    return Encoding{Word, 4}; // JALX requires admitted alternate ISA.
  }
  return Encoding{Word, hasDelaySlot(Word) ? 4u : 0u};
}

unsigned MIPSLifter::delaySlotBytes(const cs_insn &Instruction) const {
  const auto Code = encoding(Instruction);
  return Code ? Code->SlotBytes : 0;
}

LowInstructionTargetMode
MIPSLifter::controlTargetMode(const cs_insn &Instruction) const {
  const auto Code = encoding(Instruction);
  if (!Code || Mode == InstructionMode::Default)
    return LowInstructionTargetMode::Preserve;
  if (Code->Word >> 26 == 29)
    return Mode == InstructionMode::MicroMIPS
               ? LowInstructionTargetMode::MIPSClassic
               : LowInstructionTargetMode::MicroMIPS;
  if (Code->Word >> 26 == 0 &&
      ((Code->Word & 63) == 8 || (Code->Word & 63) == 9))
    return LowInstructionTargetMode::FromTargetBit0;
  return LowInstructionTargetMode::Preserve;
}

bool MIPSLifter::isReturn(const cs_insn &Instruction) const {
  const auto Code = encoding(Instruction);
  return Code && Code->Word == 0x03e00008; // JR/JRC ra
}

bool MIPSLifter::isAlignmentNop(const cs_insn &Instruction) const {
  const auto Code = encoding(Instruction);
  return Code && (Code->Word == 0 || (Instruction.size == 2 &&
                                      Code->Word == 33)); // MOVE16 zero,zero
}

bool MIPSLifter::isFunctionTerminator(const cs_insn &Instruction) const {
  const auto Code = encoding(Instruction);
  if (!Code)
    return false;
  const uint32_t Word = Code->Word;
  return Word >> 26 == 2 || (Word & 0xfc1fffff) == 8; // J / JR
}

va_t MIPSLifter::directCallTarget(const cs_insn &Instruction) const {
  const auto Code = encoding(Instruction);
  if (!Code)
    return InvalidVA;
  const uint32_t Word = Code->Word;
  // Conditional link branches require their predicate when establishing a
  // no-return call. They remain discoverable from lifted control flow.
  if (Word >> 26 != 3 && Word >> 26 != 29)
    return InvalidVA;
  const unsigned Shift =
      Mode == InstructionMode::MicroMIPS && Word >> 26 != 29 ? 1 : 2;
  const uint64_t TargetAddress = ((Instruction.address + Instruction.size) &
                                  ~((uint64_t{1} << (26 + Shift)) - 1)) |
                                 (uint64_t(Word & 0x03ffffff) << Shift);
  return isMIPS64(Target) ? TargetAddress : uint32_t(TargetAddress);
}

NdVar MIPSLifter::State::resize(NdVar Value, uint16_t Bytes, bool SignExtend) {
  if (Value.Size == Bytes)
    return Value;
  const NdVar Result = makeTemp(Bytes);
  if (Value.Size > Bytes)
    emit(NdOp::SUBBYTES, Result, {Value, NdVar::cst(0, 4)});
  else
    emit(SignExtend ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Result, {Value});
  return Result;
}

NdVar MIPSLifter::State::read(unsigned Register, uint16_t Bytes) {
  if (!Bytes)
    Bytes = Width;
  if (!Register)
    return NdVar::cst(0, Bytes);
  return resize(NdVar::reg(mipsreg::r(Register), Width), Bytes);
}

void MIPSLifter::State::requireWord(unsigned Register) {
  if (Width == 8 && Register)
    emitIntrinsic(Intrinsic::MIPSRequireWord, {}, {read(Register)});
}

void MIPSLifter::State::write(unsigned Register, NdVar Value, bool SignExtend) {
  if (!Register)
    return;
  emit(NdOp::COPY, NdVar::reg(mipsreg::r(Register), Width),
       {resize(Value, Width, SignExtend)});
}

NdVar MIPSLifter::State::binary(NdOp Opcode, NdVar A, NdVar B, uint16_t Bytes) {
  const NdVar Result = makeTemp(Bytes ? Bytes : A.Size);
  emit(Opcode, Result, {A, B});
  return Result;
}

bool MIPSLifter::liftInteger(State &S, uint32_t Word) const {
  const unsigned Opcode = Word >> 26;
  const unsigned RS = (Word >> 21) & 31;
  const unsigned RT = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31;
  const unsigned SA = (Word >> 6) & 31;
  const unsigned Function = Word & 63;
  const uint64_t Immediate = static_cast<uint64_t>(int64_t(int16_t(Word)));
  const bool Wide = S.Width == 8;

  if (Word == 0) {
    S.emit(NdOp::NOP, {}, {});
    return true;
  }
  if (Opcode == 0) {
    switch (Function) {
    case 0:    // SLL
    case 2:    // SRL / ROTR
    case 3:    // SRA
    case 4:    // SLLV
    case 6:    // SRLV / ROTRV
    case 7:    // SRAV
    case 20:   // DSLLV
    case 22:   // DSRLV / DROTRV
    case 23:   // DSRAV
    case 56:   // DSLL
    case 58:   // DSRL / DROTR
    case 59:   // DSRA
    case 60:   // DSLL32
    case 62:   // DSRL32 / DROTR32
    case 63: { // DSRA32
      const bool Double = Function >= 20;
      if (Double && !Wide)
        return false;
      const uint16_t Bytes = Double ? 8 : 4;
      const bool Variable = Function == 4 || Function == 6 || Function == 7 ||
                            Function == 20 || Function == 22 || Function == 23;
      const bool Rotate = (Function & 3) == 2 && (Variable ? SA == 1 : RS == 1);
      if ((Variable && SA != unsigned(Rotate)) ||
          (!Variable && RS != unsigned(Rotate)))
        return false;
      // SLL/SLLV accept arbitrary upper bits; right shifts and rotates do not.
      // Only the shifted value is constrained, never the variable count.
      if (!Double && (Function & 3) != 0)
        S.requireWord(RT);
      NdVar Shift = Variable
                        ? S.binary(NdOp::INT_AND, S.read(RS, Bytes),
                                   NdVar::cst(Bytes * 8 - 1, Bytes))
                        : NdVar::cst(SA + (Function >= 60 ? 32 : 0), Bytes);
      NdVar Value = S.read(RT, Bytes);
      NdOp Op = (Function & 3) == 0   ? NdOp::INT_LEFT
                : (Function & 3) == 2 ? NdOp::INT_RIGHT
                                      : NdOp::INT_ASHR;
      NdVar Result = S.binary(Op, Value, Shift);
      if (Rotate) {
        const NdVar Complement = S.binary(
            NdOp::INT_AND, S.binary(NdOp::INT_SUB, NdVar::cst(0, Bytes), Shift),
            NdVar::cst(Bytes * 8 - 1, Bytes));
        Result = S.binary(NdOp::INT_OR, Result,
                          S.binary(NdOp::INT_LEFT, Value, Complement));
      }
      S.write(RD, Result);
      return true;
    }
    case 10:   // MOVZ
    case 11: { // MOVN
      if (SA)
        return false;
      NdVar Condition =
          S.binary(Function == 10 ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                   S.read(RT), NdVar::cst(0, S.Width), 1);
      NdVar Result = S.makeTemp(S.Width);
      S.emit(NdOp::SELECT, Result, {Condition, S.read(RS), S.read(RD)});
      S.write(RD, Result);
      return true;
    }
    case 16: // MFHI
    case 18: // MFLO
      if (RS || RT || SA)
        return false;
      S.write(RD,
              NdVar::reg(Function == 16 ? mipsreg::HI : mipsreg::LO, S.Width));
      return true;
    case 17: // MTHI
    case 19: // MTLO
      if (RT || RD || SA)
        return false;
      S.emit(NdOp::COPY,
             NdVar::reg(Function == 17 ? mipsreg::HI : mipsreg::LO, S.Width),
             {S.read(RS)});
      return true;
    case 24:   // MULT
    case 25:   // MULTU
    case 28:   // DMULT
    case 29: { // DMULTU
      if (RD || SA || (Function >= 28 && !Wide))
        return false;
      const uint16_t Bytes = Function >= 28 ? 8 : 4;
      const bool Signed = (Function & 1) == 0;
      if (Bytes == 4) {
        S.requireWord(RS);
        S.requireWord(RT);
      }
      const NdVar Product = S.binary(
          NdOp::INT_MULT, S.resize(S.read(RS, Bytes), Bytes * 2, Signed),
          S.resize(S.read(RT, Bytes), Bytes * 2, Signed));
      const NdVar Low = S.resize(Product, Bytes);
      const NdVar High = S.makeTemp(Bytes);
      S.emit(NdOp::SUBBYTES, High, {Product, NdVar::cst(Bytes, 4)});
      S.emit(NdOp::COPY, NdVar::reg(mipsreg::LO, S.Width),
             {S.resize(Low, S.Width, true)});
      S.emit(NdOp::COPY, NdVar::reg(mipsreg::HI, S.Width),
             {S.resize(High, S.Width, true)});
      return true;
    }
    case 33:   // ADDU
    case 35:   // SUBU
    case 36:   // AND
    case 37:   // OR
    case 38:   // XOR
    case 39:   // NOR
    case 42:   // SLT
    case 43:   // SLTU
    case 45:   // DADDU
    case 47: { // DSUBU
      if (SA || (Function >= 45 && !Wide))
        return false;
      const bool WordOperation = Function == 33 || Function == 35;
      const uint16_t Bytes = WordOperation ? 4 : S.Width;
      if (WordOperation) {
        S.requireWord(RS);
        S.requireWord(RT);
      }
      const NdOp Op = Function == 33 || Function == 45   ? NdOp::INT_ADD
                      : Function == 35 || Function == 47 ? NdOp::INT_SUB
                      : Function == 36                   ? NdOp::INT_AND
                      : Function == 37 || Function == 39 ? NdOp::INT_OR
                      : Function == 38                   ? NdOp::INT_XOR
                      : Function == 42                   ? NdOp::INT_SLESS
                                                         : NdOp::INT_LESS;
      const bool Compare = Function == 42 || Function == 43;
      NdVar Result = S.binary(Op, S.read(RS, Bytes), S.read(RT, Bytes),
                              Compare ? 1 : Bytes);
      if (Function == 39) {
        NdVar Inverted = S.makeTemp(Bytes);
        S.emit(NdOp::INT_NOT, Inverted, {Result});
        Result = Inverted;
      }
      S.write(RD, Result, !Compare);
      return true;
    }
    default:
      return false;
    }
  }
  switch (Opcode) {
  case 9: // ADDIU (32-bit operation even on MIPS64)
    S.requireWord(RS);
    S.write(RT,
            S.binary(NdOp::INT_ADD, S.read(RS, 4), NdVar::cst(Immediate, 4)));
    return true;
  case 10: // SLTI
  case 11: // SLTIU also sign-extends its immediate before unsigned comparison
    S.write(RT,
            S.binary(Opcode == 10 ? NdOp::INT_SLESS : NdOp::INT_LESS,
                     S.read(RS), NdVar::cst(Immediate, S.Width), 1),
            false);
    return true;
  case 12: // ANDI
  case 13: // ORI
  case 14: // XORI
    S.write(RT, S.binary(Opcode == 12   ? NdOp::INT_AND
                         : Opcode == 13 ? NdOp::INT_OR
                                        : NdOp::INT_XOR,
                         S.read(RS), NdVar::cst(Word & 0xffff, S.Width)));
    return true;
  case 15: // LUI sign-extends its 32-bit result on MIPS64
    if (RS)
      return false;
    S.write(RT, NdVar::cst(uint32_t(Word << 16), 4));
    return true;
  case 25: // DADDIU
    if (!Wide)
      return false;
    S.write(RT, S.binary(NdOp::INT_ADD, S.read(RS), NdVar::cst(Immediate, 8)));
    return true;
  case 28: { // SPECIAL2: MUL, CLZ/CLO, DCLZ/DCLO
    if (Function == 2 && !SA) {
      S.requireWord(RS);
      S.requireWord(RT);
      S.write(RD, S.binary(NdOp::INT_MULT, S.read(RS, 4), S.read(RT, 4)));
      // MUL leaves both accumulators unpredictable, even when rd is zero.
      // Separate definitions let SSA discard either result independently.
      S.emitIntrinsic(Intrinsic::MIPSUnpredictable,
                      NdVar::reg(mipsreg::HI, S.Width), {});
      S.emitIntrinsic(Intrinsic::MIPSUnpredictable,
                      NdVar::reg(mipsreg::LO, S.Width), {});
      return true;
    }
    if (SA || RT != RD ||
        (Function != 32 && Function != 33 && Function != 36 &&
         Function != 37) ||
        (Function >= 36 && !Wide))
      return false;
    const uint16_t Bytes = Function >= 36 ? 8 : 4;
    if (Bytes == 4)
      S.requireWord(RS);
    NdVar Input = S.read(RS, Bytes);
    if (Function & 1) {
      NdVar Inverted = S.makeTemp(Bytes);
      S.emit(NdOp::INT_NOT, Inverted, {Input});
      Input = Inverted;
    }
    NdVar Result = S.makeTemp(Bytes);
    S.emit(NdOp::LZCOUNT, Result, {Input});
    S.write(RD, Result, false);
    return true;
  }
  case 31: // SPECIAL3 integer bitfields and lane permutations
    return liftBitfield(S, Word);
  default:
    return false;
  }
}

void MIPSLifter::lift(const cs_insn &Instruction,
                      std::vector<LowOp> &Ops) const {
  const auto Code = encoding(Instruction);
  if (!Code)
    throw UnliftedInstruction(Instruction.address, Instruction.mnemonic,
                              "unsupported MIPS encoding/profile");
  const uint32_t Word = Code->Word;
  if (Code->SlotBytes)
    throw UnliftedInstruction(Instruction.address, Instruction.mnemonic,
                              "MIPS delayed transfer requires its slot");
  const size_t Begin = Ops.size();
  if (hasDelaySlot(Word)) { // A compact JRC has no physical slot.
    liftControl(Instruction, nullptr, *Code, Ops);
    return;
  }
  State S(Instruction.address, Ops, isMIPS64(Target) ? 8 : 4, Instruction.size);
  if (liftInteger(S, Word) || liftMemory(S, Word))
    return;
  Ops.resize(Begin);
  throw UnliftedInstruction(Instruction.address, Instruction.mnemonic,
                            Instruction.op_str);
}

LowDelaySlot MIPSLifter::liftUnit(const cs_insn &Head, const cs_insn &Slot,
                                  std::vector<LowOp> &Ops) const {
  const auto Code = encoding(Head);
  if (!Code || !Code->SlotBytes || Slot.address != Head.address + Head.size ||
      (Code->SlotBytes != 1 && Slot.size != Code->SlotBytes))
    throw UnliftedInstruction(Head.address, Head.mnemonic,
                              "invalid MIPS delayed-control unit");
  return liftControl(Head, &Slot, *Code, Ops);
}

LowDelaySlot MIPSLifter::liftControl(const cs_insn &Head, const cs_insn *Slot,
                                     Encoding Code,
                                     std::vector<LowOp> &Ops) const {
  const uint32_t Word = Code.Word;
  const auto SlotCode = Slot ? encoding(*Slot) : std::nullopt;
  if (Slot &&
      (!SlotCode || hasDelaySlot(SlotCode->Word) || SlotCode->Word >> 26 == 29))
    throw UnliftedInstruction(
        Slot->address, Slot->mnemonic,
        "unsupported instruction or transfer in MIPS delay slot");
  const unsigned Opcode = Word >> 26;
  const unsigned RS = (Word >> 21) & 31;
  const unsigned RT = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31;
  const unsigned SA = (Word >> 6) & 31;
  const unsigned Function = Word & 63;
  const size_t Begin = Ops.size();
  State S(Head.address, Ops, isMIPS64(Target) ? 8 : 4, Head.size);
  const auto Address = [&](uint64_t Value) {
    return S.Width == 4 ? uint64_t(uint32_t(Value)) : Value;
  };
  const uint64_t Next = Head.address + Head.size;
  const uint64_t Resume = Address(Next + (Slot ? Slot->size : 0));
  unsigned Link = 0;
  bool Conditional = false;
  bool Likely = false;
  bool Return = false;
  NdVar Condition;
  NdVar Destination;
  if (Opcode == 0) {
    if (RT || SA || (Function == 8 && RD) ||
        (Function == 9 && RD != 0 && RD == RS &&
         !(Mode == InstructionMode::MicroMIPS && Head.size == 2)))
      throw UnliftedInstruction(Head.address, Head.mnemonic,
                                "unsupported MIPS register-transfer encoding");
    Link = Function == 9 ? RD : 0;
    Return = Function == 8 && RS == 31;
    // Snapshot before the slot and before writing a link register.
    Destination = S.makeTemp(S.Width);
    S.emit(NdOp::COPY, Destination, {S.read(RS)});
  } else if (Opcode == 2 || Opcode == 3 || Opcode == 29) {
    Link = Opcode != 2 ? 31 : 0;
    const unsigned Shift =
        Mode == InstructionMode::MicroMIPS && Opcode != 29 ? 1 : 2;
    Destination =
        NdVar::cst(Address((Next & ~((uint64_t{1} << (26 + Shift)) - 1)) |
                           (uint64_t(Word & 0x03ffffff) << Shift)),
                   S.Width);
  } else {
    Conditional = true;
    Destination = NdVar::cst(
        Address(Next + uint64_t(int64_t(int16_t(Word)) *
                                (Mode == InstructionMode::MicroMIPS ? 2 : 4))),
        S.Width);
    if (Opcode == 1) {
      Link = RT >= 16 ? 31 : 0;
      if (Link && RS == 31)
        throw UnliftedInstruction(Head.address, Head.mnemonic,
                                  "MIPS branch-and-link cannot test ra");
      Likely = (RT & 2) != 0;
      Condition =
          S.binary(NdOp::INT_SLESS, S.read(RS), NdVar::cst(0, S.Width), 1);
      if (RT & 1) {
        NdVar Inverted = S.makeTemp(1);
        S.emit(NdOp::BOOL_NOT, Inverted, {Condition});
        Condition = Inverted;
      }
    } else {
      Likely = Opcode >= 20;
      const unsigned Kind = Opcode & 3;
      if (Kind >= 2 && RT)
        throw UnliftedInstruction(Head.address, Head.mnemonic,
                                  "unsupported MIPS zero-branch encoding");
      Condition = S.binary(Kind == 0   ? NdOp::INT_EQUAL
                           : Kind == 1 ? NdOp::INT_NOTEQUAL
                                       : NdOp::INT_SLESSEQUAL,
                           S.read(RS),
                           Kind < 2 ? S.read(RT) : NdVar::cst(0, S.Width), 1);
      if (Kind == 3) {
        NdVar Inverted = S.makeTemp(1);
        S.emit(NdOp::BOOL_NOT, Inverted, {Condition});
        Condition = Inverted;
      }
    }
  }
  if (Link)
    S.write(Link,
            NdVar::cst(Resume | (Mode == InstructionMode::MicroMIPS ? 1 : 0),
                       S.Width));
  const auto GuardFalsePath = [&] {
    NdVar Inverted = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, Inverted, {Condition});
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(Resume, S.Width), Inverted});
  };
  if (Likely)
    GuardFalsePath();

  LowDelaySlot Member;
  Member.Offset = Head.size;
  Member.Size = Slot ? Slot->size : 0;
  Member.FirstOp = static_cast<uint32_t>(Ops.size() - Begin);
  const int HeadSequence = S.Seq;
  if (Slot) {
    S.Addr = Slot->address;
    S.InsnSize = Slot->size;
    S.Seq = 0;
    if (!liftInteger(S, SlotCode->Word) && !liftMemory(S, SlotCode->Word)) {
      Ops.resize(Begin);
      throw UnliftedInstruction(Slot->address, Slot->mnemonic, Slot->op_str);
    }
  }
  Member.OpCount = static_cast<uint32_t>(Ops.size() - Begin - Member.FirstOp);
  S.Addr = Head.address;
  S.InsnSize = Head.size;
  S.Seq = HeadSequence;
  if (Link) {
    if (Conditional && !Likely)
      GuardFalsePath();
    S.emit(Destination.isConst() ? NdOp::CALL : NdOp::INDIR_CALL,
           NdVar::reg(mipsreg::V0, S.Width), {Destination});
  } else if (Return) {
    S.emit(NdOp::RETURN, {}, {Destination});
  } else if (Conditional && !Likely) {
    S.emit(NdOp::COND_BR, {}, {Destination, Condition});
  } else {
    S.emit(Destination.isConst() ? NdOp::BRANCH : NdOp::INDIR_BR, {},
           {Destination});
  }
  return Member;
}

} // namespace neverd
