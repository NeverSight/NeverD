//===- HighCAnalysisDispatch.cpp - Unresolved branch annotations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Show the original indirect instruction beside a tentative dispatch exit.
///
//===----------------------------------------------------------------------===//

#include "../HighCWriter.h"

#include "neverd/decode/Decoder.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

std::string HighCWriter::analysisDispatchComment(const HighExpr &Call) const {
  if (!Opts.EmitComments || !CurrentFunc || !CurrentFunc->AnalysisOnly ||
      Call.Kind != ExprKind::Call ||
      Call.CallTarget != "__neverd_analysis_indirect_jump" ||
      !Call.DoesNotReturn || Call.Operands.size() != 1)
    return {};

  std::string Text = "indirect jump";
  const va_t Address = Call.CallSiteAddr;
  // Only this rare display exit needs decoding. Ordinary calls and comment-
  // free output never initialize a decoder or inspect image bytes here.
  if (Opts.Image && Opts.TheArch == Arch::X86 &&
      Opts.Image->Arch == Arch::X86) {
    const auto *Segment = Opts.Image->getSegmentFor(Address);
    if (Segment && Segment->isExecutable() && Address >= Segment->VA &&
        Address - Segment->VA < Segment->Data.size()) {
      const size_t Offset = Address - Segment->VA;
      Decoder Decode;
      DecodedInsn Instruction{};
      if (Decode.init(*Opts.Image) &&
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
