//===- NeverDCAPICallPaths.cpp - C API: call paths and call trees ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The graph that the listed functions' direct references (InstructionFlow.h)
/// make between functions and imports, read without analysis: the call paths
/// between waypoints on it, as an interactive disassembler's path finder
/// finds them, and the functions a function reaches or is reached from.
///
//===----------------------------------------------------------------------===//

#include "InstructionFlow.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

namespace neverd::sdk {

/// How one node refers to another, as bits of CallReferenceGraph::Edge.
enum CallEdgeKind : uint8_t {
  /// A direct call.
  CallEdge = 1,
  /// A jump to another function's entry: a tail call.
  JumpEdge = 2,
  /// A call or jump through a slot that holds the target: an import's, or
  /// one the loader relocated to code.
  IndirectEdge = 4,
  /// The target's address taken, read or written.
  DataEdge = 8,
};
inline constexpr uint8_t ControlEdges = CallEdge | JumpEdge | IndirectEdge;

struct CallReferenceGraph {
  /// The function list the graph was read from.
  uint64_t Revision = 0;
  const FuncInfo *List = nullptr;
  size_t Count = 0;
  size_t ImportCount = 0;

  struct Node {
    /// A function's entry, or an import's slot (its veneer where it has
    /// none).
    va_t Address = 0;
    std::string Name;
    std::string Module;
    bool Import = false;
    /// An independently admitted signature identity names library code.
    bool Library = false;
  };
  struct Edge {
    /// The callee in Out, the caller in In.
    uint32_t Other = 0;
    uint8_t Kinds = 0;
    /// The first instruction that makes the reference.
    va_t Site = InvalidVA;
  };
  std::vector<Node> Nodes;
  /// Each node's edges, ordered by the other node's address.
  std::vector<std::vector<Edge>> Out, In;
  /// Function entries, import slots and import veneers.
  std::unordered_map<va_t, uint32_t> NodeAt;
};

} // namespace neverd::sdk

