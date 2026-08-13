//===- NeverDCAPIGraph.cpp - C API: xrefs, CFG, and call graph ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cross-references, control-flow graphs, and call graphs.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/ir/NdOps.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <set>

using namespace neverd;
using namespace neverd::sdk;

namespace {

llvm::StringRef sbfCFGEdgeType(const sbf::SBFProgram &Program,
                               const sbf::CFGEdge &Edge) {
  switch (Edge.Kind) {
  case sbf::EdgeKind::Fallthrough:
    for (const sbf::CFGEdge &Candidate : Program.Low.Edges)
      if (Candidate.From == Edge.From &&
          Candidate.Kind == sbf::EdgeKind::BranchTaken)
        return "false";
    return "fallthrough";
  case sbf::EdgeKind::BranchTaken:
    return "true";
  case sbf::EdgeKind::Branch:
    return "unconditional";
  case sbf::EdgeKind::Call:
    return "call";
  case sbf::EdgeKind::IndirectCall:
    return "indirect-call";
  case sbf::EdgeKind::Return:
    return "return";
  case sbf::EdgeKind::Invalid:
    return "invalid";
  }
  llvm_unreachable("covered SBF CFG edge kind");
}

} // namespace

// ===--------------------------------------------------------------------===//
// XRefs
// ===--------------------------------------------------------------------===//

