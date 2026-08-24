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

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/ir/NdOps.h"
#include "neverd/sbf/analysis/SBFAnalysisLimits.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/analysis/SBFFunctionBody.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

namespace {

llvm::StringRef
sbfCFGEdgeType(const sbf::CFGEdge &Edge,
               const llvm::DenseSet<size_t> &ConditionalSources) {
  const sbf::EdgeKindInfo Info = sbf::getEdgeKindInfo(Edge.Kind);
  return ConditionalSources.contains(Edge.From) ? Info.ConditionalAPIName
                                                : Info.APIName;
}

inline constexpr llvm::StringLiteral
    kEmptyCallGraphJSON("{\"nodes\":[],\"edges\":[]}");

class LimitedJSONByteStream final : public llvm::raw_ostream {
public:
  explicit LimitedJSONByteStream(size_t Limit)
      : llvm::raw_ostream(/*unbuffered=*/true), Budget(Limit) {}

  [[nodiscard]] bool exceeded() const { return Budget.exceeded(); }
  [[nodiscard]] size_t consumed() const { return Budget.consumed(); }

private:
  void write_impl(const char *, size_t Size) override {
    (void)Budget.consume(Size);
  }

  uint64_t current_pos() const override { return Budget.consumed(); }

  sbf::AnalysisOutputByteBudget Budget;
};

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
            Obj["func"] = jsonSafeText(F.Name);
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
              Obj["func"] = jsonSafeText(F.Name);
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
            Obj["func"] = jsonSafeText(F.Name);
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
    const sbf::FunctionBodyIndex FunctionBodiesIndex(Program);
    const std::vector<size_t> Blocks = FunctionBodiesIndex.blocks(*Function);
    llvm::DenseSet<size_t> FunctionBlocks;
    FunctionBlocks.reserve(Blocks.size());
    for (size_t Block : Blocks)
      FunctionBlocks.insert(Block);
    llvm::DenseSet<size_t> ConditionalSources;
    for (const sbf::CFGEdge &Edge : Program.Low.Edges)
      if (Edge.Kind == sbf::EdgeKind::BranchTaken &&
          FunctionBlocks.contains(Edge.From))
        ConditionalSources.insert(Edge.From);
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
          Lines.push_back(jsonSafeText(vaHex(Instruction.Address) + ": " +
                                       sbf::formatInstruction(Instruction)));
      }
      Node["disasm"] = std::move(Lines);
      Nodes.push_back(std::move(Node));
    }
    for (const sbf::CFGEdge &ProgramEdge : Program.Low.Edges) {
      if (!FunctionBlocks.contains(ProgramEdge.From) ||
          (ProgramEdge.To && !FunctionBlocks.contains(*ProgramEdge.To)))
        continue;
      llvm::json::Object Edge;
      Edge["from"] = static_cast<int64_t>(ProgramEdge.From);
      if (ProgramEdge.To)
        Edge["to"] = static_cast<int64_t>(*ProgramEdge.To);
      else
        Edge["to"] = nullptr;
      Edge["type"] = sbfCFGEdgeType(ProgramEdge, ConditionalSources);
      Edges.push_back(std::move(Edge));
    }
    llvm::json::Object Root;
    Root["name"] = jsonSafeText(Function->Name);
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
  Root["name"] = jsonSafeText(F->Name);
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
    const sbf::FunctionBodyIndex FunctionBodiesIndex(*R.Result.SBF);
    const std::vector<size_t> Blocks = FunctionBodiesIndex.blocks(*Function);
    const std::set<size_t> FunctionBlocks(Blocks.begin(), Blocks.end());
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
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(kEmptyCallGraphJSON.str());

  std::optional<size_t> SBFEdgeCapacity;
  if (S->PipeResult.SBF) {
    SBFEdgeCapacity =
        sbf::callGraphEdgeCapacity(S->PipeResult.SBF->High.Functions.size());
    if (!SBFEdgeCapacity) {
      S->setError(sbf::kCallGraphNodeBudgetDiagnostic.str());
      return dupStr(kEmptyCallGraphJSON.str());
    }
    if (S->PipeResult.SBF->High.Calls.size() >
        sbf::kCallGraphInputCallSiteBudget) {
      S->setError(sbf::kCallGraphInputBudgetDiagnostic.str());
      return dupStr(kEmptyCallGraphJSON.str());
    }
  }
  if (!S->synchronizeFunctions())
    return dupStr(kEmptyCallGraphJSON.str());

  if (S->PipeResult.SBF) {
    SBFEdgeCapacity = sbf::callGraphEdgeCapacity(S->Functions.size());
    if (!SBFEdgeCapacity) {
      S->setError(sbf::kCallGraphNodeBudgetDiagnostic.str());
      return dupStr(kEmptyCallGraphJSON.str());
    }
  }

  struct CallGraphEdgeRecord {
    va_t Caller = 0;
    va_t Callee = 0;
    llvm::StringRef CallerName;
    llvm::StringRef CalleeName;
  };
  llvm::DenseSet<std::pair<va_t, va_t>> Seen;
  std::vector<CallGraphEdgeRecord> RecoveredEdges;
  JSONTextPool TextPool;
  bool EdgeBudgetExhausted = false;
  const auto AddEdge = [&](va_t Caller, va_t Callee, llvm::StringRef CallerName,
                           llvm::StringRef CalleeName) {
    const std::pair<va_t, va_t> Key = {Caller, Callee};
    if (Seen.contains(Key))
      return true;
    if (SBFEdgeCapacity && Seen.size() == *SBFEdgeCapacity) {
      EdgeBudgetExhausted = true;
      return false;
    }
    Seen.insert(Key);
    RecoveredEdges.push_back({Caller, Callee, TextPool.intern(CallerName),
                              TextPool.intern(CalleeName)});
    return true;
  };

  struct TargetCallSites {
    std::string Name;
    std::vector<size_t> SourceBlocks;
  };
  // Keep this map alive through JSON serialization: recovered edge records
  // refer to its canonical callee names without copying one name per edge.
  std::map<va_t, TargetCallSites> CallsByTarget;
  if (S->PipeResult.SBF && !S->PipeResult.SBF->High.Calls.empty()) {
    const auto &Program = *S->PipeResult.SBF;
    const sbf::FunctionBodyIndex FunctionBodies(Program);
    const auto BlockForSlot = [&](size_t Slot) -> std::optional<size_t> {
      const auto It = std::upper_bound(
          Program.Low.Blocks.begin(), Program.Low.Blocks.end(), Slot,
          [](size_t Candidate, const sbf::BasicBlock &Block) {
            return Candidate < Block.StartSlot;
          });
      if (It == Program.Low.Blocks.begin())
        return std::nullopt;
      const sbf::BasicBlock &Block = *std::prev(It);
      return Slot < Block.EndSlot ? std::optional<size_t>(Block.ID)
                                  : std::nullopt;
    };

    for (const auto &Call : Program.High.Calls) {
      if (Call.Kind != sbf::CallKind::Internal || !Call.TargetSlot)
        continue;
      const std::optional<size_t> BlockID = BlockForSlot(Call.SourceSlot);
      if (!BlockID)
        continue;
      const va_t Target =
          Program.Low.TextAddress + *Call.TargetSlot * sbf::kInstructionSize;
      auto TargetIt = CallsByTarget.find(Target);
      if (TargetIt == CallsByTarget.end()) {
        if (CallsByTarget.size() == sbf::kCallGraphTargetGroupBudget) {
          S->setError(sbf::kCallGraphInputBudgetDiagnostic.str());
          return dupStr(kEmptyCallGraphJSON.str());
        }
        TargetIt = CallsByTarget.try_emplace(Target).first;
      }
      TargetCallSites &Sites = TargetIt->second;
      if (Sites.Name.empty())
        Sites.Name = Call.Name;
      Sites.SourceBlocks.push_back(*BlockID);
    }

    std::vector<sbf::FunctionBodyIndex::BlockGroupQuery> Queries;
    std::vector<const TargetCallSites *> SitesByGroup;
    std::vector<va_t> TargetsByGroup;
    Queries.reserve(CallsByTarget.size());
    SitesByGroup.reserve(CallsByTarget.size());
    TargetsByGroup.reserve(CallsByTarget.size());
    for (const auto &[Target, Sites] : CallsByTarget) {
      Queries.push_back({Sites.SourceBlocks});
      SitesByGroup.push_back(&Sites);
      TargetsByGroup.push_back(Target);
    }

    const sbf::FunctionBodyIndex::BlockGroupFunctionBatch Callers =
        FunctionBodies.functionsForBlockGroups(
            Queries, sbf::kCallGraphProvenanceBlockVisitBudget,
            *SBFEdgeCapacity);
    if (!Callers.complete()) {
      S->setError((Callers.VisitBudgetExhausted
                       ? sbf::kCallGraphProvenanceBudgetDiagnostic
                       : sbf::kCallGraphEdgeBudgetDiagnostic)
                      .str());
      return dupStr(kEmptyCallGraphJSON.str());
    }

    for (size_t GroupID = 0; GroupID < Queries.size(); ++GroupID) {
      const va_t Target = TargetsByGroup[GroupID];
      const TargetCallSites &Sites = *SitesByGroup[GroupID];
      for (size_t FunctionID : Callers.functionsForGroup(GroupID)) {
        if (FunctionID >= Program.High.Functions.size())
          continue;
        const sbf::Function &Caller = Program.High.Functions[FunctionID];
        if (!AddEdge(Caller.Address, Target, Caller.Name, Sites.Name)) {
          S->setError(sbf::kCallGraphEdgeBudgetDiagnostic.str());
          return dupStr(kEmptyCallGraphJSON.str());
        }
      }
    }
  }

  std::map<va_t, std::string> FuncNames;
  for (const auto &F : S->Functions)
    FuncNames[F.Entry] = F.Name;
  for (const auto &LF : S->PipeResult.LowFuncs) {
    for (const auto &B : LF.Blocks) {
      for (const auto &Op : B.Ops) {
        if (Op.Opcode != NdOp::CALL)
          continue;
        if (Op.NumInputs < 1 || !Op.Inputs[0].isConst())
          continue;
        va_t Tgt = Op.Inputs[0].Offset;
        const auto Callee = FuncNames.find(Tgt);
        if (Callee == FuncNames.end())
          continue;
        if (!AddEdge(LF.Entry, Tgt, LF.Name, Callee->second))
          break;
      }
      if (EdgeBudgetExhausted)
        break;
    }
    if (EdgeBudgetExhausted)
      break;
  }
  if (EdgeBudgetExhausted) {
    S->setError(sbf::kCallGraphEdgeBudgetDiagnostic.str());
    return dupStr(kEmptyCallGraphJSON.str());
  }

  std::vector<llvm::StringRef> FunctionJSONNames;
  FunctionJSONNames.reserve(S->Functions.size());
  for (const auto &Function : S->Functions)
    FunctionJSONNames.push_back(TextPool.intern(Function.Name));

  // One canonical streaming serializer is used first as an exact escaped-byte
  // counter and then for the committed document. This counts fixed syntax,
  // addresses, integers, and repaired names exactly as they appear on the
  // wire, without materializing JSON nodes before the byte gate.
  const auto WriteCallGraphJSON = [&](llvm::raw_ostream &OS,
                                      auto &&CanContinue) {
    llvm::json::OStream JSON(OS);
    JSON.object([&] {
      JSON.attributeArray("nodes", [&] {
        for (size_t FunctionID = 0; FunctionID < S->Functions.size();
             ++FunctionID) {
          if (!CanContinue())
            break;
          const auto &Function = S->Functions[FunctionID];
          JSON.object([&] {
            JSON.attribute("name", FunctionJSONNames[FunctionID]);
            JSON.attribute("addr", vaHex(Function.Entry));
            JSON.attribute("size", static_cast<int64_t>(Function.Size));
          });
        }
      });
      JSON.attributeArray("edges", [&] {
        for (const CallGraphEdgeRecord &Recovered : RecoveredEdges) {
          if (!CanContinue())
            break;
          JSON.object([&] {
            JSON.attribute("caller", vaHex(Recovered.Caller));
            JSON.attribute("callee", vaHex(Recovered.Callee));
            JSON.attribute("caller_name", Recovered.CallerName);
            JSON.attribute("callee_name", Recovered.CalleeName);
          });
        }
      });
    });
  };

  if (S->PipeResult.SBF) {
    LimitedJSONByteStream Counter(sbf::kCallGraphOutputByteBudget);
    WriteCallGraphJSON(Counter, [&] { return !Counter.exceeded(); });
    if (Counter.exceeded()) {
      S->setError(sbf::kCallGraphByteBudgetDiagnostic.str());
      return dupStr(kEmptyCallGraphJSON.str());
    }

    std::string Serialized;
    Serialized.reserve(Counter.consumed());
    llvm::raw_string_ostream OS(Serialized);
    WriteCallGraphJSON(OS, [] { return true; });
    OS.flush();
    return dupStr(Serialized);
  }

  std::string Serialized;
  llvm::raw_string_ostream OS(Serialized);
  WriteCallGraphJSON(OS, [] { return true; });
  OS.flush();
  return dupStr(Serialized);
}