namespace {

constexpr uint32_t Unreached = std::numeric_limits<uint32_t>::max();

std::optional<uint32_t> nodeAt(const CallReferenceGraph &G,
                               const BinaryImage &Img, va_t Address) {
  if (const auto It = G.NodeAt.find(Address); It != G.NodeAt.end())
    return It->second;
  // A Thumb address names its function with the mode bit set.
  if (Img.Arch == Arch::ARM && (Address & 1))
    if (const auto It = G.NodeAt.find(Address & ~va_t{1}); It != G.NodeAt.end())
      return It->second;
  return std::nullopt;
}

std::shared_ptr<const CallReferenceGraph> buildGraph(Session &S) {
  auto G = std::make_shared<CallReferenceGraph>();
  G->Revision = S.FunctionListRevision;
  G->List = S.Functions.data();
  G->Count = S.Functions.size();
  G->ImportCount = S.Img.Imports.size();
  const BinaryImage &Img = S.Img;

  std::unordered_map<const Import *, uint32_t> ImportNodes;
  const auto ImportNode = [&](const Import &Imported, va_t Veneer) {
    const auto [It, Added] =
        ImportNodes.try_emplace(&Imported, uint32_t(G->Nodes.size()));
    if (Added) {
      CallReferenceGraph::Node Node;
      Node.Address = Imported.IATAddr ? Imported.IATAddr : Veneer;
      Node.Name = Imported.Name.empty()
                      ? "ordinal_" + std::to_string(Imported.Ordinal)
                      : Imported.Name;
      Node.Module = Imported.Module;
      Node.Import = true;
      G->Nodes.push_back(std::move(Node));
      if (Imported.IATAddr)
        G->NodeAt.try_emplace(Imported.IATAddr, It->second);
    }
    return It->second;
  };

  std::vector<const FuncInfo *> Ordered;
  Ordered.reserve(S.Functions.size());
  for (const FuncInfo &F : S.Functions)
    Ordered.push_back(&F);
  std::sort(
      Ordered.begin(), Ordered.end(),
      [](const FuncInfo *L, const FuncInfo *R) { return L->Entry < R->Entry; });
  Ordered.erase(std::unique(Ordered.begin(), Ordered.end(),
                            [](const FuncInfo *L, const FuncInfo *R) {
                              return L->Entry == R->Entry;
                            }),
                Ordered.end());

  // A function that is an import's veneer is the import; the others decode.
  std::vector<const FuncInfo *> Bodies;
  std::vector<uint32_t> BodyNodes;
  for (const FuncInfo *F : Ordered) {
    if (const Import *Imported = Img.findImportStubAt(F->Entry)) {
      G->NodeAt.try_emplace(F->Entry, ImportNode(*Imported, F->Entry));
      continue;
    }
    CallReferenceGraph::Node Node;
    Node.Address = F->Entry;
    Node.Name = F->Name;
    Node.Library = S.SigDB.nameDecision(F->Entry) != nullptr;
    const uint32_t Index = G->Nodes.size();
    G->Nodes.push_back(std::move(Node));
    G->NodeAt.try_emplace(F->Entry, Index);
    Bodies.push_back(F);
    BodyNodes.push_back(Index);
  }
  for (const Import &Imported : Img.Imports)
    if (Imported.IATAddr)
      (void)ImportNode(Imported, 0);

  // A call into a function's body, past its entry, reaches that function.
  struct Extent {
    va_t Begin, End;
    uint32_t Node;
  };
  std::vector<Extent> Extents;
  for (size_t I = 0; I < Bodies.size(); ++I)
    if (Bodies[I]->Size)
      Extents.push_back(
          {Bodies[I]->Entry, Bodies[I]->Entry + Bodies[I]->Size, BodyNodes[I]});
  const auto Containing = [&](va_t Address) -> std::optional<uint32_t> {
    auto It =
        std::upper_bound(Extents.begin(), Extents.end(), Address,
                         [](va_t A, const Extent &E) { return A < E.Begin; });
    if (It == Extents.begin())
      return std::nullopt;
    --It;
    return Address < It->End ? std::optional(It->Node) : std::nullopt;
  };

  struct Reference {
    uint32_t To;
    uint8_t Kind;
    va_t Site;
  };
  std::vector<std::vector<Reference>> Slots(Bodies.size());
  // The references of one instruction of body \p Index.
  const auto Collect = [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
                           InstructionFlowScratch &Scratch) {
    auto &Out = Slots[Index];
    const auto Add = [&](std::optional<uint32_t> To, uint8_t Kind) {
      if (To)
        Out.push_back({*To, Kind, DI.Addr});
    };
    const InstructionFlow Flow = Scratch.summarize(Img, Dec, DI);
    const bool Indirect = Flow.Kind == "icall" || Flow.Kind == "ijump";
    if (Flow.Target != InvalidVA && !Flow.Kind.empty() &&
        Flow.Kind != UnliftedFlow) {
      if (Flow.Kind == "call") {
        const auto To = nodeAt(*G, Img, Flow.Target);
        Add(To ? To : Containing(Flow.Target), CallEdge);
      } else {
        // A jump within a function stays in it; one to another function's
        // entry leaves it.
        Add(nodeAt(*G, Img, Flow.Target), Indirect ? IndirectEdge : JumpEdge);
      }
    }
    for (const InstructionRef &Ref : Flow.Refs) {
      if (Indirect && Ref.Kind == "read") {
        // The slot an indirect transfer reads: an import's, or one the
        // loader relocated to code, reaches what it holds.
        if (const auto Slot = nodeAt(*G, Img, Ref.To)) {
          Add(Slot, IndirectEdge);
          continue;
        }
        if (Img.CodePtrRelocSlots.count(Ref.To))
          if (const auto Target = relocatedPointer(Img, Ref.To)) {
            Add(nodeAt(*G, Img, *Target), IndirectEdge);
            continue;
          }
      }
      Add(nodeAt(*G, Img, Ref.To), DataEdge);
    }
  };

  // The bodies decode as direct references read them: across a recorded
  // extent, else along the function's own jumps short of other entries.
  std::vector<va_t> Entries;
  Entries.reserve(Ordered.size());
  for (const FuncInfo *F : Ordered)
    Entries.push_back(F->Entry);
  std::optional<libc::NoReturnTargetIndex> NoReturnTargets;
  if (std::any_of(Bodies.begin(), Bodies.end(),
                  [](const FuncInfo *F) { return !F->Size; }))
    NoReturnTargets.emplace(Img);
  for (size_t First = 0; First < Bodies.size();
       First += FunctionPage::MaxFunctionsPerPage) {
    FunctionPage Page;
    const size_t Last = std::min<size_t>(
        Bodies.size(), First + FunctionPage::MaxFunctionsPerPage);
    Page.Functions.assign(Bodies.begin() + First, Bodies.begin() + Last);
    Page.Entries = Entries;
    if (!decodeFunctions(
            Img, Page,
            [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
                InstructionFlowScratch &Scratch) {
              Collect(First + Index, Dec, DI, Scratch);
            },
            {}, nullptr, NoReturnTargets ? &*NoReturnTargets : nullptr)) {
      S.setError("failed to initialize a decoder for the image");
      return nullptr;
    }
  }

  // One edge per ordered pair, with every kind of reference it stands for
  // and the first instruction making one.
  std::unordered_map<uint64_t, size_t> EdgeIndex;
  struct Flat {
    uint32_t From, To;
    uint8_t Kinds;
    va_t Site;
  };
  std::vector<Flat> Edges;
  for (size_t I = 0; I < Bodies.size(); ++I)
    for (const Reference &Ref : Slots[I]) {
      const uint32_t From = BodyNodes[I];
      if (Ref.To == From)
        continue;
      const uint64_t Key = (uint64_t(From) << 32) | Ref.To;
      const auto [It, Added] = EdgeIndex.try_emplace(Key, Edges.size());
      if (Added) {
        Edges.push_back({From, Ref.To, Ref.Kind, Ref.Site});
        continue;
      }
      Flat &Edge = Edges[It->second];
      Edge.Kinds |= Ref.Kind;
      Edge.Site = std::min(Edge.Site, Ref.Site);
    }
  G->Out.assign(G->Nodes.size(), {});
  G->In.assign(G->Nodes.size(), {});
  for (const Flat &Edge : Edges) {
    G->Out[Edge.From].push_back({Edge.To, Edge.Kinds, Edge.Site});
    G->In[Edge.To].push_back({Edge.From, Edge.Kinds, Edge.Site});
  }
  const auto ByAddress = [&](const CallReferenceGraph::Edge &L,
                             const CallReferenceGraph::Edge &R) {
    return G->Nodes[L.Other].Address < G->Nodes[R.Other].Address;
  };
  for (auto &List : G->Out)
    std::sort(List.begin(), List.end(), ByAddress);
  for (auto &List : G->In)
    std::sort(List.begin(), List.end(), ByAddress);
  return G;
}

/// The session's graph, read again when the function list changed.
const CallReferenceGraph *callGraph(Session &S) {
  if (!S.synchronizeFunctions())
    return nullptr;
  const auto &Cached = S.CallGraph;
  if (!Cached || Cached->Revision != S.FunctionListRevision ||
      Cached->List != S.Functions.data() ||
      Cached->Count != S.Functions.size() ||
      Cached->ImportCount != S.Img.Imports.size())
    S.CallGraph = buildGraph(S);
  return S.CallGraph.get();
}

std::string edgeKindName(uint8_t Kinds) {
  if (Kinds & CallEdge)
    return "call";
  if (Kinds & JumpEdge)
    return "jump";
  if (Kinds & IndirectEdge)
    return "indirect";
  return "data";
}

/// A waypoint, root or exclusion: a hexadecimal address of a function, an
/// import's slot or veneer, else a function's name, else an import's.
std::optional<uint32_t> resolveNode(Session &S, const CallReferenceGraph &G,
                                    llvm::StringRef Text, std::string &Error) {
  if (Text.starts_with_insensitive("0x")) {
    uint64_t Address = 0;
    if (Text.drop_front(2).getAsInteger(16, Address)) {
      Error = "invalid address: " + Text.str();
      return std::nullopt;
    }
    if (const auto Node = nodeAt(G, S.Img, Address))
      return Node;
    Error = "no function or import at " + Text.str();
    return std::nullopt;
  }
  if (const auto Index = S.functionIndexNamed(Text))
    if (const auto Node = nodeAt(G, S.Img, S.Functions[*Index].Entry))
      return Node;
  for (uint32_t I = 0; I < G.Nodes.size(); ++I)
    if (G.Nodes[I].Import && G.Nodes[I].Name == Text)
      return I;
  Error = "no function or import named " + Text.str();
  return std::nullopt;
}

/// Reads an integer member of \p Options from \p Min to \p Max.
bool readBound(const llvm::json::Object &Options, llvm::StringRef Key,
               unsigned Min, unsigned Max, unsigned &Value,
               std::string &Error) {
  const llvm::json::Value *Member = Options.get(Key);
  if (!Member)
    return true;
  const auto Integer = Member->getAsInteger();
  if (!Integer || *Integer < Min || *Integer > Max) {
    Error = Key.str() + " must be an integer from " + std::to_string(Min) +
            " to " + std::to_string(Max);
    return false;
  }
  Value = static_cast<unsigned>(*Integer);
  return true;
}

bool readFlag(const llvm::json::Object &Options, llvm::StringRef Key,
              bool &Value, std::string &Error) {
  const llvm::json::Value *Member = Options.get(Key);
  if (!Member)
    return true;
  const auto Flag = Member->getAsBoolean();
  if (!Flag) {
    Error = Key.str() + " must be a boolean";
    return false;
  }
  Value = *Flag;
  return true;
}

bool readNodes(Session &S, const CallReferenceGraph &G,
               const llvm::json::Object &Options, llvm::StringRef Key,
               std::vector<uint32_t> &Nodes, std::string &Error) {
  const llvm::json::Value *Member = Options.get(Key);
  if (!Member)
    return true;
  const llvm::json::Array *List = Member->getAsArray();
  if (!List) {
    Error = Key.str() + " must be an array of addresses or names";
    return false;
  }
  for (const llvm::json::Value &Item : *List) {
    const auto Text = Item.getAsString();
    if (!Text) {
      Error = Key.str() + " must be an array of addresses or names";
      return false;
    }
    const auto Node = resolveNode(S, G, *Text, Error);
    if (!Node)
      return false;
    Nodes.push_back(*Node);
  }
  return true;
}

std::optional<llvm::json::Object> parseOptions(const char *Json,
                                               std::string &Error) {
  if (!Json || !*Json)
    return llvm::json::Object();
  auto Parsed = llvm::json::parse(Json);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    Error = "options must be a JSON object";
    return std::nullopt;
  }
  if (auto *Object = Parsed->getAsObject())
    return std::move(*Object);
  Error = "options must be a JSON object";
  return std::nullopt;
}