const char *neverd_xrefs_to_json(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &F : S->PipeResult.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        for (int I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Addr) {
            llvm::json::Object Obj;
            Obj["from"] = vaHex(Op.Addr);
            Obj["func"] = F.Name;
            Obj["block"] = B.Id;
            Arr.push_back(std::move(Obj));
          }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_xrefs_from_json(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &F : S->PipeResult.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Addr == Addr)
          for (int I = 0; I < Op.NumInputs; ++I)
            if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset != 0) {
              llvm::json::Object Obj;
              Obj["to"] = vaHex(Op.Inputs[I].Offset);
              Obj["func"] = F.Name;
              Obj["opcode"] = std::string(ndOpName(Op.Opcode));
              Arr.push_back(std::move(Obj));
            }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_xrefs_scan(neverd_session_t Sess, const char *InputPath,
                              neverd_va_t Target) {
  PipelineRunner R;
  std::string Err;
  auto *S = static_cast<Session *>(Sess);
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  PipelineOptions Opts;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  llvm::json::Array Arr;
  for (const auto &F : R.Result.LowFuncs)
    for (const auto &B : F.Blocks)
      for (const auto &Op : B.Ops)
        for (int i = 0; i < Op.NumInputs; ++i)
          if (Op.Inputs[i].isConst() &&
              Op.Inputs[i].Offset == static_cast<va_t>(Target)) {
            llvm::json::Object Obj;
            Obj["from"] = vaHex(Op.Addr);
            Obj["func"] = F.Name;
            Obj["block"] = static_cast<int64_t>(B.Id);
            Arr.push_back(std::move(Obj));
          }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

// ===--------------------------------------------------------------------===//
// CFG graph
// ===--------------------------------------------------------------------===//

const char *neverd_cfg_json(neverd_session_t Sess, neverd_va_t FuncEntry) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(std::string("{}"));

  if (S->PipeResult.EVM) {
    if (FuncEntry != evm::kEntryPC) {
      S->setError("EVM program entry is pc " + std::to_string(evm::kEntryPC));
      return dupStr(std::string("{}"));
    }
    llvm::json::Array Nodes;
    llvm::json::Array Edges;
    for (const auto &Block : S->PipeResult.EVM->Low.Blocks) {
      llvm::json::Object Node;
      Node["id"] = static_cast<int64_t>(Block.StartPC);
      Node["start"] = vaHex(Block.StartPC);
      Node["end"] = vaHex(Block.EndPC);
      Node["insn_count"] = static_cast<int64_t>(Block.InstructionCount);
      Node["reachable"] = Block.Reachable;
      llvm::json::Array Lines;
      for (size_t I = Block.FirstInstruction;
           I < Block.FirstInstruction + Block.InstructionCount; ++I) {
        const auto &Instruction = S->PipeResult.EVM->Low.Instructions[I];
        Lines.push_back(vaHex(Instruction.PC) + ": " +
                        std::string(Instruction.Info.Name));
      }
      Node["disasm"] = std::move(Lines);
      Nodes.push_back(std::move(Node));
      for (const auto &Successor : Block.Successors) {
        llvm::json::Object Edge;
        Edge["from"] = static_cast<int64_t>(Block.StartPC);
        if (Successor.Target)
          Edge["to"] = static_cast<int64_t>(*Successor.Target);
        else
          Edge["to"] = nullptr;
        switch (Successor.Kind) {
        case evm::EdgeKind::ConditionalTrue:
          Edge["type"] = "true";
          break;
        case evm::EdgeKind::ConditionalFalse:
          Edge["type"] = "false";
          break;
        case evm::EdgeKind::Indirect:
          Edge["type"] = "indirect";
          break;
        default:
          Edge["type"] = "unconditional";
          break;
        }
        Edges.push_back(std::move(Edge));
      }
    }
    llvm::json::Object Root;
    Root["name"] = kEVMEntrySymbolName;
    Root["entry"] = vaHex(evm::kEntryPC);
    Root["nodes"] = std::move(Nodes);
    Root["edges"] = std::move(Edges);
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  }

  if (S->PipeResult.SBF) {
    const auto &Program = *S->PipeResult.SBF;
    const sbf::Function *Function = nullptr;
    for (const auto &Candidate : Program.High.Functions)
      if (Candidate.Address == FuncEntry) {
        Function = &Candidate;
        break;
      }
    if (!Function) {
      S->setError("SBF function entry not found");
      return dupStr(std::string("{}"));
    }
    const std::set<size_t> FunctionBlocks(Function->Blocks.begin(),
                                          Function->Blocks.end());
    llvm::json::Array Nodes;
    llvm::json::Array Edges;
    for (const auto &Block : Program.Low.Blocks) {
      if (!FunctionBlocks.contains(Block.ID))
        continue;
      llvm::json::Object Node;
      Node["id"] = static_cast<int64_t>(Block.ID);
      Node["start"] = vaHex(Program.Low.TextAddress +
                            Block.StartSlot * sbf::kInstructionSize);
      Node["end"] = vaHex(Program.Low.TextAddress +
                          Block.EndSlot * sbf::kInstructionSize);
      Node["insn_count"] =
          static_cast<int64_t>(Block.EndSlot - Block.StartSlot);
      Node["reachable"] = Block.Reachable;
      llvm::json::Array Lines;
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
        const auto &Instruction = Program.Low.Instructions[Slot];
        if (!Instruction.IsContinuation)
          Lines.push_back(vaHex(Instruction.Address) + ": " +
                          sbf::formatInstruction(Instruction));
      }
      Node["disasm"] = std::move(Lines);
      Nodes.push_back(std::move(Node));
      for (const sbf::CFGEdge &ProgramEdge : Program.Low.Edges) {
        if (ProgramEdge.From != Block.ID || !ProgramEdge.To ||
            !FunctionBlocks.contains(*ProgramEdge.To))
          continue;
        llvm::json::Object Edge;
        Edge["from"] = static_cast<int64_t>(Block.ID);
        Edge["to"] = static_cast<int64_t>(*ProgramEdge.To);
        Edge["type"] = sbfCFGEdgeType(Program, ProgramEdge);
        Edges.push_back(std::move(Edge));
      }
    }
    llvm::json::Object Root;
    Root["name"] = Function->Name;
    Root["entry"] = vaHex(Function->Address);
    Root["nodes"] = std::move(Nodes);
    Root["edges"] = std::move(Edges);
    return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
  }

  const LowFunc *F = S->findLowFunc(FuncEntry);
  if (!F) {
    S->setError("function not found");
    return dupStr(std::string("{}"));
  }

  llvm::json::Array Nodes;
  llvm::json::Array Edges;

  for (const auto &B : F->Blocks) {
    llvm::json::Object Node;
    Node["id"] = B.Id;
    Node["start"] = vaHex(B.StartAddr);
    Node["end"] = vaHex(B.EndAddr);
    Node["insn_count"] = static_cast<int64_t>(B.Ops.size());

    llvm::json::Array DisasmLines;
    va_t Cur = B.StartAddr;
    while (Cur < B.EndAddr) {
      const Segment *Seg = S->Img.getSegmentFor(Cur);
      if (!Seg)
        break;
      uint64_t Off64 = Cur - Seg->VA;
      if (Off64 >= Seg->Data.size())
        break;
      size_t Off = static_cast<size_t>(Off64);
      size_t Avail = static_cast<size_t>(std::min<uint64_t>(
          16, std::min<uint64_t>(B.EndAddr - Cur,
                                 std::min<uint64_t>(Seg->Size - Off64,
                                                    Seg->Data.size() - Off))));
      if (Avail == 0)
        break;
      const uint8_t *Bytes = Seg->Data.data() + Off;
      DecodedInsn DI;
      int Sz = S->Dec.decodeOne(Bytes, Avail, Cur, DI);
      if (Sz <= 0)
        break;
      std::string Line = vaHex(Cur) + ": " + (DI.Raw ? DI.Raw->mnemonic : "") +
                         " " + (DI.Raw ? DI.Raw->op_str : "");
      DisasmLines.push_back(Line);
      Cur += Sz;
    }
    Node["disasm"] = std::move(DisasmLines);
    Nodes.push_back(std::move(Node));

    for (size_t I = 0; I < B.Succs.size(); ++I) {
      llvm::json::Object Edge;
      Edge["from"] = B.Id;
      Edge["to"] = B.Succs[I];
      if (B.Succs.size() == 2)
        Edge["type"] = (I == 0) ? "true" : "false";
      else
        Edge["type"] = "unconditional";
      Edges.push_back(std::move(Edge));
    }
  }

  llvm::json::Object Root;
  Root["name"] = F->Name;
  Root["entry"] = vaHex(F->Entry);
  Root["nodes"] = std::move(Nodes);
  Root["edges"] = std::move(Edges);
  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

