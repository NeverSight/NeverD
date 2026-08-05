//===- PipelineDump.cpp - IR dump helpers ---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Debug dump routines for LowIR, MedIR, and HighIR.
///
//===----------------------------------------------------------------------===//

#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

namespace {

std::string joinInts(const std::vector<int> &V, const char *Sep) {
  std::string S;
  for (size_t I = 0; I < V.size(); ++I) {
    if (I > 0)
      S += Sep;
    S += std::to_string(V[I]);
  }
  return S;
}

const char *vnodeSpaceAbbrev(VnodeSpace S) {
  switch (S) {
  case VnodeSpace::REG:
    return "reg";
  case VnodeSpace::TEMP:
    return "tmp";
  case VnodeSpace::CONST:
    return "cst";
  case VnodeSpace::RAM:
    return "ram";
  case VnodeSpace::STACK:
    return "stk";
  }
  return "?";
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// IR dump helpers
//===----------------------------------------------------------------------===//

void Pipeline::dumpLowIR(const std::vector<LowFunc> &Funcs) {
  llvm::outs() << "\n=== LowIR Dump ===\n";
  for (auto &LF : Funcs) {
    llvm::outs() << "func " << LF.Name << " @ 0x" << llvm::utohexstr(LF.Entry)
                 << " (" << LF.Blocks.size() << " blocks)\n";
    for (auto &Blk : LF.Blocks) {
      llvm::outs() << "  block " << Blk.Id << " [0x"
                   << llvm::utohexstr(Blk.StartAddr) << " - 0x"
                   << llvm::utohexstr(Blk.EndAddr) << "] succs=["
                   << joinInts(Blk.Succs, ",") << "] preds=["
                   << joinInts(Blk.Preds, ",") << "]\n";
      for (auto &Op : Blk.Ops) {
        llvm::outs() << "    [0x" << llvm::utohexstr(Op.Addr) << "." << Op.Seq
                     << "] " << ndOpName(Op.Opcode) << " ";
        if (Op.Output.Size > 0)
          llvm::outs() << vnodeSpaceAbbrev(Op.Output.Space) << ":0x"
                       << llvm::utohexstr(Op.Output.Offset) << ":"
                       << Op.Output.Size;
        for (uint8_t J = 0; J < Op.NumInputs; ++J)
          llvm::outs() << " " << vnodeSpaceAbbrev(Op.Inputs[J].Space) << ":0x"
                       << llvm::utohexstr(Op.Inputs[J].Offset) << ":"
                       << Op.Inputs[J].Size;
        llvm::outs() << "\n";
      }
    }
  }
}

void Pipeline::dumpMedIR(const std::vector<MedFunc> &Funcs) {
  llvm::outs() << "\n=== MedIR Dump ===\n";
  for (auto &MF : Funcs) {
    llvm::outs() << "func " << MF.Name << " @ 0x" << llvm::utohexstr(MF.Entry)
                 << " cc=" << static_cast<int>(MF.CC)
                 << " FrameSize=" << MF.FrameSize << "\n";
    for (auto &Blk : MF.Blocks) {
      llvm::outs() << "  block " << Blk.Id << " succs=["
                   << joinInts(Blk.Succs, ",") << "] preds=["
                   << joinInts(Blk.Preds, ",") << "]\n";
      for (auto &Phi : Blk.Phis)
        llvm::outs() << "    PHI " << Phi.Output.display() << " = ...\n";
      for (auto &Op : Blk.Ops) {
        llvm::outs() << "    " << ndOpName(Op.Opcode) << " "
                     << Op.Output.display();
        for (uint8_t J = 0; J < Op.NumInputs; ++J)
          llvm::outs() << " " << Op.Inputs[J].display();
        llvm::outs() << "\n";
      }
    }
  }
}

void Pipeline::dumpHighIR(const std::vector<HighFunc> &Funcs) {
  llvm::outs() << "\n=== HighIR Dump ===\n";
  for (auto &HF : Funcs) {
    llvm::outs() << "func " << HF.Name << " @ 0x" << llvm::utohexstr(HF.Entry)
                 << "\n";
    for (auto &Stmt : HF.Body)
      llvm::outs() << "  " << Stmt.str(1) << "\n";
    llvm::outs() << "\n";
  }
}

} // namespace neverd