bool onlyKeys(const llvm::json::Object &Options,
              std::initializer_list<llvm::StringRef> Keys, std::string &Error) {
  for (const auto &Member : Options)
    if (std::find(Keys.begin(), Keys.end(), llvm::StringRef(Member.first)) ==
        Keys.end()) {
      Error = "unknown option: " + llvm::StringRef(Member.first).str();
      return false;
    }
  return true;
}

/// Which edges a search follows, and the nodes it leaves out.
struct Traversal {
  const CallReferenceGraph &G;
  bool DataRefs = false;
  std::vector<uint8_t> Excluded;

  bool follows(const CallReferenceGraph::Edge &E) const {
    return (E.Kinds & ControlEdges) || (DataRefs && (E.Kinds & DataEdge));
  }

  /// Edges from \p Start, forward or backward, of every node within
  /// \p MaxDepth of it.
  std::vector<uint32_t> distances(uint32_t Start, bool Forward,
                                  unsigned MaxDepth) const {
    std::vector<uint32_t> Distance(G.Nodes.size(), Unreached);
    std::vector<uint32_t> Queue{Start};
    Distance[Start] = 0;
    for (size_t Head = 0; Head < Queue.size(); ++Head) {
      const uint32_t Node = Queue[Head];
      if (Distance[Node] == MaxDepth)
        continue;
      for (const auto &E : (Forward ? G.Out : G.In)[Node]) {
        if (!follows(E) || Excluded[E.Other] || Distance[E.Other] != Unreached)
          continue;
        Distance[E.Other] = Distance[Node] + 1;
        Queue.push_back(E.Other);
      }
    }
    return Distance;
  }
};

