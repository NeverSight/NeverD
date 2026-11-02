//===- DataFlowJSON.cpp - Dependency evidence records and bounded pages ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original addresses and scalar bits use hexadecimal strings without loss
/// through JSON number conversion. Unknown or oversized evidence is refused.
//===----------------------------------------------------------------------===//

#include "neverd/diff/DataFlowJSON.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <bit>
#include <initializer_list>
#include <stdexcept>

namespace neverd::diff {
namespace {
namespace limits = neverd::limits::diff;
using Object = llvm::json::Object;
using Array = llvm::json::Array;
using Value = llvm::json::Value;
void require(bool Valid, llvm::StringRef Error) {
  if (!Valid)
    throw std::invalid_argument("dataflow: " + Error.str());
}
const Object &object(const Value &V) {
  require(V.getAsObject(), "expected object");
  return *V.getAsObject();
}
const Value &field(const Object &O, llvm::StringRef Key) {
  require(O.get(Key), "missing field " + Key.str());
  return *O.get(Key);
}
void fields(const Object &O, std::initializer_list<llvm::StringRef> Keys) {
  for (const auto &[Key, Ignored] : O) {
    (void)Ignored;
    require(llvm::is_contained(Keys, llvm::StringRef(Key)),
            "unknown field " + Key.str());
  }
}
uint64_t count(const Value &V, uint64_t Maximum = UINT32_MAX) {
  const auto N = V.getAsInteger();
  require(N && *N >= 0 && uint64_t(*N) <= Maximum, "invalid bounded integer");
  return uint64_t(*N);
}
uint64_t count(const Object &O, llvm::StringRef Key, uint64_t Default,
               uint64_t Maximum) {
  return O.get(Key) ? count(*O.get(Key), Maximum) : Default;
}
bool boolean(const Value &V) {
  const auto B = V.getAsBoolean();
  require(B.has_value(), "expected boolean");
  return *B;
}
std::string text(const Value &V, size_t Maximum) {
  const auto S = V.getAsString();
  require(S && S->size() <= Maximum, "invalid or oversized string");
  return S->str();
}
const Array &array(const Value &V, uint64_t Maximum) {
  const auto *A = V.getAsArray();
  require(A && A->size() <= Maximum, "invalid or oversized array");
  return *A;
}
std::string hex(uint64_t V) { return "0x" + llvm::utohexstr(V, true); }
uint64_t address(const Value &V) {
  const auto T = text(V, 18);
  llvm::StringRef S(T);
  uint64_t N = 0;
  require(S.consume_front("0x") && !S.empty() && !S.getAsInteger(16, N),
          "expected hexadecimal bits/address");
  return N;
}
Value index(uint32_t I) { return I == NoIndex ? Value(nullptr) : Value(I); }
uint32_t index(const Value &V) {
  return V.kind() == Value::Null ? NoIndex : count(V, NoIndex - 1);
}
Array indices(llvm::ArrayRef<uint32_t> Values) {
  Array Out;
  for (auto I : Values)
    Out.push_back(I);
  return Out;
}
Array gaps(uint32_t Mask) {
  Array Out;
  for (auto Name : flowGapNames(Mask))
    Out.push_back(Name);
  return Out;
}
Object node(const FlowNode &N, bool Extended) {
  Array Inputs;
  for (const auto &I : N.Inputs) {
    Array Input{uint8_t(I.Type),     index(I.Node), I.Bytes,
                hex(I.Value),        I.Provenance,  hex(I.AddressOwner),
                index(I.Predecessor)};
    if (Extended)
      Input.push_back(I.Semantics);
    Inputs.push_back(std::move(Input));
  }
  Object Out{{"opcode", N.Opcode},
             {"result_bytes", N.ResultBytes},
             {"memory_ordering", N.MemoryOrdering},
             {"memory_address_space", N.MemoryAddressSpace},
             {"block", N.Block},
             {"address", hex(N.Address)},
             {"origin_sequence", N.OriginSequence},
             {"phi", N.Phi},
             {"commutative", N.Commutative},
             {"gap_mask", N.Gaps},
             {"call_argument_begin", index(N.CallArgumentBegin)},
             {"call_argument_count", N.CallArgumentCount},
             {"inputs", std::move(Inputs)}};
  if (Extended) {
    Out["semantics"] = N.Semantics;
    Array Origins;
    for (const auto &O : N.Origins)
      Origins.push_back(
          Object{{"address", hex(O.Address)}, {"sequence", O.Sequence}});
    Out["origins"] = std::move(Origins);
  }
  return Out;
}
Object block(const FlowBlock &B) {
  return Object{{"begin", hex(B.Begin)},
                {"end", hex(B.End)},
                {"terminator", index(B.Terminator)},
                {"successors", indices(B.Successors)},
                {"gap_mask", B.Gaps}};
}
Object parameterView(const FlowParameterView &V) {
  Object Out{{"source_bits", V.SourceBits},
             {"view_bits", V.ViewBits},
             {"attributes", V.Attributes},
             {"elided_truncations", V.ElidedTruncations},
             {"native_site", nullptr}};
  if (V.NativeSite)
    Out["native_site"] = Object{{"register", hex(V.NativeSite->Register)},
                                {"index", hex(uint64_t(V.NativeSite->Index))}};
  return Out;
}
Object loopProgress(const FlowLoopProgress &L) {
  Object Out{{"block", L.Block},
             {"counter", L.Counter},
             {"update", L.Update},
             {"compare", L.Compare}};
  if (L.Header != NoIndex)
    Out["header"] = L.Header;
  if (L.Latch != NoIndex)
    Out["latch"] = L.Latch;
  if (L.Proof != FlowCounterProof::None)
    Out["proof"] = flowCounterProofName(L.Proof);
  return Out;
}
Object progress(const FlowProgress &P, llvm::StringRef Profile) {
  Object Out{{"kind", flowProgressName(P.Kind)},
             {"source_contract", P.SourceContract},
             {"observed_contract", P.ObservedContract},
             {"loop_count", P.LoopCount}};
  if (Profile == LLVMBranchProgressDataFlowProfile ||
      Profile == LLVMModularProgressDataFlowProfile ||
      Profile == LLVMParameterBoundProgressDataFlowProfile ||
      Profile == LLVMInvariantBoundProgressDataFlowProfile) {
    Out["backedge_count"] = P.BackedgeCount;
    Out["guard_count"] = P.Loops.size();
  }
  return Out;
}
uint64_t projectedParameters(const DataFlowGraph &G) {
  return llvm::count_if(G.ParameterViews,
                        [](const auto &V) { return V.ElidedTruncations != 0; });
}
Object metadata(const DataFlowGraph &G) {
  Object Out{{"schema", "neverd.bindiff.dataflow"},
             {"schema_version", dataFlowSchemaVersion(G.Profile)},
             {"profile", G.Profile},
             {"revision", G.Revision},
             {"abi", G.ABI},
             {"function", hex(G.Function)},
             {"entry_block", G.EntryBlock},
             {"gap_mask", G.Gaps},
             {"node_count", G.Nodes.size()},
             {"block_count", G.Blocks.size()}};
  if (hasParameterViews(G.Profile)) {
    Out["parameter_count"] = G.ParameterViews.size();
    Out["projected_parameter_count"] = projectedParameters(G);
  }
  if (G.Progress)
    Out["progress"] = progress(*G.Progress, G.Profile);
  return Out;
}
void page(unsigned Limit) {
  require(Limit && Limit <= limits::kMaxPageSize,
          "page limit is outside the admitted range");
}
} // namespace

Object dataFlowOptionsJSON(const DataFlowOptions &O) {
  return Object{{"max_nodes", O.MaxNodes},
                {"max_blocks", O.MaxBlocks},
                {"max_work", O.MaxWork},
                {"rounds", O.Rounds}};
}
DataFlowOptions readDataFlowOptions(const Value &V) {
  const auto &O = object(V);
  for (const auto &[Key, Ignored] : O) {
    (void)Ignored;
    require(Key == "max_nodes" || Key == "max_blocks" || Key == "max_work" ||
                Key == "rounds",
            "unknown setting " + Key.str());
  }
  DataFlowOptions Out;
  Out.MaxNodes = count(O, "max_nodes", Out.MaxNodes, limits::kMaxFlowNodes);
  Out.MaxBlocks = count(O, "max_blocks", Out.MaxBlocks, limits::kMaxFlowBlocks);
  Out.MaxWork = count(O, "max_work", Out.MaxWork, limits::kMaxFlowWork);
  Out.Rounds = count(O, "rounds", Out.Rounds, limits::kMaxFlowRounds);
  if (auto E = Out.validate())
    throw std::invalid_argument(*E);
  return Out;
}
Object dataFlowSettingsJSON(const DataFlowSettings &S) {
  Object Out{{"enabled", S.Enabled},
             {"max_functions", S.MaxFunctions},
             {"max_native_instructions", S.MaxNativeInstructions},
             {"max_total_nodes", S.MaxTotalNodes},
             {"graph", dataFlowOptionsJSON(S.Graph)}};
  if (S.Profile != DataFlowProfile)
    Out["profile"] = S.Profile;
  return Out;
}
DataFlowSettings readDataFlowSettings(const Value &V) {
  DataFlowSettings S;
  if (const auto Enabled = V.getAsBoolean()) {
    S.Enabled = *Enabled;
    return S;
  }
  const auto &O = object(V);
  for (const auto &[Key, Ignored] : O) {
    (void)Ignored;
    require(Key == "enabled" || Key == "max_functions" ||
                Key == "max_native_instructions" || Key == "max_total_nodes" ||
                Key == "graph" || Key == "profile",
            "unknown collection setting " + Key.str());
  }
  S.Enabled = O.get("enabled") ? boolean(*O.get("enabled")) : true;
  if (const auto *P = O.get("profile"))
    S.Profile = text(*P, limits::kMaxFlowIdentityBytes);
  S.MaxFunctions =
      count(O, "max_functions", S.MaxFunctions, limits::kMaxFlowFunctions);
  S.MaxNativeInstructions =
      count(O, "max_native_instructions", S.MaxNativeInstructions,
            limits::kMaxFlowNativeInstructions);
  S.MaxTotalNodes =
      count(O, "max_total_nodes", S.MaxTotalNodes, limits::kMaxFlowTotalNodes);
  if (const auto *G = O.get("graph"))
    S.Graph = readDataFlowOptions(*G);
  if (auto Error = S.validate())
    throw std::invalid_argument(*Error);
  return S;
}
Object dataFlowEvidenceSummary(const DataFlowEvidence *E) {
  Object Out{{"requested", bool(E)}, {"semantic_validation", "not_performed"}};
  if (!E)
    return Out;
  uint64_t Available = 0, Unsupported = 0, Limited = 0, Failed = 0;
  for (const auto &Row : E->Entries) {
    Available += Row.Availability == FlowAvailability::Available;
    Unsupported += Row.Availability == FlowAvailability::Unsupported;
    Limited += Row.Availability == FlowAvailability::ResourceLimited;
    Failed += Row.Availability == FlowAvailability::Failed;
  }
  Out["state"] = stateName(E->State);
  Out["recipe"] = E->Recipe;
  Out["eligible_functions"] = E->EligibleFunctions;
  Out["processed_functions"] = E->Entries.size();
  Out["unprocessed_functions"] = E->EligibleFunctions - E->Entries.size();
  Out["available"] = Available;
  Out["unsupported"] = Unsupported;
  Out["resource_limited"] = Limited;
  Out["failed"] = Failed;
  Out["nodes"] = E->nodes();
  Out["pipeline_cancellation"] = "between_functions";
  return Out;
}
void writeDataFlowEvidence(llvm::json::OStream &Out,
                           const DataFlowEvidence *E) {
  if (!E) {
    Out.value(nullptr);
    return;
  }
  Out.object([&] {
    Out.attribute("state", stateName(E->State));
    Out.attribute("recipe", E->Recipe);
    Out.attribute("eligible_functions", E->EligibleFunctions);
    Out.attributeArray("entries", [&] {
      for (const auto &Row : E->Entries)
        Out.object([&] {
          Out.attribute("function", hex(Row.Function));
          Out.attribute("availability", availabilityName(Row.Availability));
          Out.attribute("reason", Row.Reason);
          Out.attributeBegin("graph");
          if (Row.Graph)
            writeDataFlowGraph(Out, *Row.Graph);
          else
            Out.value(nullptr);
          Out.attributeEnd();
        });
    });
  });
}
std::shared_ptr<const DataFlowEvidence>
readDataFlowEvidence(const Value &V, const DataFlowSettings &S,
                     const ComparisonControl &Control) {
  Control.check();
  if (auto Error = S.validate())
    throw std::invalid_argument(*Error);
  if (V.kind() == Value::Null) {
    require(!S.Enabled, "enabled collection has no saved evidence");
    return {};
  }
  require(S.Enabled, "disabled collection has saved evidence");
  const auto &O = object(V);
  auto E = std::make_shared<DataFlowEvidence>();
  const auto State =
      stateNamed(text(field(O, "state"), limits::kMaxFlowIdentityBytes));
  require(State.has_value(), "unknown collection state");
  E->State = *State;
  E->Recipe = text(field(O, "recipe"), limits::kMaxFlowIdentityBytes);
  E->EligibleFunctions = count(field(O, "eligible_functions"));
  uint64_t Nodes = 0;
  for (const auto &RowValue : array(field(O, "entries"), S.MaxFunctions)) {
    Control.check();
    const auto &Row = object(RowValue);
    DataFlowEntry Entry;
    Entry.Function = address(field(Row, "function"));
    const auto Availability = availabilityNamed(
        text(field(Row, "availability"), limits::kMaxFlowIdentityBytes));
    require(Availability.has_value(), "unknown function availability");
    Entry.Availability = *Availability;
    Entry.Reason = text(field(Row, "reason"), limits::kMaxFlowReasonBytes);
    const auto &Graph = field(Row, "graph");
    if (Graph.kind() != Value::Null) {
      require(Nodes < S.MaxTotalNodes, "collection node allowance exceeded");
      auto Options = S.Graph;
      Options.MaxNodes =
          std::min<uint64_t>(Options.MaxNodes, S.MaxTotalNodes - Nodes);
      Entry.Graph = readDataFlowGraph(Graph, Options, Control);
      Nodes += Entry.Graph->retainedItems();
    }
    require(bool(Entry.Graph) ==
                    (Entry.Availability == FlowAvailability::Available) &&
                (Entry.Graph ? Entry.Reason.empty() : !Entry.Reason.empty()),
            "availability disagrees with graph/reason");
    require(E->Entries.empty() || E->Entries.back().Function < Entry.Function,
            "collection entries are not in address order");
    E->Entries.push_back(std::move(Entry));
  }
  require(E->EligibleFunctions >= E->Entries.size() &&
              (E->State != ComparisonState::Complete ||
               E->EligibleFunctions == E->Entries.size()),
          "collection coverage is inconsistent");
  return E;
}
Object dataFlowEntriesJSON(const DataFlowEvidence &E, uint64_t Offset,
                           unsigned Limit) {
  page(Limit);
  auto Out = dataFlowEvidenceSummary(&E);
  Array Rows;
  for (uint64_t I = Offset; I < E.Entries.size() && I - Offset < Limit; ++I) {
    const auto &Row = E.Entries[I];
    Rows.push_back(
        Object{{"function", hex(Row.Function)},
               {"availability", availabilityName(Row.Availability)},
               {"reason", Row.Reason},
               {"nodes", Row.Graph ? Row.Graph->Nodes.size() : 0},
               {"blocks", Row.Graph ? Row.Graph->Blocks.size() : 0}});
  }
  Out["offset"] = Offset;
  Out["total"] = E.Entries.size();
  Out["rows"] = std::move(Rows);
  return Out;
}
void writeDataFlowGraph(llvm::json::OStream &Out, const DataFlowGraph &G) {
  Out.object([&] {
    auto Header = metadata(G);
    if (G.Progress) {
      Array Loops;
      for (const auto &L : G.Progress->Loops)
        Loops.push_back(loopProgress(L));
      (*Header.getObject("progress"))["loops"] = std::move(Loops);
    }
    std::vector<std::string> Keys;
    for (const auto &[Key, Ignored] : Header) {
      (void)Ignored;
      Keys.push_back(Key.str());
    }
    std::sort(Keys.begin(), Keys.end());
    for (const auto &Key : Keys)
      Out.attribute(Key, *Header.get(Key));
    if (hasParameterViews(G.Profile))
      Out.attributeArray("parameter_views", [&] {
        for (const auto &V : G.ParameterViews)
          Out.value(parameterView(V));
      });
    Out.attributeArray("blocks", [&] {
      for (const auto &B : G.Blocks)
        Out.value(block(B));
    });
    Out.attributeArray("nodes", [&] {
      for (const auto &N : G.Nodes)
        Out.value(node(N, isLLVMDataFlowProfile(G.Profile)));
    });
  });
}
DataFlowGraph readDataFlowGraph(const Value &V, const DataFlowOptions &Options,
                                const ComparisonControl &Control) {
  Control.check();
  if (auto Error = Options.validate())
    throw std::invalid_argument(*Error);
  const auto &O = object(V);
  DataFlowGraph G;
  G.Profile = text(field(O, "profile"), limits::kMaxFlowIdentityBytes);
  const bool Extended = isLLVMDataFlowProfile(G.Profile);
  const bool Parameters = hasParameterViews(G.Profile);
  const bool Progress = hasProgressProof(G.Profile);
  if (!Progress)
    require(!O.get("progress"), "legacy graph contains progress evidence");
  if (Progress)
    fields(O, {"schema", "schema_version", "profile", "revision", "abi",
               "function", "entry_block", "gap_mask", "node_count",
               "block_count", "parameter_count", "projected_parameter_count",
               "parameter_views", "progress", "blocks", "nodes"});
  else if (Parameters)
    fields(O, {"schema", "schema_version", "profile", "revision", "abi",
               "function", "entry_block", "gap_mask", "node_count",
               "block_count", "parameter_count", "projected_parameter_count",
               "parameter_views", "blocks", "nodes"});
  else
    require(!O.get("parameter_views") && !O.get("parameter_count") &&
                !O.get("projected_parameter_count"),
            "legacy graph contains parameter-view metadata");
  require(field(O, "schema").getAsString() == "neverd.bindiff.dataflow" &&
              count(field(O, "schema_version")) ==
                  dataFlowSchemaVersion(G.Profile),
          "unsupported saved graph schema");
  G.Revision = text(field(O, "revision"), limits::kMaxFlowIdentityBytes);
  G.ABI = text(field(O, "abi"), limits::kMaxFlowIdentityBytes);
  G.Function = address(field(O, "function"));
  G.EntryBlock = count(field(O, "entry_block"));
  G.Gaps = count(field(O, "gap_mask"), AllFlowGaps);
  uint64_t Edges = 0, ProgressRows = 0;
  if (Progress) {
    const auto &P = object(field(O, "progress"));
    const bool InvariantBound =
        G.Profile == LLVMInvariantBoundProgressDataFlowProfile;
    const bool ParameterBound =
        G.Profile == LLVMParameterBoundProgressDataFlowProfile ||
        InvariantBound;
    const bool Modular =
        G.Profile == LLVMModularProgressDataFlowProfile || ParameterBound;
    const bool Branched =
        G.Profile == LLVMBranchProgressDataFlowProfile || Modular;
    if (Branched)
      fields(P, {"kind", "source_contract", "observed_contract", "loop_count",
                 "backedge_count", "guard_count", "loops"});
    else
      fields(P, {"kind", "source_contract", "observed_contract", "loop_count",
                 "loops"});
    FlowProgress Record;
    const auto Kind = text(field(P, "kind"), limits::kMaxFlowIdentityBytes);
    if (Kind == "unproven")
      Record.Kind = FlowProgressKind::Unproven;
    else if (Kind == "acyclic")
      Record.Kind = FlowProgressKind::Acyclic;
    else {
      require(Kind == "counter_loops", "unknown progress proof kind");
      Record.Kind = FlowProgressKind::CounterLoops;
    }
    Record.SourceContract =
        text(field(P, "source_contract"), limits::kMaxFlowSemanticsBytes);
    Record.ObservedContract =
        text(field(P, "observed_contract"), limits::kMaxFlowSemanticsBytes);
    Edges += 1 + Record.SourceContract.size() + Record.ObservedContract.size();
    require(Edges <= Options.MaxWork, "progress exceeds work allowance");
    for (const auto &Row : array(field(P, "loops"), Options.MaxNodes - 1)) {
      Control.check();
      require(++Edges <= Options.MaxWork, "loop proofs exceed work allowance");
      const auto &L = object(Row);
      const bool MultiBlock =
          G.Profile == LLVMMultiBlockProgressDataFlowProfile ||
          G.Profile == LLVMNestedProgressDataFlowProfile || Branched;
      if (Modular)
        fields(L, {"block", "counter", "update", "compare", "header", "latch",
                   "proof"});
      else if (Branched)
        fields(L, {"block", "counter", "update", "compare", "header", "latch"});
      else if (MultiBlock)
        fields(L, {"block", "counter", "update", "compare", "header"});
      else
        fields(L, {"block", "counter", "update", "compare"});
      Record.Loops.push_back(
          {uint32_t(count(field(L, "block"))),
           uint32_t(count(field(L, "counter"))),
           uint32_t(count(field(L, "update"))),
           uint32_t(count(field(L, "compare"))),
           MultiBlock ? uint32_t(count(field(L, "header"))) : NoIndex,
           Branched ? uint32_t(count(field(L, "latch"))) : NoIndex});
      if (Modular) {
        const auto Proof =
            text(field(L, "proof"), limits::kMaxFlowIdentityBytes);
        require(Proof == "nowrap" || Proof == "modular" ||
                    (ParameterBound && Proof == "parameter_modular") ||
                    (InvariantBound && Proof == "invariant_modular"),
                "unknown counter proof basis");
        Record.Loops.back().Proof =
            Proof == "nowrap"              ? FlowCounterProof::NoWrap
            : Proof == "modular"           ? FlowCounterProof::Modular
            : Proof == "parameter_modular" ? FlowCounterProof::ParameterModular
                                           : FlowCounterProof::InvariantModular;
      }
    }
    Record.LoopCount = count(field(P, "loop_count"));
    Record.BackedgeCount =
        Branched ? count(field(P, "backedge_count")) : Record.LoopCount;
    require((Branched ? count(field(P, "guard_count")) : Record.LoopCount) ==
                Record.Loops.size(),
            "progress count disagrees with witnesses");
    ProgressRows = 1 + Record.Loops.size();
    G.Progress = std::move(Record);
  }
  if (Parameters) {
    for (const auto &Row :
         array(field(O, "parameter_views"), Options.MaxNodes - ProgressRows)) {
      Control.check();
      const auto &P = object(Row);
      fields(P, {"source_bits", "view_bits", "attributes", "native_site",
                 "elided_truncations"});
      FlowParameterView View;
      View.SourceBits = count(field(P, "source_bits"), 64);
      View.ViewBits = count(field(P, "view_bits"), 64);
      View.Attributes =
          text(field(P, "attributes"), limits::kMaxFlowSemanticsBytes);
      View.ElidedTruncations =
          count(field(P, "elided_truncations"), Options.MaxNodes);
      Edges += 1 + View.Attributes.size() + View.ElidedTruncations;
      require(Edges <= Options.MaxWork, "parameters exceed work allowance");
      const auto &Native = field(P, "native_site");
      if (Native.kind() != Value::Null) {
        const auto &Site = object(Native);
        fields(Site, {"register", "index"});
        View.NativeSite = FlowParameterSite{
            address(field(Site, "register")),
            std::bit_cast<int64_t>(address(field(Site, "index")))};
      }
      G.ParameterViews.push_back(std::move(View));
    }
    require(count(field(O, "parameter_count")) == G.ParameterViews.size() &&
                count(field(O, "projected_parameter_count")) ==
                    projectedParameters(G),
            "parameter inventory counts disagree");
  }
  for (const auto &Row : array(field(O, "blocks"), Options.MaxBlocks)) {
    Control.check();
    const auto &B = object(Row);
    if (Parameters)
      fields(B, {"begin", "end", "terminator", "successors", "gap_mask"});
    FlowBlock Block;
    Block.Begin = address(field(B, "begin"));
    Block.End = address(field(B, "end"));
    Block.Terminator = index(field(B, "terminator"));
    Block.Gaps = count(field(B, "gap_mask"), AllFlowGaps);
    for (const auto &I : array(field(B, "successors"), Options.MaxBlocks)) {
      Control.check();
      require(++Edges <= Options.MaxWork, "CFG exceeds work allowance");
      Block.Successors.push_back(count(I));
    }
    G.Blocks.push_back(std::move(Block));
  }
  for (const auto &Row :
       array(field(O, "nodes"),
             Options.MaxNodes - G.ParameterViews.size() - ProgressRows)) {
    Control.check();
    const auto &N = object(Row);
    if (Parameters)
      fields(N, {"opcode", "result_bytes", "memory_ordering",
                 "memory_address_space", "block", "address", "origin_sequence",
                 "phi", "commutative", "gap_mask", "call_argument_begin",
                 "call_argument_count", "inputs", "semantics", "origins"});
    else
      require(!N.get("parameter_views"),
              "legacy node contains parameter-view metadata");
    FlowNode Node;
    Node.Opcode = text(field(N, "opcode"), limits::kMaxFlowOpcodeBytes);
    Node.ResultBytes = count(field(N, "result_bytes"), UINT16_MAX);
    Node.MemoryOrdering = count(field(N, "memory_ordering"), UINT8_MAX);
    Node.MemoryAddressSpace =
        count(field(N, "memory_address_space"), UINT8_MAX);
    Node.Block = count(field(N, "block"));
    Node.Address = address(field(N, "address"));
    const auto Origin = field(N, "origin_sequence").getAsInteger();
    require(Origin && *Origin >= -1 && *Origin <= INT32_MAX,
            "invalid original operation sequence");
    Node.OriginSequence = *Origin;
    Node.Phi = boolean(field(N, "phi"));
    Node.Commutative = boolean(field(N, "commutative"));
    Node.Gaps = count(field(N, "gap_mask"), AllFlowGaps);
    Node.CallArgumentBegin = index(field(N, "call_argument_begin"));
    Node.CallArgumentCount = count(field(N, "call_argument_count"));
    if (Extended) {
      Node.Semantics =
          text(field(N, "semantics"), limits::kMaxFlowSemanticsBytes);
      Edges += Node.Semantics.size();
      require(Edges <= Options.MaxWork, "metadata exceeds work allowance");
      for (const auto &Origin : array(field(N, "origins"), Options.MaxWork)) {
        Control.check();
        require(++Edges <= Options.MaxWork, "origins exceed work allowance");
        const auto &O = object(Origin);
        if (Parameters)
          fields(O, {"address", "sequence"});
        Node.Origins.push_back(
            {address(field(O, "address")),
             int32_t(count(field(O, "sequence"), INT32_MAX))});
      }
    } else {
      require(!N.get("semantics") && !N.get("origins"),
              "MedIR graph contains optimized metadata");
    }
    for (const auto &Input : array(field(N, "inputs"), Options.MaxWork)) {
      Control.check();
      require(++Edges <= Options.MaxWork, "operands exceed work allowance");
      const auto &A = array(Input, Extended ? 8 : 7);
      require(A.size() == (Extended ? 8u : 7u), "invalid operand field count");
      FlowOperand I;
      I.Type =
          FlowOperand::Kind(count(A[0], unsigned(FlowOperand::Kind::Unknown)));
      I.Node = index(A[1]);
      I.Bytes = count(A[2], UINT16_MAX);
      I.Value = address(A[3]);
      I.Provenance = count(A[4], UINT8_MAX);
      I.AddressOwner = address(A[5]);
      I.Predecessor = index(A[6]);
      if (Extended) {
        I.Semantics = text(A[7], limits::kMaxFlowSemanticsBytes);
        Edges += I.Semantics.size();
        require(Edges <= Options.MaxWork,
                "operand metadata exceeds work allowance");
      }
      require((I.Type == FlowOperand::Kind::Node) == (I.Node != NoIndex),
              "operand kind disagrees with its definition");
      Node.Inputs.push_back(I);
    }
    G.Nodes.push_back(std::move(Node));
  }
  require(count(field(O, "node_count")) == G.Nodes.size() &&
              count(field(O, "block_count")) == G.Blocks.size(),
          "graph inventory counts disagree");
  if (auto Error = G.validate(Options, Control))
    throw std::invalid_argument(*Error);
  return G;
}
Object dataFlowNodeJSON(const DataFlowGraph &G, uint32_t N) {
  require(N < G.Nodes.size(), "missing graph node");
  auto Row = node(G.Nodes[N], isLLVMDataFlowProfile(G.Profile));
  Row["id"] = N;
  Row["gaps"] = gaps(G.Nodes[N].Gaps);
  if (hasParameterViews(G.Profile)) {
    Array Views;
    for (uint32_t I = 0; I < G.Nodes[N].Inputs.size(); ++I) {
      const auto &Input = G.Nodes[N].Inputs[I];
      if (Input.Type != FlowOperand::Kind::Parameter)
        continue;
      require(Input.Value < G.ParameterViews.size(), "missing parameter view");
      auto View = parameterView(G.ParameterViews[Input.Value]);
      View["input"] = I;
      View["parameter"] = Input.Value;
      Views.push_back(std::move(View));
    }
    Row["parameter_views"] = std::move(Views);
  }
  if (G.Progress) {
    const auto &Loops = G.Progress->Loops;
    const auto At = std::lower_bound(
        Loops.begin(), Loops.end(), G.Nodes[N].Block,
        [](const auto &L, uint32_t Block) { return L.Block < Block; });
    Row["progress_loop"] = At != Loops.end() && At->Block == G.Nodes[N].Block
                               ? Value(loopProgress(*At))
                               : Value(nullptr);
  }
  return Row;
}
Object dataFlowPageJSON(const DataFlowGraph &G, bool Blocks, uint64_t Offset,
                        unsigned Limit) {
  page(Limit);
  auto Out = metadata(G);
  const uint64_t Total = Blocks ? G.Blocks.size() : G.Nodes.size();
  Array Rows;
  for (uint64_t N = Offset; N < Total && N - Offset < Limit; ++N) {
    auto Row = Blocks ? block(G.Blocks[N]) : dataFlowNodeJSON(G, N);
    Row["id"] = N;
    Row["gaps"] = gaps(Blocks ? G.Blocks[N].Gaps : G.Nodes[N].Gaps);
    Rows.push_back(std::move(Row));
  }
  Out["kind"] = Blocks ? "blocks" : "nodes";
  Out["offset"] = Offset;
  Out["total"] = Total;
  Out["rows"] = std::move(Rows);
  Out["semantic_validation"] = "not_performed";
  return Out;
}
Object dataFlowSliceJSON(const DataFlowGraph &G, const FlowSlice &Slice,
                         uint64_t Offset, unsigned Limit) {
  page(Limit);
  auto Out = metadata(G);
  Array Rows;
  for (uint64_t I = Offset; I < Slice.Nodes.size() && I - Offset < Limit; ++I) {
    const auto N = Slice.Nodes[I];
    Rows.push_back(dataFlowNodeJSON(G, N));
  }
  Out["state"] = stateName(Slice.State);
  Out["work"] = Slice.Work;
  Out["gaps"] = gaps(Slice.Gaps);
  Out["control_dependencies"] = Slice.ControlDependencies;
  Out["offset"] = Offset;
  Out["total"] = Slice.Nodes.size();
  Out["rows"] = std::move(Rows);
  Out["semantic_validation"] = "not_performed";
  return Out;
}
} // namespace neverd::diff
