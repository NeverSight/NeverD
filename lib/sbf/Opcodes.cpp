//===- Opcodes.cpp - Solana SBF opcode metadata -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Opcodes.h"

#include <array>

namespace neverd::sbf {
namespace {

constexpr std::array OpcodeTable = {
#define SBF_OPCODE(NAME, ENCODING, MNEMONIC, OPERATION, OPERAND_FORM, WIDTH,   \
                   VERSION_MASK, EFFECT)                                       \
  OpcodeInfo{Opcode::NAME,                                                     \
             ENCODING,                                                         \
             MNEMONIC,                                                         \
             Operation::OPERATION,                                             \
             OperandForm::OPERAND_FORM,                                        \
             WIDTH,                                                            \
             VersionMask::VERSION_MASK,                                        \
             OpcodeEffect::EFFECT},
#include "neverd/sbf/SBFOpcodes.def"
};

} // namespace

llvm::ArrayRef<OpcodeInfo> opcodeInfos() { return OpcodeTable; }

const OpcodeInfo *getOpcodeInfo(uint8_t Encoding, Version V) {
  for (const OpcodeInfo &Info : OpcodeTable)
    if (Info.Encoding == Encoding && Info.isAvailableIn(V))
      return &Info;
  return nullptr;
}

const OpcodeInfo *getOpcodeInfo(Opcode ID) {
  for (const OpcodeInfo &Info : OpcodeTable)
    if (Info.ID == ID)
      return &Info;
  return nullptr;
}

llvm::StringRef opcodeName(Opcode ID) {
  if (const OpcodeInfo *Info = getOpcodeInfo(ID))
    return Info->Mnemonic;
  return "unknown";
}

} // namespace neverd::sbf