llvm::json::Array edgeRow(const CallReferenceGraph &G, uint32_t From,
                          const CallReferenceGraph::Edge &E) {
  return llvm::json::Array{vaHex(G.Nodes[From].Address),
                           vaHex(G.Nodes[E.Other].Address),
                           edgeKindName(E.Kinds), vaHex(E.Site)};
}

llvm::json::Object nodeObject(const CallReferenceGraph &G, uint32_t Node) {
  const auto &N = G.Nodes[Node];
  llvm::json::Object Object{{"address", vaHex(N.Address)},
                            {"name", jsonSafeText(N.Name)},
                            {"kind", N.Import ? "import" : "function"}};
  if (N.Import && !N.Module.empty())
    Object["module"] = jsonSafeText(N.Module);
  if (N.Library)
    Object["library"] = true;
  return Object;
}

struct PathOptions {
  unsigned MaxDepth = kDefaultCallPathDepth;
  unsigned MaxPaths = kDefaultCallPaths;
  unsigned MaxListed = kDefaultCallGraphListed;
  bool ShortestOnly = false;
};

/// The paths from \p From to \p To of one segment, or none when neither
/// reaches the other.
std::optional<llvm::json::Object> segment(const Traversal &T,
                                          const PathOptions &Options,
                                          uint32_t From, uint32_t To,
                                          std::set<uint32_t> &Mentioned) {
  const CallReferenceGraph &G = T.G;
  std::vector<uint32_t> FromStart = T.distances(From, true, Options.MaxDepth);
  bool Reversed = false;
  if (FromStart[To] == Unreached) {
    // Waypoints that connect only the other way round are a path in reverse.
    std::vector<uint32_t> Back = T.distances(To, true, Options.MaxDepth);
    if (Back[From] == Unreached)
      return std::nullopt;
    std::swap(From, To);
    FromStart = std::move(Back);
    Reversed = true;
  }
  const std::vector<uint32_t> ToEnd = T.distances(To, false, Options.MaxDepth);
  const uint32_t Shortest = FromStart[To];
  const uint32_t Bound = Options.ShortestOnly ? Shortest : Options.MaxDepth;
  const auto OnPath = [&](uint32_t Node) {
    return FromStart[Node] != Unreached && ToEnd[Node] != Unreached &&
           FromStart[Node] + ToEnd[Node] <= Bound;
  };

  std::vector<uint32_t> Members;
  for (uint32_t Node = 0; Node < G.Nodes.size(); ++Node)
    if (OnPath(Node))
      Members.push_back(Node);
  std::sort(Members.begin(), Members.end(), [&](uint32_t L, uint32_t R) {
    return std::tie(FromStart[L], G.Nodes[L].Address) <
           std::tie(FromStart[R], G.Nodes[R].Address);
  });
  llvm::json::Array Nodes, Edges;
  size_t EdgeCount = 0;
  bool Listed = true;
  for (uint32_t Node : Members) {
    if (Nodes.size() < Options.MaxListed) {
      Nodes.push_back(vaHex(G.Nodes[Node].Address));
      Mentioned.insert(Node);
    } else {
      Listed = false;
    }
    for (const auto &E : G.Out[Node]) {
      if (!T.follows(E) || !OnPath(E.Other) ||
          FromStart[Node] + 1 + ToEnd[E.Other] > Bound)
        continue;
      ++EdgeCount;
      if (Edges.size() < Options.MaxListed)
        Edges.push_back(edgeRow(G, Node, E));
      else
        Listed = false;
    }
  }

  // The simple paths, shortest first: each length's in depth-first order.
  llvm::json::Array Paths;
  std::vector<uint32_t> Path{From};
  std::vector<uint8_t> Visited(G.Nodes.size());
  Visited[From] = 1;
  uint64_t Steps = 0;
  bool Exhausted = false;
  uint32_t Length = 0;
  std::function<void(uint32_t)> Walk = [&](uint32_t Node) {
    if (Node == To) {
      if (Path.size() - 1 == Length) {
        llvm::json::Array Row;
        for (uint32_t Step : Path) {
          Row.push_back(vaHex(G.Nodes[Step].Address));
          Mentioned.insert(Step);
        }
        Paths.push_back(std::move(Row));
      }
      return;
    }
    const uint32_t Depth = Path.size() - 1;
    for (const auto &E : G.Out[Node]) {
      if (Paths.size() >= Options.MaxPaths || Exhausted)
        return;
      if (!T.follows(E) || Visited[E.Other] || ToEnd[E.Other] == Unreached ||
          Depth + 1 + ToEnd[E.Other] > Length)
        continue;
      if (++Steps > kCallPathStepBudget) {
        Exhausted = true;
        return;
      }
      Visited[E.Other] = 1;
      Path.push_back(E.Other);
      Walk(E.Other);
      Path.pop_back();
      Visited[E.Other] = 0;
    }
  };
  for (Length = Shortest;
       Length <= Bound && Paths.size() < Options.MaxPaths && !Exhausted;
       ++Length)
    Walk(From);
  Mentioned.insert(From);
  Mentioned.insert(To);

  llvm::json::Object Result{
      {"from", vaHex(G.Nodes[From].Address)},
      {"to", vaHex(G.Nodes[To].Address)},
      {"shortest", int64_t(Shortest)},
      {"node_count", int64_t(Members.size())},
      {"edge_count", int64_t(EdgeCount)},
      {"nodes", std::move(Nodes)},
      {"edges", std::move(Edges)},
      {"paths", std::move(Paths)},
      // Every node, edge and, unless none were asked
      // for, path within the bound is listed.
      {"complete",
       Listed && (Options.MaxPaths == 0 || (!Exhausted && Length > Bound))}};
  if (Reversed)
    Result["reversed"] = true;
  return Result;
}

