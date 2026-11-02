//===- CAnalysisDispatch.cpp - Unresolved branch annotations --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode only an unresolved display exit, after the caller admits comments.
///
//===----------------------------------------------------------------------===//

#include "CAnalysisDispatch.h"

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

std::string analysisDispatchComment(const BinaryImage *Image, Arch TheArch,
                                    va_t Address) {
  std::string Text = "indirect jump";
  if (Image && TheArch == Arch::X86 && Image->Arch == Arch::X86) {
    const auto *Segment = Image->getSegmentFor(Address);
    if (Segment && Segment->isExecutable() && Address >= Segment->VA &&
        Address - Segment->VA < Segment->Data.size()) {
      const size_t Offset = Address - Segment->VA;
      Decoder Decode;
      DecodedInsn Instruction{};
      if (Decode.init(*Image) &&
          Decode.decodeOne(Segment->Data.data() + Offset,
                           Segment->Data.size() - Offset, Address,
                           Instruction) &&
          Instruction.Raw && Instruction.Raw->detail &&
          Instruction.Id == X86_INS_JMP) {
        const auto &X86 = Instruction.Raw->detail->x86;
        if (X86.op_count == 1 && (X86.operands[0].type == X86_OP_REG ||
                                  X86.operands[0].type == X86_OP_MEM))
          Text = std::string(Instruction.Raw->mnemonic) + " " +
                 Instruction.Raw->op_str;
      }
    }
  }
  if (Address && Address != InvalidVA)
    Text = "0x" + llvm::utohexstr(Address) + ": " + Text;
  return " /* " + Text + "; unresolved target */";
}

} // namespace neverd
