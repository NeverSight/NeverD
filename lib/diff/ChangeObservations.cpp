//===- ChangeObservations.cpp - Native/SSA patch review facts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local observations retain native alignment uncertainty and SSA boundaries.
/// Dependency context never establishes executable call or memory effects.
//===----------------------------------------------------------------------===//

#include "neverd/diff/ChangeObservations.h"

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {
namespace {
struct Stopped {
  ComparisonState State;
};
class Budget {
  FunctionChangeObservations &Out;
  const ChangeObservationOptions &Options;
  const ComparisonControl &Control;

public:
  Budget(FunctionChangeObservations &Out,
         const ChangeObservationOptions &Options,
         const ComparisonControl &Control)
      : Out(Out), Options(Options), Control(Control) {}
  void poll() const {
    if (const auto Stop = Control.stop())
      throw Stopped{*Stop};
  }
  void charge(uint64_t Count = 1) {
    if (Count > Options.MaxWork - Out.Work)
      throw Stopped{ComparisonState::ResourceLimited};
    const auto Before = Out.Work;
    Out.Work += Count;
    if (Before / limits::diff::kObservationPollInterval !=
        Out.Work / limits::diff::kObservationPollInterval)
      poll();
  }
};
void require(bool Value, const char *Reason) {
  if (!Value)
    throw std::invalid_argument(Reason);
}
struct NativeScope {
  std::set<uint32_t> Instructions;
  std::set<uint64_t> Addresses;
};
NativeScope nativeScope(const Program &P, uint32_t Index, Budget &Work) {
  require(Index < P.Functions.size(), "observation function is out of range");
  const auto &F = P.Functions[Index];
  require(F.FirstBlock <= P.Blocks.size() &&
              F.BlockCount <= P.Blocks.size() - F.FirstBlock,
          "observation function has invalid blocks");
  NativeScope Scope;
  for (const auto &B : P.blocks(F)) {
    Work.charge();
    require(B.FirstInstruction <= P.Instructions.size() &&
                B.InstructionCount <=
                    P.Instructions.size() - B.FirstInstruction,
            "observation block has invalid instructions");
    for (uint32_t I = 0; I < B.InstructionCount; ++I) {
      Work.charge();
      const auto N = B.FirstInstruction + I;
      require(Scope.Instructions.insert(N).second &&
                  Scope.Addresses.insert(P.Instructions[N].Address).second,
              "observation function repeats an instruction");
    }
  }
  return Scope;
}
bool call(const FlowNode &N) {
  return N.Opcode == "CALL" || N.Opcode == "INDIR_CALL";
}
llvm::StringRef family(const FlowNode &N) {
  const llvm::StringRef Op = N.Opcode;
  if (Op == "INT_LESS" || Op == "INT_SLESS" || Op == "INT_LESSEQUAL" ||
      Op == "INT_SLESSEQUAL")
    return "integer_order";
  if (Op == "INT_EQUAL" || Op == "INT_NOTEQUAL")
    return "integer_equality";
  if (Op == "INT_ZEXT" || Op == "INT_SEXT")
    return "integer_extension";
  if (call(N))
    return "call";
  return Op;
}
bool signedOperation(const FlowNode &N) {
  return N.Opcode == "INT_SLESS" || N.Opcode == "INT_SLESSEQUAL" ||
         N.Opcode == "INT_SEXT";
}
llvm::ArrayRef<FlowOperand> localInputs(const FlowNode &N) {
  llvm::ArrayRef<FlowOperand> Inputs = N.Inputs;
  if (N.Opcode == "COND_BR")
    return Inputs.take_back(1);
  if (N.Opcode == "BR" || N.Opcode == "INDIR_BR")
    return {};
  if (!call(N))
    return Inputs;
  return N.CallArgumentBegin == NoIndex
             ? llvm::ArrayRef<FlowOperand>()
             : Inputs.slice(N.CallArgumentBegin, N.CallArgumentCount);
}
struct Candidate {
  uint32_t Node = NoIndex, Count = 0;
};
struct GraphView {
  const DataFlowGraph *Graph = nullptr;
  std::vector<uint32_t> Contexts;
  std::map<uint64_t, std::map<std::string, Candidate>> Origins;
  uint32_t Gaps = 0;
};
GraphView graphView(const DataFlowGraph *G, const Program &P, uint32_t F,
                    const NativeScope &Scope, const DataFlowOptions &Options,
                    Budget &Work) {
  GraphView View;
  if (!G || G->Profile != DataFlowProfile)
    return View;
  require(G->Function == P.Functions[F].Address &&
              G->Revision == P.Identity.Revision && G->ABI == P.Identity.ABI,
          "observation graph is not bound to the native function");
  Work.charge(G->Nodes.size() + G->Blocks.size());
  for (const auto &N : G->Nodes)
    Work.charge(N.Inputs.size());
  for (const auto &B : G->Blocks)
    Work.charge(B.Successors.size());
  if (const auto Error = G->validate(Options))
    throw std::invalid_argument(*Error);
  View.Graph = G;
  View.Gaps = G->Gaps;
  View.Contexts.resize(G->Nodes.size());
  std::deque<std::pair<uint32_t, uint32_t>> Pending;
  const auto Mark = [&](uint32_t N, uint32_t Context) {
    const auto New = Context & ~View.Contexts[N];
    if (New) {
      View.Contexts[N] |= New;
      Pending.emplace_back(N, New);
    }
  };
  for (uint32_t I = 0; I < G->Nodes.size(); ++I) {
    Work.charge();
    const auto &N = G->Nodes[I];
    View.Gaps |= N.Gaps;
    if (N.OriginSequence >= 0) {
      require(Scope.Addresses.count(N.Address),
              "observation graph has a foreign native origin");
      auto &C = View.Origins[N.Address][family(N).str()];
      C.Node = I;
      ++C.Count;
    }
    if (N.Opcode == "COND_BR")
      Mark(I, ContextPredicate);
    else if (N.Opcode == "RETURN")
      Mark(I, ContextReturn);
    else if (N.Opcode == "STORE")
      Mark(I, ContextMemoryWrite);
    else if (call(N) && N.CallArgumentBegin != NoIndex)
      for (const auto &Input : localInputs(N)) {
        Work.charge();
        if (Input.Type == FlowOperand::Kind::Node)
          Mark(Input.Node, ContextCallArgument);
      }
  }
  for (const auto &B : G->Blocks)
    View.Gaps |= B.Gaps;
  while (!Pending.empty()) {
    const auto [N, Context] = Pending.front();
    Pending.pop_front();
    Work.charge();
    for (const auto &Input : G->Nodes[N].Inputs) {
      Work.charge();
      if (Input.Type == FlowOperand::Kind::Node)
        Mark(Input.Node, Context);
    }
  }
  return View;
}
using Constant = std::pair<uint32_t, uint64_t>;
std::vector<Constant> constants(const FlowNode &N, Budget &Work) {
  std::vector<Constant> Values;
  const auto Inputs = localInputs(N);
  for (uint32_t I = 0; I < Inputs.size(); ++I) {
    Work.charge();
    const auto &V = Inputs[I];
    if (V.Type == FlowOperand::Kind::Constant &&
        (V.Provenance == uint8_t(ConstantAddressProvenance::Unknown) ||
         V.Provenance == uint8_t(ConstantAddressProvenance::Scalar)))
      Values.emplace_back(N.Commutative ? 0 : I, V.Value);
  }
  llvm::sort(Values);
  return Values;
}
std::vector<uint16_t> widths(const FlowNode &N, Budget &Work) {
  std::vector<uint16_t> Values;
  for (const auto &V : localInputs(N)) {
    Work.charge();
    Values.push_back(V.Bytes);
  }
  if (N.Commutative)
    llvm::sort(Values);
  return Values;
}
// Only direct synthetic arithmetic operands are associated through the native
// anchor. Their addresses are display hints, never original-code attribution.
bool syntheticOperand(const FlowNode &N) {
  const llvm::StringRef Op = N.Opcode;
  return N.OriginSequence < 0 && !N.Phi && !N.Gaps &&
         (Op.starts_with("INT_") || Op.starts_with("BOOL_") ||
          Op.starts_with("FLOAT_") || Op == "COPY" || Op == "SUBBYTES" ||
          Op == "CONCAT");
}
ChangeObservation localChange(uint32_t Row, const GraphView &A, uint32_t AN,
                              const GraphView &B, uint32_t BN, Budget &Work) {
  const auto &X = A.Graph->Nodes[AN], &Y = B.Graph->Nodes[BN];
  ChangeObservation O;
  O.Row = Row;
  O.PrimaryNode = AN;
  O.SecondaryNode = BN;
  O.PrimaryContext = A.Contexts[AN];
  O.SecondaryContext = B.Contexts[BN];
  O.Kinds = 0;
  if (X.Opcode != Y.Opcode)
    O.Kinds |= ObserveOperation;
  if (signedOperation(X) != signedOperation(Y))
    O.Kinds |= ObserveSignedness;
  const bool ArgumentsKnown = !call(X) || (X.CallArgumentBegin != NoIndex &&
                                           Y.CallArgumentBegin != NoIndex);
  if (ArgumentsKnown) {
    const auto XC = constants(X, Work), YC = constants(Y, Work);
    if (XC.size() == YC.size() &&
        std::equal(
            XC.begin(), XC.end(), YC.begin(),
            [](const auto &L, const auto &R) { return L.first == R.first; }) &&
        XC != YC)
      O.Kinds |= ObserveConstant;
    const auto XW = widths(X, Work), YW = widths(Y, Work);
    if (X.ResultBytes != Y.ResultBytes || (XW.size() == YW.size() && XW != YW))
      O.Kinds |= ObserveWidth;
    if (call(X)) {
      O.PrimaryContext |= ContextCallArgument;
      O.SecondaryContext |= ContextCallArgument;
      if (X.CallArgumentCount != Y.CallArgumentCount)
        O.Kinds |= ObserveCallArguments;
    }
  }
  return O;
}
unsigned priority(const ChangeObservation &O) {
  using namespace limits::diff;
  unsigned P = kObservationNativePriority;
  const auto Raise = [&](bool Apply, unsigned Value) {
    if (Apply)
      P = std::max(P, Value);
  };
  Raise(O.Kinds & ObserveConstant, kObservationConstantPriority);
  Raise(O.Kinds & ObserveWidth, kObservationWidthPriority);
  Raise(O.Kinds & ObserveSignedness, kObservationSignednessPriority);
  const auto C = O.PrimaryContext | O.SecondaryContext;
  Raise(C & ContextPredicate, kObservationPredicatePriority);
  Raise(C & ContextReturn, kObservationReturnPriority);
  Raise(C & ContextCallArgument, kObservationCallArgumentPriority);
  Raise(C & ContextMemoryWrite, kObservationMemoryWritePriority);
  return P;
}
} // namespace

std::optional<std::string> ChangeObservationOptions::validate() const {
  if (!MaxRows || MaxRows > limits::diff::kMaxObservationRows ||
      !MaxObservations || MaxObservations > limits::diff::kMaxObservations ||
      !MaxWork || MaxWork > limits::diff::kMaxObservationWork)
    return "change-observation allowance is out of range";
  return Graph.validate();
}
std::vector<llvm::StringRef> observationKindNames(uint32_t Mask) {
  std::vector<llvm::StringRef> Names;
  for (auto [Bit, Name] :
       {std::pair{ObserveNativeInstruction, "native_instruction"},
        {ObserveConstant, "constant"},
        {ObserveOperation, "operation"},
        {ObserveWidth, "operand_width"},
        {ObserveSignedness, "signedness"},
        {ObserveCallArguments, "recovered_call_arguments"}})
    if (Mask & Bit)
      Names.push_back(Name);
  return Names;
}
std::vector<llvm::StringRef> observationContextNames(uint32_t Mask) {
  std::vector<llvm::StringRef> Names;
  for (auto [Bit, Name] : {std::pair{ContextPredicate, "predicate_dependency"},
                           {ContextReturn, "return_dependency"},
                           {ContextCallArgument, "call_argument_dependency"},
                           {ContextMemoryWrite, "memory_write_dependency"}})
    if (Mask & Bit)
      Names.push_back(Name);
  return Names;
}
FunctionChangeObservations observeFunctionChanges(
    const Program &Primary, uint32_t PF, const Program &Secondary, uint32_t SF,
    const FunctionDelta &Delta, const DataFlowGraph *PrimaryGraph,
    const DataFlowGraph *SecondaryGraph,
    const ChangeObservationOptions &Options, const ComparisonControl &Control) {
  if (const auto Error = Options.validate())
    throw std::invalid_argument(*Error);
  require(Delta.ComputationComplete ==
              (Delta.State == ComparisonState::Complete),
          "observation delta has inconsistent completion state");
  FunctionChangeObservations Out;
  Out.State = Delta.State;
  Out.TotalRows = Delta.Rows.size();
  Out.NativeAnalysisComplete = Delta.AnalysisComplete;
  Out.ReferenceComparisonComplete = Delta.ReferenceComparisonComplete;
  if (!Delta.ComputationComplete)
    return Out;
  Budget Work(Out, Options, Control);
  try {
    Work.poll();
    const auto AScope = nativeScope(Primary, PF, Work);
    const auto BScope = nativeScope(Secondary, SF, Work);
    if (PrimaryGraph && SecondaryGraph)
      require(PrimaryGraph->ABI == SecondaryGraph->ABI &&
                  PrimaryGraph->Profile == SecondaryGraph->Profile,
              "observation SSA profiles or ABIs differ");
    const auto A =
        graphView(PrimaryGraph, Primary, PF, AScope, Options.Graph, Work);
    const auto B =
        graphView(SecondaryGraph, Secondary, SF, BScope, Options.Graph, Work);
    Out.PrimarySSA = A.Graph != nullptr;
    Out.SecondarySSA = B.Graph != nullptr;
    Out.PrimaryGaps = A.Gaps;
    Out.SecondaryGaps = B.Gaps;
    if (Primary.Functions[PF].has(TraitIncomplete))
      Out.PrimaryGaps |= FlowNativeCoverage;
    if (Secondary.Functions[SF].has(TraitIncomplete))
      Out.SecondaryGaps |= FlowNativeCoverage;
    std::set<uint32_t> SeenA, SeenB;
    const auto Add = [&](ChangeObservation O) {
      if (Out.Observations.size() == Options.MaxObservations)
        throw Stopped{ComparisonState::ResourceLimited};
      O.Priority = priority(O);
      O.PrimaryInstruction = Delta.Rows[O.Row].Primary;
      O.SecondaryInstruction = Delta.Rows[O.Row].Secondary;
      O.NativeKind = Delta.Rows[O.Row].Kind;
      Out.Observations.push_back(O);
    };
    for (uint32_t I = 0; I < Delta.Rows.size(); ++I) {
      Work.poll();
      if (Out.VisitedRows == Options.MaxRows)
        throw Stopped{ComparisonState::ResourceLimited};
      Work.charge();
      const auto &R = Delta.Rows[I];
      require(unsigned(R.Kind) <= unsigned(RowKind::Uncertain),
              "observation delta row has an invalid kind");
      require(R.Primary != NoIndex || R.Secondary != NoIndex,
              "observation delta row has no instruction");
      require(
          R.Primary == NoIndex || (AScope.Instructions.count(R.Primary) &&
                                   SeenA.insert(R.Primary).second),
          "observation delta has a foreign or repeated primary instruction");
      require(
          R.Secondary == NoIndex || (BScope.Instructions.count(R.Secondary) &&
                                     SeenB.insert(R.Secondary).second),
          "observation delta has a foreign or repeated secondary instruction");
      const auto Before = Out.Observations.size();
      if (A.Graph && B.Graph && R.Primary != NoIndex &&
          R.Secondary != NoIndex) {
        const auto AX = A.Origins.find(Primary.Instructions[R.Primary].Address);
        const auto BX =
            B.Origins.find(Secondary.Instructions[R.Secondary].Address);
        if (AX != A.Origins.end() && BX != B.Origins.end())
          for (const auto &[Family, AC] : AX->second) {
            Work.charge();
            const auto It = BX->second.find(Family);
            if (It == BX->second.end())
              continue;
            const auto &BC = It->second;
            if (AC.Count != 1 || BC.Count != 1) {
              ++Out.AmbiguousOrigins;
              continue;
            }
            auto O = localChange(I, A, AC.Node, B, BC.Node, Work);
            O.PrimaryAnchorNode = AC.Node;
            O.SecondaryAnchorNode = BC.Node;
            if (O.Kinds)
              Add(O);
            const auto &X = A.Graph->Nodes[AC.Node];
            const auto &Y = B.Graph->Nodes[BC.Node];
            const auto XI = localInputs(X), YI = localInputs(Y);
            // Exactly one node operand on each side gives an unambiguous
            // direct dependency. Ordered slots must agree; commutative slots
            // may exchange. Multiple node operands deliberately abstain.
            uint32_t XNode = NoIndex, YNode = NoIndex, XSlot = 0, YSlot = 0;
            unsigned XCount = 0, YCount = 0;
            for (uint32_t J = 0; J < XI.size(); ++J) {
              Work.charge();
              if (XI[J].Type == FlowOperand::Kind::Node) {
                XNode = XI[J].Node;
                XSlot = J;
                ++XCount;
              }
            }
            for (uint32_t J = 0; J < YI.size(); ++J) {
              Work.charge();
              if (YI[J].Type == FlowOperand::Kind::Node) {
                YNode = YI[J].Node;
                YSlot = J;
                ++YCount;
              }
            }
            if (XCount != 1 || YCount != 1 ||
                (XSlot != YSlot && !(X.Commutative && Y.Commutative)))
              continue;
            const auto &XC = A.Graph->Nodes[XNode], &YC = B.Graph->Nodes[YNode];
            if (!syntheticOperand(XC) || !syntheticOperand(YC) ||
                family(XC) != family(YC))
              continue;
            O = localChange(I, A, XNode, B, YNode, Work);
            O.PrimaryAnchorNode = AC.Node;
            O.SecondaryAnchorNode = BC.Node;
            if (O.Kinds)
              Add(O);
          }
      }
      if (Out.Observations.size() == Before && R.Kind != RowKind::Identical &&
          R.Kind != RowKind::Moved && R.Kind != RowKind::Uncertain) {
        ChangeObservation O;
        O.Row = I;
        if (R.Kind == RowKind::Constant)
          O.Kinds |= ObserveConstant;
        Add(O);
      }
      ++Out.VisitedRows;
    }
    Work.poll();
  } catch (const Stopped &Stop) {
    Out.State = Stop.State;
  }
  llvm::sort(Out.Observations, [](const auto &A, const auto &B) {
    if (A.Priority != B.Priority)
      return A.Priority > B.Priority;
    return std::tie(A.Row, A.PrimaryNode, A.SecondaryNode) <
           std::tie(B.Row, B.PrimaryNode, B.SecondaryNode);
  });
  return Out;
}
} // namespace neverd::diff