/// The checks every query of the graph shares; null with the session's error
/// set when the image has no native function graph.
const CallReferenceGraph *nativeGraph(Session *S) {
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (S->Img.Arch == Arch::EVM || S->Img.Arch == Arch::SBF) {
    S->setError("call paths are only published for native images");
    return nullptr;
  }
  return callGraph(*S);
}

} // namespace

const char *neverd_call_paths_json(neverd_session_t Sess,
                                   const char *OptionsJson) {
  auto *S = toSession(Sess);
  std::string Error;
  auto Options = parseOptions(OptionsJson, Error);
  const CallReferenceGraph *G = Options ? nativeGraph(S) : nullptr;
  if (!G) {
    if (S && !Error.empty())
      S->setError(Error);
    return nullptr;
  }
  PathOptions Limits;
  Traversal T{*G};
  T.Excluded.assign(G->Nodes.size(), 0);
  std::vector<uint32_t> Waypoints, Excluded;
  if (!onlyKeys(*Options,
                {"waypoints", "exclude", "max_depth", "shortest_only",
                 "data_refs", "max_paths", "max_listed"},
                Error) ||
      !readNodes(*S, *G, *Options, "waypoints", Waypoints, Error) ||
      !readNodes(*S, *G, *Options, "exclude", Excluded, Error) ||
      !readBound(*Options, "max_depth", 1, kMaxCallPathDepth, Limits.MaxDepth,
                 Error) ||
      !readBound(*Options, "max_paths", 0, kMaxCallPaths, Limits.MaxPaths,
                 Error) ||
      !readBound(*Options, "max_listed", 1, kMaxCallGraphListed,
                 Limits.MaxListed, Error) ||
      !readFlag(*Options, "shortest_only", Limits.ShortestOnly, Error) ||
      !readFlag(*Options, "data_refs", T.DataRefs, Error)) {
    S->setError(Error);
    return nullptr;
  }
  if (Waypoints.size() < 2 || Waypoints.size() > kMaxCallPathWaypoints) {
    S->setError("waypoints must name 2 to " +
                std::to_string(kMaxCallPathWaypoints) +
                " functions or imports");
    return nullptr;
  }
  for (uint32_t Node : Excluded)
    T.Excluded[Node] = 1;
  // A waypoint is visited even when it is also excluded.
  for (uint32_t Node : Waypoints)
    T.Excluded[Node] = 0;

  std::set<uint32_t> Mentioned;
  llvm::json::Array Segments;
  int64_t Unconnected = 0;
  for (size_t I = 0; I + 1 < Waypoints.size(); ++I) {
    if (auto Found =
            segment(T, Limits, Waypoints[I], Waypoints[I + 1], Mentioned)) {
      Segments.push_back(std::move(*Found));
      continue;
    }
    ++Unconnected;
    Mentioned.insert(Waypoints[I]);
    Mentioned.insert(Waypoints[I + 1]);
    Segments.push_back(
        llvm::json::Object{{"from", vaHex(G->Nodes[Waypoints[I]].Address)},
                           {"to", vaHex(G->Nodes[Waypoints[I + 1]].Address)},
                           {"shortest", nullptr},
                           {"node_count", 0},
                           {"edge_count", 0},
                           {"nodes", llvm::json::Array()},
                           {"edges", llvm::json::Array()},
                           {"paths", llvm::json::Array()},
                           {"complete", true}});
  }
  llvm::json::Array Nodes;
  for (uint32_t Node : Mentioned)
    Nodes.push_back(nodeObject(*G, Node));
  llvm::json::Object Result{{"segments", std::move(Segments)},
                            {"unconnected", Unconnected},
                            {"nodes", std::move(Nodes)},
                            {"max_depth", int64_t(Limits.MaxDepth)},
                            {"data_refs", T.DataRefs}};
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_call_tree_json(neverd_session_t Sess,
                                  const char *OptionsJson) {
  auto *S = toSession(Sess);
  std::string Error;
  auto Options = parseOptions(OptionsJson, Error);
  const CallReferenceGraph *G = Options ? nativeGraph(S) : nullptr;
  if (!G) {
    if (S && !Error.empty())
      S->setError(Error);
    return nullptr;
  }
  unsigned MaxDepth = kDefaultCallTreeDepth,
           MaxListed = kDefaultCallGraphListed;
  bool ExpandLibrary = false;
  Traversal T{*G};
  T.Excluded.assign(G->Nodes.size(), 0);
  std::vector<uint32_t> Roots, Excluded;
  std::string Direction = "callees";
  if (const auto *Member = Options->get("direction")) {
    const auto Text = Member->getAsString();
    if (!Text || (*Text != "callees" && *Text != "callers")) {
      S->setError("direction must be callees or callers");
      return nullptr;
    }
    Direction = Text->str();
  }
  if (const auto *Member = Options->get("root")) {
    if (const auto Text = Member->getAsString()) {
      if (const auto Node = resolveNode(*S, *G, *Text, Error))
        Roots.push_back(*Node);
    } else {
      Error = "root must be an address or a name";
    }
  } else {
    Error = "root is required";
  }
  if (!Error.empty() ||
      !onlyKeys(*Options,
                {"root", "direction", "max_depth", "max_listed", "data_refs",
                 "exclude", "expand_library"},
                Error) ||
      !readNodes(*S, *G, *Options, "exclude", Excluded, Error) ||
      !readBound(*Options, "max_depth", 1, kMaxCallPathDepth, MaxDepth,
                 Error) ||
      !readBound(*Options, "max_listed", 1, kMaxCallGraphListed, MaxListed,
                 Error) ||
      !readFlag(*Options, "data_refs", T.DataRefs, Error) ||
      !readFlag(*Options, "expand_library", ExpandLibrary, Error)) {
    S->setError(Error);
    return nullptr;
  }
  for (uint32_t Node : Excluded)
    T.Excluded[Node] = 1;
  const uint32_t Root = Roots.front();
  T.Excluded[Root] = 0;
  const bool Callees = Direction == "callees";
  const auto &Adjacent = Callees ? G->Out : G->In;
  // Library code and imports are listed, but what they reach is not theirs.
  const auto Expands = [&](uint32_t Node) {
    return Node == Root || (!G->Nodes[Node].Import &&
                            (ExpandLibrary || !G->Nodes[Node].Library));
  };

  std::vector<uint32_t> Depth(G->Nodes.size(), Unreached);
  std::vector<uint32_t> Order{Root};
  Depth[Root] = 0;
  bool Listed = true, DepthLimited = false;
  for (size_t Head = 0; Head < Order.size(); ++Head) {
    const uint32_t Node = Order[Head];
    if (!Expands(Node))
      continue;
    for (const auto &E : Adjacent[Node]) {
      if (!T.follows(E) || T.Excluded[E.Other] || Depth[E.Other] != Unreached)
        continue;
      if (Depth[Node] == MaxDepth) {
        DepthLimited = true;
        break;
      }
      if (Order.size() == MaxListed) {
        Listed = false;
        break;
      }
      Depth[E.Other] = Depth[Node] + 1;
      Order.push_back(E.Other);
    }
  }

  llvm::json::Array Nodes, Edges;
  for (uint32_t Node : Order) {
    llvm::json::Object Object = nodeObject(*G, Node);
    Object["depth"] = int64_t(Depth[Node]);
    Nodes.push_back(std::move(Object));
    if (!Expands(Node))
      continue;
    for (const auto &E : Adjacent[Node])
      if (T.follows(E) && Depth[E.Other] != Unreached)
        Edges.push_back(Callees
                            ? edgeRow(*G, Node, E)
                            : edgeRow(*G, E.Other, {Node, E.Kinds, E.Site}));
  }
  llvm::json::Object Result{{"root", vaHex(G->Nodes[Root].Address)},
                            {"direction", Direction},
                            {"max_depth", int64_t(MaxDepth)},
                            {"nodes", std::move(Nodes)},
                            {"edges", std::move(Edges)},
                            {"depth_limited", DepthLimited},
                            {"complete", Listed}};
  if (Callees) {
    // Callees before their callers: the order to decompile them in so that
    // each function's callees come first; imports and library code do not
    // decompile.
    llvm::json::Array BottomUp;
    std::vector<uint8_t> Seen(G->Nodes.size());
    std::vector<std::pair<uint32_t, size_t>> Stack{{Root, 0}};
    Seen[Root] = 1;
    while (!Stack.empty()) {
      auto &[Node, Next] = Stack.back();
      const auto &List = G->Out[Node];
      bool Descended = false;
      while (Next < List.size()) {
        const auto &E = List[Next++];
        const uint32_t Callee = E.Other;
        if (!T.follows(E) || Depth[Callee] == Unreached || Seen[Callee] ||
            !Expands(Callee))
          continue;
        Seen[Callee] = 1;
        Stack.push_back({Callee, 0});
        Descended = true;
        break;
      }
      if (Descended)
        continue;
      BottomUp.push_back(vaHex(G->Nodes[Node].Address));
      Stack.pop_back();
    }
    Result["bottom_up"] = std::move(BottomUp);
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
