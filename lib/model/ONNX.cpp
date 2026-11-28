//===- ONNX.cpp - Bounded inventory of exported ONNX protobuf graphs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded inventory of exported ONNX protobuf graphs.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "neverd/support/FilePath.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>

namespace neverd::model {
namespace {
struct Field {
  unsigned Id, Wire;
  uint64_t Value, At, End;
};
struct Proto {
  File &F;
  Budget &B;
  uint64_t Pos, End;
  SHA256Stream &Observed;
  std::string metadata(uint64_t At, uint64_t Length) const {
    auto Bytes = F.header(At, Length, B);
    std::array<uint8_t, 16> Range;
    llvm::support::endian::write64le(Range.data(), At);
    llvm::support::endian::write64le(Range.data() + 8, Length);
    Observed.update(Range);
    Observed.update(llvm::StringRef(Bytes));
    return Bytes;
  }
  uint64_t varint() {
    uint64_t V = 0;
    for (unsigned I = 0; I < 10; ++I) {
      if (Pos == End)
        throw Error("truncated protobuf varint");
      auto C = static_cast<unsigned char>(metadata(Pos++, 1)[0]);
      if (I == 9 && C > 1)
        throw Error("protobuf varint overflow");
      V |= uint64_t(C & 127) << (7 * I);
      if (!(C & 128))
        return V;
    }
    throw Error("protobuf varint too long");
  }
  Field next() { return field(varint()); }
  Field field(uint64_t Tag) {
    B.tick();
    if (!(Tag >> 3) || Tag >> 3 > 0x1fffffff)
      throw Error("invalid protobuf field number");
    Field X{unsigned(Tag >> 3), unsigned(Tag & 7), 0, Pos, Pos};
    if (!X.Wire) {
      X.Value = varint();
      X.End = Pos;
      return X;
    }
    uint64_t Length;
    switch (X.Wire) {
    case 1:
      Length = 8;
      break;
    case 2:
      Length = varint();
      break;
    case 5:
      Length = 4;
      break;
    default:
      throw Error("unsupported protobuf wire type");
    }
    X.At = Pos;
    if (Length > End - Pos)
      throw Error("protobuf field exceeds message");
    Pos += Length;
    X.End = Pos;
    return X;
  }
  Proto sub(Field X) const {
    if (X.Wire != 2)
      throw Error("wrong wire type for embedded message");
    return {F, B, X.At, X.End, Observed};
  }
  std::string label(Field X) const {
    if (X.Wire != 2 || X.End - X.At > limits::kNameBytes)
      throw Error("invalid ONNX label");
    auto S = metadata(X.At, X.End - X.At);
    if (!S.empty())
      validName(S);
    return S;
  }
};
uint64_t integer(Field X) {
  if (X.Wire)
    throw Error("invalid integer wire type");
  return X.Value;
}
Json opset(Proto R) {
  Json O = {{"domain", ""}};
  while (R.Pos < R.End) {
    auto X = R.next();
    if (X.Id == 1)
      O["domain"] = R.label(X);
    if (X.Id == 2)
      O["version"] = integer(X);
  }
  return O;
}
Json port(Proto R) {
  Json V = Json::object();
  while (R.Pos < R.End) {
    auto X = R.next();
    if (X.Id == 1)
      V["name"] = R.label(X);
    if (X.Id != 2)
      continue;
    auto T = R.sub(X);
    while (T.Pos < T.End) {
      auto K = T.next();
      if (K.Id != 1) {
        V["type_field"] = K.Id;
        continue;
      }
      auto D = T.sub(K);
      while (D.Pos < D.End) {
        auto A = D.next();
        if (A.Id == 1)
          V["element_type"] = integer(A);
        if (A.Id != 2)
          continue;
        auto S = D.sub(A);
        V["shape"] = Json::array();
        while (S.Pos < S.End) {
          auto Dim = S.next();
          if (Dim.Id != 1)
            continue;
          S.B.entry();
          auto M = S.sub(Dim);
          Json Value = nullptr;
          while (M.Pos < M.End) {
            auto I = M.next();
            if (I.Id == 1)
              Value = integer(I);
            if (I.Id == 2)
              Value = M.label(I);
          }
          V["shape"].push_back(Value);
        }
      }
    }
  }
  return V;
}
std::optional<uint64_t> storageBytes(unsigned Type, uint64_t Elements) {
  static constexpr unsigned Bits[] = {0, 32, 8,  8,  16, 16, 32,  64, 0,
                                      8, 16, 64, 32, 64, 64, 128, 16, 8,
                                      8, 8,  8,  4,  4,  4,  8,   2,  2};
  if (Type >= std::size(Bits) || !Bits[Type])
    return std::nullopt;
  return llvm::divideCeil(product(Elements, Bits[Type]), uint64_t(8));
}

std::optional<uint64_t> externalNumber(const Json &Refs, const char *Key) {
  auto It = Refs.find(Key);
  if (It == Refs.end())
    return std::nullopt;
  llvm::StringRef Text(It->get_ref<const std::string &>());
  uint64_t Value;
  if (Text.empty() || !llvm::all_of(Text, llvm::isDigit) ||
      Text.getAsInteger(10, Value))
    throw Error("invalid external initializer offset or length");
  return Value;
}

class GraphReader {
  Package &P;
  const std::string &Source;
  std::string tensor(Proto R, const std::string &Scope) {
    Tensor T;
    T.FileId = Source;
    T.HasOffset = false;
    std::string Name;
    unsigned Type = 0;
    std::optional<Field> Raw;
    bool External = false, Typed = false;
    Json References = Json::object();
    const auto Begin = R.Pos;
    while (R.Pos < R.End) {
      auto Tag = R.varint();
      auto Number = Tag >> 3;
      // Finding the next unpacked varint would require reading its weight
      // bytes. The initializer submessage already gives us its exact end.
      if ((Tag & 7) == 0 && (Number == 5 || Number == 7 || Number == 11)) {
        P.gap(Source,
              "unpacked initializer values and remaining fields not inspected",
              {{"tensor", Name},
               {"message_offset", Begin},
               {"unread_offset", R.Pos},
               {"unread_bytes", R.End - R.Pos}});
        return Name;
      }
      auto X = R.field(Tag);
      switch (X.Id) {
      case 1: {
        auto add = [&](uint64_t D) {
          if (D > INT64_MAX || T.Shape.size() >= limits::kDepth)
            throw Error("invalid initializer shape");
          T.Shape.push_back(D);
        };
        if (!X.Wire)
          add(X.Value);
        else {
          auto D = R.sub(X);
          while (D.Pos < D.End)
            add(D.varint());
        }
        break;
      }
      case 2:
        if (integer(X) > INT32_MAX)
          throw Error("invalid initializer type ID");
        Type = X.Value;
        break;
      case 8:
        Name = R.label(X);
        break;
      case 9:
        if (X.Wire != 2 || Raw)
          throw Error("invalid raw initializer");
        Raw = X;
        break;
      case 13: {
        auto E = R.sub(X);
        std::string Key, Value;
        while (E.Pos < E.End) {
          auto K = E.next();
          if (K.Id == 1)
            Key = E.label(K);
          if (K.Id == 2)
            Value = E.label(K);
        }
        if (Key.empty() || References.contains(Key))
          throw Error("invalid external initializer reference");
        References[Key] = Value;
        break;
      }
      case 14:
        External = integer(X) != 0;
        break;
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 10:
      case 11:
        Typed = true;
        break;
      default:
        break;
      }
    }
    if (Name.empty() || !Type)
      throw Error("unnamed or untyped graph initializer");
    T.Type = "onnx:" + std::to_string(Type);
    T.Elements = 1;
    for (auto D : T.Shape)
      T.Elements = product(T.Elements, D);
    auto Expected = storageBytes(Type, T.Elements);
    if (External || !References.empty()) {
      if (!References.contains("location"))
        throw Error("missing external initializer location");
      auto S = References.at("location").get<std::string>();
      if (S.find_first_of(":\\") != S.npos)
        throw Error("nonlocal external initializer reference");
      if (Raw || Typed)
        throw Error("conflicting inline and external initializer storage");
      auto Relative = utf8Path(Source).parent_path() / utf8Path(S);
      memberPath(P.Root, Relative);
      uint64_t Offset = externalNumber(References, "offset").value_or(0);
      auto Length = externalNumber(References, "length");
      if (!P.B.Opts.ExternalData)
        P.gap(Source, "external initializer bytes not opened",
              {{"tensor", Name},
               {"member", S},
               {"offset", Offset},
               {"length", Length ? Json(*Length) : Json(nullptr)}});
      else {
        T.FileId = pathToUTF8(Relative);
        auto &ExternalFile = P.file(T.FileId);
        if (Offset > ExternalFile.size() ||
            (Length && *Length > ExternalFile.size() - Offset))
          throw Error("external initializer range exceeds local member");
        auto Bytes = Length.value_or(ExternalFile.size() - Offset);
        if (!Expected)
          P.gap(Source, "unverified external initializer dtype",
                {{"tensor", Name}});
        else if (*Expected != Bytes)
          throw Error("external initializer byte size conflicts with shape");
        else {
          T.Offset = Offset;
          T.HasOffset = true;
          T.Bytes = Bytes;
        }
      }
    } else if (Raw && !Typed) {
      if (!Expected)
        P.gap(Source, "unverified initializer dtype", {{"tensor", Name}});
      else if (*Expected != Raw->End - Raw->At)
        throw Error("ONNX initializer byte size conflicts with shape");
      else {
        T.Offset = Raw->At;
        T.HasOffset = true;
        T.Bytes = Raw->End - Raw->At;
      }
    } else
      P.gap(Source, "typed, segmented or absent initializer storage",
            {{"tensor", Name}});
    P.addTensor(Scope + "/" + Name, std::move(T));
    return Name;
  }
  Json node(Proto R, const std::string &Id, unsigned Depth) {
    Json N = {{"id", Id},
              {"inputs", Json::array()},
              {"outputs", Json::array()},
              {"attributes", Json::array()}};
    while (R.Pos < R.End) {
      auto X = R.next();
      switch (X.Id) {
      case 1:
        P.B.entry();
        N["inputs"].push_back(R.label(X));
        break;
      case 2:
        P.B.entry();
        N["outputs"].push_back(R.label(X));
        break;
      case 3:
        N["name"] = R.label(X);
        break;
      case 4:
        N["op_type"] = R.label(X);
        break;
      case 7:
        N["domain"] = R.label(X);
        break;
      case 8:
        N["overload"] = R.label(X);
        break;
      case 5: {
        P.B.entry();
        auto A = R.sub(X);
        Json Attr = {{"offset", X.At}, {"end", X.End}};
        unsigned Sub = 0;
        while (A.Pos < A.End) {
          auto K = A.next();
          if (K.Id == 1)
            Attr["name"] = A.label(K);
          if (K.Id == 20)
            Attr["type"] = integer(K);
          if (K.Id == 6 || K.Id == 11) {
            auto Path = Id + "/attr" + std::to_string(N["attributes"].size()) +
                        "/graph" + std::to_string(Sub++);
            graph(A.sub(K), Path, Depth + 1, false);
            Attr["graphs"].push_back(Path);
          }
        }
        N["attributes"].push_back(std::move(Attr));
        break;
      }
      default:
        break;
      }
    }
    if (!N.contains("op_type"))
      throw Error("missing ONNX operator type");
    if (N.contains("domain") && N["domain"] != "" && N["domain"] != "ai.onnx" &&
        N["domain"] != "ai.onnx.ml")
      P.finding(
          "graph.custom-domain", Source, "info", "operator", "declared",
          "Operator uses a nonstandard domain. Its implementation was not "
          "loaded.",
          {{"node", Id}, {"domain", N["domain"]}, {"op_type", N["op_type"]}});
    return N;
  }

public:
  GraphReader(Package &P, const std::string &S) : P(P), Source(S) {}
  void graph(Proto R, std::string Path, unsigned Depth, bool Function) {
    if (Depth > limits::kDepth)
      throw Error("ONNX graph nesting limit");
    P.B.entry();
    Json G = {{"id", Path},
              {"artifact", Source},
              {"kind", Function ? "function" : "exported_graph"},
              {"offset", R.Pos},
              {"end", R.End},
              {"nodes", Json::array()},
              {"inputs", Json::array()},
              {"outputs", Json::array()}};
    std::map<std::string, std::pair<uint64_t, size_t>> Producers;
    std::set<std::string> Initializers, Inputs;
    SHA256Stream Topology;
    uint64_t Count = 0;
    while (R.Pos < R.End) {
      auto X = R.next();
      if (X.Id == (Function ? 1u : 2u))
        G["name"] = R.label(X);
      else if (X.Id == (Function ? 7u : 1u)) {
        P.B.entry();
        auto N = node(R.sub(X), Path + "/node" + std::to_string(Count), Depth);
        N["offset"] = X.At;
        N["end"] = X.End;
        Topology.update(llvm::StringRef(
            Json::array({N.at("op_type"), N.value("domain", std::string{}),
                         N.value("overload", std::string{}), N.at("inputs"),
                         N.at("outputs")})
                .dump()));
        for (size_t Slot = 0; Slot < N["outputs"].size(); ++Slot) {
          auto &Value = N["outputs"][Slot].get_ref<const std::string &>();
          if (!Value.empty() &&
              !Producers.emplace(Value, std::pair(Count, Slot)).second)
            throw Error("duplicate ONNX value producer");
        }
        if (Count >= P.B.Opts.Offset && G["nodes"].size() < P.B.Opts.Limit)
          G["nodes"].push_back(std::move(N));
        ++Count;
      } else if (Function && (X.Id == 4 || X.Id == 5))
        G[X.Id == 4 ? "inputs" : "outputs"].push_back(R.label(X));
      else if (Function && X.Id == 10)
        G["domain"] = R.label(X);
      else if (Function && X.Id == 9)
        G["opsets"].push_back(opset(R.sub(X)));
      else if (!Function && (X.Id == 11 || X.Id == 12)) {
        P.B.entry();
        G[X.Id == 11 ? "inputs" : "outputs"].push_back(port(R.sub(X)));
      } else if (!Function && X.Id == 5)
        Initializers.insert(tensor(R.sub(X), Path));
      else if (!Function && X.Id == 15)
        P.gap(Source, "sparse initializer not decoded", {{"offset", X.At}});
    }
    for (const auto &Port : G["inputs"]) {
      if (Port.is_string())
        Inputs.insert(Port.get<std::string>());
      else if (Port.contains("name"))
        Inputs.insert(Port.at("name").get<std::string>());
    }
    for (auto &Node : G["nodes"]) {
      Json Edges = Json::array();
      for (size_t Slot = 0; Slot < Node.at("inputs").size(); ++Slot) {
        auto &Value = Node.at("inputs")[Slot].get_ref<const std::string &>();
        if (Value.empty())
          continue;
        Json Edge = {{"input_slot", Slot}, {"value", Value}};
        if (auto It = Producers.find(Value); It != Producers.end()) {
          Edge["source"] = "node";
          Edge["node"] = Path + "/node" + std::to_string(It->second.first);
          Edge["output_slot"] = It->second.second;
        } else if (Initializers.contains(Value))
          Edge["source"] = Inputs.contains(Value)
                               ? "graph-input-with-initializer"
                               : "initializer";
        else
          Edge["source"] = Inputs.contains(Value)
                               ? "graph-input"
                               : "unresolved-or-enclosing-scope";
        Edges.push_back(std::move(Edge));
      }
      Node["input_edges"] = std::move(Edges);
    }
    Topology.update(
        llvm::StringRef(Json::array({G.at("inputs"), G.at("outputs")}).dump()));
    P.Fingerprints["graph-topology/" + Path] =
        llvm::toHex(Topology.final(), true);
    G["node_count"] = Count;
    G["node_offset"] = P.B.Opts.Offset;
    P.Graphs.push_back(std::move(G));
  }
};
} // namespace
void readONNX(Package &P, const std::string &Name) {
  File &F = P.file(Name);
  SHA256Stream Observed;
  Proto R{F, P.B, 0, F.size(), Observed};
  GraphReader Reader(P, Name);
  bool Graph = false;
  uint64_t IR = 0;
  unsigned Func = 0;
  Json Ops = Json::array();
  while (R.Pos < R.End) {
    auto X = R.next();
    if (X.Id == 1)
      IR = integer(X);
    if (X.Id == 8)
      Ops.push_back(opset(R.sub(X)));
    if (X.Id == 7) {
      if (Graph)
        throw Error("duplicate ONNX model graph");
      Graph = true;
      Reader.graph(R.sub(X), "graph", 0, false);
    }
    if (X.Id == 25)
      Reader.graph(R.sub(X), "function" + std::to_string(Func++), 0, true);
    if (X.Id == 20)
      P.gap(Name, "training graph not inspected", {{"offset", X.At}});
  }
  if (!IR || !Graph)
    throw Error("missing ONNX IR version or graph");
  if (IR > 14)
    P.gap(Name, "unqualified ONNX IR version", {{"version", IR}});
  P.Artifacts.push_back(
      {{"id", Name},
       {"format", "onnx"},
       {"observed_fields_sha256", llvm::toHex(Observed.final(), true)},
       {"hash_scope", "offset, length and bytes of each metadata read"},
       {"ir_version", IR},
       {"opsets", Ops},
       {"size", F.size()}});
}
} // namespace neverd::model