const char *neverd_cfg_dot(neverd_session_t Sess, const char *InputPath,
                           const char *FuncNameOrAddr, int Styled) {
  PipelineRunner R;
  std::string Err;
  auto *S = static_cast<Session *>(Sess);
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }
  PipelineOptions Opts;
  if (S) {
    S->applyAnalysisOptions(Opts);
  }
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return nullptr;
  }

  if (R.Result.EVM) {
    std::string Buf;
    llvm::raw_string_ostream OS(Buf);
    OS << "digraph cfg {\n"
          "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
    for (const auto &Block : R.Result.EVM->Low.Blocks)
      OS << "  bb" << Block.StartPC << " [label=\"PC 0x"
         << llvm::utohexstr(Block.StartPC) << "\"];\n";
    for (const auto &Block : R.Result.EVM->Low.Blocks)
      for (const auto &Edge : Block.Successors)
        if (Edge.Target)
          OS << "  bb" << Block.StartPC << " -> bb" << *Edge.Target << ";\n";
    OS << "}\n";
    return dupStr(Buf);
  }

  if (R.Result.SBF) {
    std::string Buf;
    llvm::raw_string_ostream OS(Buf);
    const sbf::Function *Function =
        sbf::findFunction(*R.Result.SBF, FuncNameOrAddr ? FuncNameOrAddr : "");
    if (!Function) {
      if (S)
        S->setError("SBF function not found");
      return nullptr;
    }
    const std::set<size_t> FunctionBlocks(Function->Blocks.begin(),
                                          Function->Blocks.end());
    OS << "digraph cfg {\n";
    if (Styled) {
      OS << "  node [shape=box, fontname=\"Courier\", fontsize=10, "
            "style=filled, fillcolor=\"#252526\", fontcolor=\"#ebebeb\", "
            "color=\"#3c3c3c\"];\n"
            "  edge [color=\"#569cd6\"];\n"
            "  bgcolor=\"#1e1e1e\";\n";
    } else {
      OS << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
    }
    for (const auto &Block : R.Result.SBF->Low.Blocks) {
      if (!FunctionBlocks.contains(Block.ID))
        continue;
      OS << "  bb" << Block.ID << " [label=\"BB" << Block.ID << " (slot "
         << Block.StartSlot << ")\"];\n";
    }
    for (const auto &Block : R.Result.SBF->Low.Blocks) {
      if (!FunctionBlocks.contains(Block.ID))
        continue;
      for (size_t Successor : Block.Successors)
        if (FunctionBlocks.contains(Successor))
          OS << "  bb" << Block.ID << " -> bb" << Successor << ";\n";
    }
    OS << "}\n";
    return dupStr(Buf);
  }

  std::string FN(FuncNameOrAddr ? FuncNameOrAddr : "");
  const LowFunc *Target = nullptr;
  va_t TargetAddr = 0;
  bool HasTargetAddr = false;
  llvm::StringRef AddrRef(FN);
  if (!AddrRef.empty() && AddrRef.front() != '-') {
    if (AddrRef.consume_front("0x") || AddrRef.consume_front("0X"))
      HasTargetAddr = !AddrRef.empty();
    else
      HasTargetAddr = true;
    if (HasTargetAddr)
      HasTargetAddr = !AddrRef.getAsInteger(16, TargetAddr);
  }
  for (const auto &F : R.Result.LowFuncs)
    if (F.Name == FN || (HasTargetAddr && F.Entry == TargetAddr)) {
      Target = &F;
      break;
    }
  if (!Target) {
    if (S)
      S->setError("function not found: " + FN);
    return nullptr;
  }

  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  OS << "digraph cfg {\n";
  if (Styled) {
    OS << "  node [shape=box, fontname=\"Courier\", fontsize=10, "
          "style=filled, fillcolor=\"#252526\", fontcolor=\"#ebebeb\", "
          "color=\"#3c3c3c\"];\n";
    OS << "  edge [color=\"#569cd6\"];\n";
    OS << "  bgcolor=\"#1e1e1e\";\n";
  } else {
    OS << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
  }
  for (const auto &B : Target->Blocks)
    OS << "  bb" << B.Id << " [label=\"BB" << B.Id << " (0x"
       << llvm::utohexstr(B.StartAddr) << ")\"];\n";
  for (const auto &B : Target->Blocks)
    for (size_t J = 0; J < B.Succs.size(); ++J) {
      std::string Color = Styled ? "\"#569cd6\"" : "blue";
      if (B.Succs.size() == 2)
        Color = (J == 0) ? (Styled ? "\"#4ec9b0\"" : "green")
                         : (Styled ? "\"#f44747\"" : "red");
      OS << "  bb" << B.Id << " -> bb" << B.Succs[J] << " [color=" << Color
         << "];\n";
    }
  OS << "}\n";
  return dupStr(Buf);
}

