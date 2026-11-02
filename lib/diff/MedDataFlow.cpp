//===- MedDataFlow.cpp - Immutable projection of native SSA observations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the pipeline's operation widths, ordered operands, PHI predecessor
/// labels and native origins. Unknown effects are boundaries, never guesses.
//===----------------------------------------------------------------------===//

#include "neverd/diff/MedDataFlow.h"

#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {
namespace {
const char *const Opcodes[] = {
#define NEVERD_DIFF_FLOW_OPCODE(Name) #Name,
    ND_OP_LIST(NEVERD_DIFF_FLOW_OPCODE)
#undef NEVERD_DIFF_FLOW_OPCODE
};
using Variable = std::tuple<MedVar::VarKind, int, int>;
Variable key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }
bool commutative(NdOp Op) {
  switch (Op) {
  case NdOp::INT_ADD:
  case NdOp::INT_MULT:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    return true;
  default:
    return false;
  }
}
uint32_t gaps(NdOp Op) {
  switch (Op) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
    return FlowMemoryAlias;
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::INTRINSIC:
    return FlowCallEffects | FlowMemoryAlias;
  case NdOp::INDIR_BR:
    return FlowIndirectControl;
  default:
    return 0;
  }
}
} // namespace

DataFlowGraph buildMedDataFlow(const MedFunc &F, llvm::StringRef Revision,
                               llvm::StringRef ABI, bool NativeComplete,
                               const DataFlowOptions &Options,
                               const ComparisonControl &Control) {
  if (auto Error = Options.validate())
    throw std::invalid_argument(*Error);
  if (F.SkippedSSA)
    throw std::invalid_argument("dataflow unavailable: pipeline skipped SSA");
  if (F.Blocks.empty())
    throw std::invalid_argument("dataflow has no body");
  if (F.Blocks.size() > Options.MaxBlocks)
    throw DataFlowInterrupted(ComparisonState::ResourceLimited);
  uint64_t Work = 0;
  const auto Charge = [&] {
    if (Work == Options.MaxWork)
      throw DataFlowInterrupted(ComparisonState::ResourceLimited);
    if (Work++ % limits::diff::kFlowPollWork == 0)
      if (auto State = Control.stop())
        throw DataFlowInterrupted(*State);
  };
  DataFlowGraph G;
  G.Function = F.Entry;
  G.Revision = Revision.str();
  G.ABI = ABI.str();
  G.Gaps = NativeComplete ? 0 : FlowNativeCoverage;
  if (F.HasUnresolvedSEHFrame)
    G.Gaps |= FlowExceptionalControl;
  std::map<int, uint32_t> Blocks;
  for (const auto &B : F.Blocks) {
    Charge();
    if (!Blocks.emplace(B.Id, G.Blocks.size()).second)
      throw std::invalid_argument("dataflow has duplicate block identities");
    FlowBlock Out;
    Out.Begin = B.StartAddr;
    Out.End = B.EndAddr;
    if (!B.ExceptionalSuccs.empty() || !B.ExceptionalPreds.empty())
      Out.Gaps |= FlowExceptionalControl;
    G.Blocks.push_back(std::move(Out));
  }
  const auto Block = [&](int Id) {
    const auto It = Blocks.find(Id);
    if (It == Blocks.end())
      throw std::invalid_argument("dataflow names a missing CFG block");
    return It->second;
  };
  std::map<std::pair<int, int>, const MedCallInfo *> CallInfos;
  for (const auto &CI : F.CallInfos) {
    Charge();
    const auto BI = Block(CI.BlockId);
    if (CI.OpIdx < 0 || size_t(CI.OpIdx) >= F.Blocks[BI].Ops.size())
      throw std::invalid_argument(
          "dataflow call arguments have no owning operation");
    const auto Code = F.Blocks[BI].Ops[CI.OpIdx].Opcode;
    if ((Code != NdOp::CALL && Code != NdOp::INDIR_CALL) ||
        !CallInfos.emplace(std::pair{CI.BlockId, CI.OpIdx}, &CI).second)
      throw std::invalid_argument(
          "dataflow call arguments have a stale or duplicate owner");
  }
  // The pipeline/emitter use the first retained block as the entry; block
  // identities need not equal their vector position.
  G.EntryBlock = 0;
  std::map<Variable, uint32_t> Definitions, Parameters;
  std::map<uint64_t, uint32_t> RegisterParameters;
  std::map<int, uint32_t> StackParameters;
  for (uint32_t I = 0; I < F.Params.size(); ++I) {
    Charge();
    const auto &P = F.Params[I];
    Parameters.emplace(key(P), I);
    if (P.Kind == MedVar::Param || P.Kind == MedVar::Reg) {
      auto Add = [&](auto &Map, auto Key) {
        auto [At, Inserted] = Map.emplace(Key, I);
        if (!Inserted)
          At->second = NoIndex;
      };
      if (P.RegOff == kNoParamReg)
        Add(StackParameters, P.Id);
      else
        Add(RegisterParameters, P.RegOff);
    }
  }
  const auto Parameter = [&](const MedVar &V) -> uint32_t {
    uint32_t Index = NoIndex;
    if (V.Kind == MedVar::Param || V.Kind == MedVar::Reg) {
      if (V.RegOff == kNoParamReg) {
        if (auto It = StackParameters.find(V.Id); It != StackParameters.end())
          Index = It->second;
      } else if (auto It = RegisterParameters.find(V.RegOff);
                 It != RegisterParameters.end())
        Index = It->second;
    } else if (auto It = Parameters.find(key(V)); It != Parameters.end())
      Index = It->second;
    return Index != NoIndex && V.Size && V.Size <= F.Params[Index].Size
               ? Index
               : NoIndex;
  };
  std::map<uint32_t, uint32_t> Calls;
  std::vector<std::vector<std::pair<int, MedVar>>> Inputs;
  const auto Define = [&](const MedVar &V, uint32_t N) {
    Charge();
    if (!V.Size)
      return;
    if (V.isConst() || V.Kind == MedVar::Unspecified ||
        !Definitions.emplace(key(V), N).second)
      throw std::invalid_argument(
          "dataflow has a non-SSA or invalid definition");
  };
  const auto Add = [&](FlowNode Node) {
    Charge();
    if (G.Nodes.size() == Options.MaxNodes)
      throw DataFlowInterrupted(ComparisonState::ResourceLimited);
    const uint32_t Index = G.Nodes.size();
    G.Nodes.push_back(std::move(Node));
    Inputs.emplace_back();
    return Index;
  };
  for (const auto &B : F.Blocks) {
    const auto BI = Block(B.Id);
    for (auto S : B.Succs) {
      Charge();
      G.Blocks[BI].Successors.push_back(Block(S));
    }
    for (const auto &Phi : B.Phis) {
      FlowNode Node;
      Node.Opcode = "PHI";
      Node.Phi = true;
      Node.Block = BI;
      Node.ResultBytes = Phi.Output.Size;
      Node.Gaps = Phi.ExceptionalEntry ? FlowExceptionalControl : 0;
      const auto N = Add(std::move(Node));
      Define(Phi.Output, N);
      for (const auto &Input : Phi.Args) {
        Charge();
        Inputs[N].push_back(Input);
      }
      if (Phi.ExceptionalEntry)
        Inputs[N].emplace_back(-1, *Phi.ExceptionalEntry);
    }
    for (const auto &Op : B.Ops) {
      if (Op.Opcode >= NdOp::_COUNT || Op.NumInputs > Op.Inputs.size())
        throw std::invalid_argument("dataflow operation has an invalid shape");
      FlowNode Node;
      Node.Opcode = Opcodes[size_t(Op.Opcode)];
      Node.ResultBytes = Op.Output.Size;
      Node.MemoryOrdering = uint8_t(Op.MemoryOrdering);
      Node.MemoryAddressSpace = uint8_t(Op.MemoryAddressSpace);
      Node.Block = BI;
      Node.Address = Op.Addr;
      Node.OriginSequence = Op.OriginSeq;
      Node.Commutative = commutative(Op.Opcode);
      Node.Gaps = gaps(Op.Opcode);
      const MedCallInfo *Call = nullptr;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto It = CallInfos.find({B.Id, int(&Op - B.Ops.data())});
        if (It == CallInfos.end())
          Node.Gaps |= FlowUnknownInput;
        else {
          Call = It->second;
          if (Call->Args.size() > Options.MaxWork)
            throw DataFlowInterrupted(ComparisonState::ResourceLimited);
          Node.CallArgumentBegin = Op.NumInputs;
          Node.CallArgumentCount = Call->Args.size();
        }
      }
      const auto N = Add(std::move(Node));
      const bool EntrySeed =
          BI == G.EntryBlock && Op.Output.SSAVer == 0 &&
          med_calling_conv_detail::isEntryLiveInCopy(Op, Op.Output.RegOff);
      if (!EntrySeed)
        Define(Op.Output, N);
      for (const auto &V : Op.IntrinsicOutputs)
        Define(V, N);
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        Charge();
        Inputs[N].emplace_back(-1, Op.Inputs[I]);
      }
      if (Call)
        for (const auto &Argument : Call->Args) {
          Charge();
          Inputs[N].emplace_back(-1, Argument);
        }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        if (Op.CallSiteId && !Calls.emplace(Op.CallSiteId, N).second)
          throw std::invalid_argument("dataflow repeats a call-site identity");
      }
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN ||
          Op.DoesNotReturn)
        G.Blocks[BI].Terminator = N;
      if (Op.Opcode == NdOp::INDIR_BR)
        G.Blocks[BI].Gaps |= FlowIndirectControl;
    }
  }
  for (const auto &Clobber : F.CallClobbers) {
    Charge();
    const auto Call = Calls.find(Clobber.CallSiteId);
    if (Call == Calls.end())
      throw std::invalid_argument("dataflow clobber has no owning call");
    Define(Clobber.Value, Call->second);
    if (Clobber.PreservedPrefixSize)
      Inputs[Call->second].emplace_back(-1, Clobber.PreservedInput);
  }
  for (uint32_t N = 0; N < Inputs.size(); ++N)
    for (const auto &[Pred, V] : Inputs[N]) {
      Charge();
      FlowOperand I;
      I.Bytes = V.Size;
      if (Pred >= 0)
        I.Predecessor = Block(Pred);
      else if (G.Nodes[N].Phi)
        G.Nodes[N].Gaps |= FlowUnknownInput;
      if (V.isConst()) {
        I.Type = FlowOperand::Kind::Constant;
        I.Value = V.ConstVal;
        I.Provenance = uint8_t(V.Provenance);
        I.AddressOwner = V.AddressOwnerVA;
      } else if (V.Kind == MedVar::Param && Parameter(V) != NoIndex) {
        // Params are immutable incoming values, even when their synthetic ID
        // collides with a local. Register-backed params use physical identity.
        I.Type = FlowOperand::Kind::Parameter;
        I.Value = Parameter(V);
      } else if (auto It = Definitions.find(key(V)); It != Definitions.end()) {
        I.Type = FlowOperand::Kind::Node;
        I.Node = It->second;
      } else if (V.SSAVer == 0 && Parameter(V) != NoIndex) {
        I.Type = FlowOperand::Kind::Parameter;
        I.Value = Parameter(V);
      } else {
        I.Type = FlowOperand::Kind::Unknown;
        G.Nodes[N].Gaps |= FlowUnknownInput;
      }
      G.Nodes[N].Inputs.push_back(I);
    }
  if (auto State = Control.stop())
    throw DataFlowInterrupted(*State);
  if (auto Error = G.validate(Options))
    throw std::invalid_argument(*Error);
  return G;
}

} // namespace neverd::diff
