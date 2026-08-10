//===- Opcodes.h - Solana SBF opcode metadata -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_OPCODES_H
#define NEVERD_SBF_OPCODES_H

#include "neverd/sbf/Version.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd::sbf {

enum class Opcode : uint16_t {
#define SBF_OPCODE(NAME, ENCODING, MNEMONIC, OPERATION, OPERAND_FORM, WIDTH,   \
                   VERSION_MASK, EFFECT)                                       \
  NAME,
#include "neverd/sbf/SBFOpcodes.def"
  Unknown,
};

enum class Operation : uint8_t {
  LoadImm,
  Load,
  Store,
  Add,
  Sub,
  Mul,
  UHighMul,
  SHighMul,
  UDiv,
  URem,
  SDiv,
  SRem,
  Or,
  And,
  Xor,
  LSh,
  RSh,
  ARSh,
  Neg,
  Mov,
  EndianLE,
  EndianBE,
  HighOr,
  Jump,
  Eq,
  Ne,
  UGt,
  UGe,
  ULt,
  ULe,
  SGt,
  SGe,
  SLt,
  SLe,
  Set,
  Call,
  CallX,
  Exit,
  Invalid,
};

enum class OperandForm : uint8_t {
  None,
  Dst,
  DstImm,
  DstSrc,
  LDDW,
  Load,
  StoreImm,
  StoreReg,
  Endian,
  Branch,
  BranchImm,
  BranchReg,
  CallImm,
  CallReg,
};

enum class OpcodeEffect : uint8_t {
  Pure = 0,
  MayFault = 1u << 0,
  ReadsMemory = 1u << 1,
  WritesMemory = 1u << 2,
  Branches = 1u << 3,
  Terminates = 1u << 4,
  Calls = 1u << 5,
  Returns = 1u << 6,

  MemoryRead = (1u << 0) | (1u << 1),
  MemoryWrite = (1u << 0) | (1u << 2),
  Branch = 1u << 3,
  Terminator = 1u << 4,
  Call = (1u << 0) | (1u << 5),
  Return = 1u << 6,
};

constexpr bool hasEffect(OpcodeEffect Set, OpcodeEffect Effect) {
  return (static_cast<uint8_t>(Set) & static_cast<uint8_t>(Effect)) != 0;
}

struct OpcodeInfo {
  Opcode ID;
  uint8_t Encoding;
  llvm::StringLiteral Mnemonic;
  Operation Op;
  OperandForm Form;
  uint8_t Width;
  VersionMask Versions;
  OpcodeEffect Effect;

  bool isAvailableIn(Version V) const { return versionInMask(V, Versions); }
  bool isBranch() const {
    return hasEffect(Effect, OpcodeEffect::Branches) ||
           hasEffect(Effect, OpcodeEffect::Terminates);
  }
  bool isConditionalBranch() const {
    return hasEffect(Effect, OpcodeEffect::Branches);
  }
  bool isCall() const { return hasEffect(Effect, OpcodeEffect::Calls); }
  bool isExit() const { return hasEffect(Effect, OpcodeEffect::Returns); }
  bool mayFault() const { return hasEffect(Effect, OpcodeEffect::MayFault); }
  bool readsMemory() const {
    return hasEffect(Effect, OpcodeEffect::ReadsMemory);
  }
  bool writesMemory() const {
    return hasEffect(Effect, OpcodeEffect::WritesMemory);
  }
  bool usesImmediate() const {
    return Form == OperandForm::DstImm || Form == OperandForm::StoreImm ||
           Form == OperandForm::BranchImm || Form == OperandForm::CallImm ||
           Form == OperandForm::Endian || Form == OperandForm::LDDW;
  }
  bool usesSourceRegister() const {
    return Form == OperandForm::DstSrc || Form == OperandForm::Load ||
           Form == OperandForm::StoreReg || Form == OperandForm::BranchReg;
  }
};

llvm::ArrayRef<OpcodeInfo> opcodeInfos();
const OpcodeInfo *getOpcodeInfo(uint8_t Encoding, Version V);
const OpcodeInfo *getOpcodeInfo(Opcode ID);
llvm::StringRef opcodeName(Opcode ID);

} // namespace neverd::sbf

#endif // NEVERD_SBF_OPCODES_H