// ===--------------------------------------------------------------------===//
// Call graph
// ===--------------------------------------------------------------------===//

const char *neverd_callgraph_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->ensurePipeline())
    return dupStr("{\"nodes\":[],\"edges\":[]}");
  if (!S->synchronizeFunctions())
    return dupStr("{\"nodes\":[],\"edges\":[]}");

  std::map<va_t, std::string> FuncNames;
  for (const auto &F : S->Functions)
    FuncNames[F.Entry] = F.Name;

  llvm::json::Array Nodes;
  for (const auto &F : S->Functions) {
    llvm::json::Object N;
    N["name"] = F.Name;
    N["addr"] = vaHex(F.Entry);
    N["size"] = static_cast<int64_t>(F.Size);
    Nodes.push_back(std::move(N));
  }

  std::set<std::pair<va_t, va_t>> Seen;
  llvm::json::Array Edges;
  if (S->PipeResult.SBF) {
    const auto &Program = *S->PipeResult.SBF;
    std::map<size_t, const sbf::Function *> BlockOwners;
    for (const auto &Function : Program.High.Functions)
      for (size_t Block : Function.Blocks)
        BlockOwners.try_emplace(Block, &Function);
    std::map<size_t, size_t> SlotBlocks;
    for (const auto &Block : Program.Low.Blocks)
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
        SlotBlocks[Slot] = Block.ID;
    for (const auto &Call : Program.High.Calls) {
      if (Call.Kind != sbf::CallKind::Internal || !Call.TargetSlot)
        continue;
      auto BlockIt = SlotBlocks.find(Call.SourceSlot);
      if (BlockIt == SlotBlocks.end())
        continue;
      auto CallerIt = BlockOwners.find(BlockIt->second);
      if (CallerIt == BlockOwners.end())
        continue;
      const va_t Target =
          Program.Low.TextAddress + *Call.TargetSlot * sbf::kInstructionSize;
      auto Key = std::make_pair(CallerIt->second->Address, Target);
      if (!Seen.insert(Key).second)
        continue;
      llvm::json::Object Edge;
      Edge["caller"] = vaHex(Key.first);
      Edge["callee"] = vaHex(Key.second);
      Edge["caller_name"] = CallerIt->second->Name;
      Edge["callee_name"] = Call.Name;
      Edges.push_back(std::move(Edge));
    }
  }
  for (const auto &LF : S->PipeResult.LowFuncs) {
    for (const auto &B : LF.Blocks) {
      for (const auto &Op : B.Ops) {
        if (Op.Opcode != NdOp::CALL)
          continue;
        if (Op.NumInputs < 1 || !Op.Inputs[0].isConst())
          continue;
        va_t Tgt = Op.Inputs[0].Offset;
        if (FuncNames.find(Tgt) == FuncNames.end())
          continue;
        auto Key = std::make_pair(LF.Entry, Tgt);
        if (!Seen.insert(Key).second)
          continue;
        llvm::json::Object E;
        E["caller"] = vaHex(LF.Entry);
        E["callee"] = vaHex(Tgt);
        E["caller_name"] = LF.Name;
        E["callee_name"] = FuncNames[Tgt];
        Edges.push_back(std::move(E));
      }
    }
  }

  llvm::json::Object Result;
  Result["nodes"] = std::move(Nodes);
  Result["edges"] = std::move(Edges);
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
