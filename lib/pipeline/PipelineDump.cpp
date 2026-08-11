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

const char *exceptionalEdgeName(ExceptionalEdgeKind Kind) {
  switch (Kind) {
  case ExceptionalEdgeKind::SEHFilter:
    return "seh-filter";
  case ExceptionalEdgeKind::SEHHandler:
    return "seh-handler";
  case ExceptionalEdgeKind::SEHFinally:
    return "seh-finally";
  case ExceptionalEdgeKind::CxxCleanup:
    return "cxx-cleanup";
  case ExceptionalEdgeKind::CxxCatch:
    return "cxx-catch";
  case ExceptionalEdgeKind::Unknown:
    return "unknown";
  }
  return "unknown";
}

void dumpExceptionSummary(const std::optional<ExceptionFunction> &Metadata,
                          llvm::raw_ostream &OS, llvm::StringRef Prefix) {
  if (!Metadata)
    return;
  const ExceptionFunction &EH = *Metadata;
  OS << Prefix << "exception encoding=" << getExceptionEncodingName(EH.Encoding)
     << " status=" << getExceptionParseStatusName(EH.ParseStatus)
     << " personality=" << getExceptionPersonalityName(EH.Personality)
     << " range=[0x" << llvm::utohexstr(EH.CodeRange.Begin) << ",0x"
     << llvm::utohexstr(EH.CodeRange.End) << ")\n";
}

void dumpExceptionalEdges(const std::vector<ExceptionalEdge> &Edges,
                          llvm::raw_ostream &OS) {
  OS << "[";
  for (size_t I = 0; I < Edges.size(); ++I) {
    if (I)
      OS << ",";
    const ExceptionalEdge &Edge = Edges[I];
    OS << exceptionalEdgeName(Edge.Kind) << ":";
    if (Edge.BlockId >= 0)
      OS << "b" << Edge.BlockId;
    else
      OS << "external";
    OS << "@0x" << llvm::utohexstr(Edge.TargetVA);
  }
  OS << "]";
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// IR dump helpers
//===----------------------------------------------------------------------===//

void Pipeline::dumpLowIR(const std::vector<LowFunc> &Funcs) {
  dumpLowIR(Funcs, llvm::outs());
}

void Pipeline::dumpLowIR(const std::vector<LowFunc> &Funcs,
                         llvm::raw_ostream &OS) {
  OS << "\n=== LowIR Dump ===\n";
  for (auto &LF : Funcs) {
    OS << "func " << LF.Name << " @ 0x" << llvm::utohexstr(LF.Entry) << " ("
       << LF.Blocks.size() << " blocks)\n";
    dumpExceptionSummary(LF.ExceptionMetadata, OS, "  ");
    for (auto &Blk : LF.Blocks) {
      OS << "  block " << Blk.Id << " [0x" << llvm::utohexstr(Blk.StartAddr)
         << " - 0x" << llvm::utohexstr(Blk.EndAddr) << "] succs=["
         << joinInts(Blk.Succs, ",") << "] preds=[" << joinInts(Blk.Preds, ",")
         << "]";
      if (!Blk.ExceptionalSuccs.empty()) {
        OS << " eh_succs=";
        dumpExceptionalEdges(Blk.ExceptionalSuccs, OS);
      }
      OS << "\n";
      for (auto &Op : Blk.Ops) {
        OS << "    [0x" << llvm::utohexstr(Op.Addr) << "." << Op.Seq << "] "
           << ndOpName(Op.Opcode) << " ";
        if (Op.Output.Size > 0)
          OS << vnodeSpaceAbbrev(Op.Output.Space) << ":0x"
             << llvm::utohexstr(Op.Output.Offset) << ":" << Op.Output.Size;
        for (uint8_t J = 0; J < Op.NumInputs; ++J)
          OS << " " << vnodeSpaceAbbrev(Op.Inputs[J].Space) << ":0x"
             << llvm::utohexstr(Op.Inputs[J].Offset) << ":"
             << Op.Inputs[J].Size;
        OS << "\n";
      }
    }
  }
}

void Pipeline::dumpMedIR(const std::vector<MedFunc> &Funcs) {
  dumpMedIR(Funcs, llvm::outs());
}

void Pipeline::dumpMedIR(const std::vector<MedFunc> &Funcs,
                         llvm::raw_ostream &OS) {
  OS << "\n=== MedIR Dump ===\n";
  for (auto &MF : Funcs) {
    OS << "func " << MF.Name << " @ 0x" << llvm::utohexstr(MF.Entry)
       << " cc=" << static_cast<int>(MF.CC) << " FrameSize=" << MF.FrameSize
       << "\n";
    dumpExceptionSummary(MF.ExceptionMetadata, OS, "  ");
    for (auto &Blk : MF.Blocks) {
      OS << "  block " << Blk.Id << " succs=[" << joinInts(Blk.Succs, ",")
         << "] preds=[" << joinInts(Blk.Preds, ",") << "]";
      if (!Blk.ExceptionalSuccs.empty()) {
        OS << " eh_succs=";
        dumpExceptionalEdges(Blk.ExceptionalSuccs, OS);
      }
      OS << "\n";
      for (auto &Phi : Blk.Phis)
        OS << "    PHI " << Phi.Output.display() << " = ...\n";
      for (auto &Op : Blk.Ops) {
        OS << "    " << ndOpName(Op.Opcode) << " " << Op.Output.display();
        for (uint8_t J = 0; J < Op.NumInputs; ++J)
          OS << " " << Op.Inputs[J].display();
        OS << "\n";
      }
    }
  }
}

void Pipeline::dumpHighIR(const std::vector<HighFunc> &Funcs) {
  dumpHighIR(Funcs, llvm::outs());
}

void Pipeline::dumpHighIR(const std::vector<HighFunc> &Funcs,
                          llvm::raw_ostream &OS) {
  OS << "\n=== HighIR Dump ===\n";
  for (auto &HF : Funcs) {
    OS << "func " << HF.Name << " @ 0x" << llvm::utohexstr(HF.Entry) << "\n";
    dumpExceptionSummary(HF.ExceptionMetadata, OS, "  ");
    for (auto &Stmt : HF.Body)
      OS << "  " << Stmt.str(1) << "\n";
    OS << "\n";
  }
}

} // namespace neverd
