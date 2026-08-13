//===- SBFOpcodes.cpp - Solana SBF opcode metadata ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/runtime/SBFOpcodes.h"

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
#include "neverd/sbf/runtime/SBFOpcodes.def"
};

constexpr std::array ConcreteVersions = {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  Version::NAME,
#include "neverd/sbf/image/SBFVersions.def"
};

constexpr bool validateOpcodeTable() {
  for (size_t Index = 0; Index < OpcodeTable.size(); ++Index) {
    const OpcodeInfo &Info = OpcodeTable[Index];
    if (Info.Mnemonic.empty() || Info.Versions == VersionMask::None ||
        (Info.Width != 0 && Info.Width != kBitsPerByte &&
         Info.Width != kHalfWordBitWidth && Info.Width != kWordBitWidth &&
         Info.Width != kDoubleWordBitWidth))
      return false;
    if ((Info.Form == OperandForm::Endian) != (Info.Width == 0))
      return false;
    if ((Info.Op == Operation::Load) != Info.readsMemory() ||
        (Info.Op == Operation::Store) != Info.writesMemory())
      return false;
    if ((Info.Op == Operation::Call || Info.Op == Operation::CallX) !=
        Info.isCall())
      return false;
    if ((Info.Op == Operation::Exit) != Info.isExit())
      return false;

    for (size_t Other = Index + 1; Other < OpcodeTable.size(); ++Other) {
      const OpcodeInfo &Candidate = OpcodeTable[Other];
      if (Info.ID == Candidate.ID)
        return false;
      for (Version TheVersion : ConcreteVersions)
        if (Info.Encoding == Candidate.Encoding &&
            Info.isAvailableIn(TheVersion) &&
            Candidate.isAvailableIn(TheVersion))
          return false;
    }
  }
  return true;
}

static_assert(validateOpcodeTable(),
              "SBFOpcodes.def contains inconsistent metadata");

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
