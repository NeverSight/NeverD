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
#include <bit>
#include <cmath>

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
Json valueType(Proto R, unsigned Depth) {
  if (Depth > limits::kDepth)
    throw Error("ONNX type nesting limit");
  Json Type = Json::object();
  while (R.Pos < R.End) {
    auto X = R.next();
    if (X.Id == 6) {
      Type["denotation"] = R.label(X);
      continue;
    }
    if (X.Id != 1 && X.Id != 4 && X.Id != 5 && X.Id != 7 && X.Id != 8 &&
        X.Id != 9) {
      Type["opaque_type_field"] = X.Id;
      continue;
    }
    if (Type.contains("kind"))
      throw Error("multiple ONNX value type alternatives");
    auto T = R.sub(X);
    Type["kind"] = X.Id == 1   ? "tensor"
                   : X.Id == 4 ? "sequence"
                   : X.Id == 5 ? "map"
                   : X.Id == 7 ? "opaque"
                   : X.Id == 8 ? "sparse_tensor"
                               : "optional";
    while (T.Pos < T.End) {
      auto F = T.next();
      if ((X.Id == 4 || X.Id == 9) && F.Id == 1)
        Type["element"] = valueType(T.sub(F), Depth + 1);
      else if (X.Id == 5 && F.Id == 1)
        Type["key_type"] = integer(F);
      else if (X.Id == 5 && F.Id == 2)
        Type["value"] = valueType(T.sub(F), Depth + 1);
      else if (X.Id == 7 && (F.Id == 1 || F.Id == 2))
        Type[F.Id == 1 ? "domain" : "name"] = T.label(F);
      else if ((X.Id == 1 || X.Id == 8) && F.Id == 1)
        Type["element_type"] = integer(F);
      else if ((X.Id == 1 || X.Id == 8) && F.Id == 2) {
        auto S = T.sub(F);
        auto &Shape = Type["shape"] = Json::array();
        while (S.Pos < S.End) {
          auto D = S.next();
          if (D.Id != 1)
            continue;
          S.B.entry();
          auto Dim = S.sub(D);
          Json Value = nullptr;
          while (Dim.Pos < Dim.End) {
            auto I = Dim.next();
            if ((I.Id == 1 || I.Id == 2) && !Value.is_null())
              throw Error("multiple ONNX dimension alternatives");
            if (I.Id == 1) {
              if (integer(I) > INT64_MAX)
                throw Error("negative ONNX dimension");
              Value = I.Value;
            } else if (I.Id == 2)
              Value = Dim.label(I);
          }
          Shape.push_back(std::move(Value));
        }
      }
    }
  }
  return Type;
}

Json port(Proto R) {
  Json V = Json::object();
  while (R.Pos < R.End) {
    auto X = R.next();
    if (X.Id == 1)
      V["name"] = R.label(X);
    if (X.Id == 2) {
      auto Type = valueType(R.sub(X), 0);
      for (auto Key : {"shape", "element_type"})
        if (Type.contains(Key))
          V[Key] = Type[Key];
      V["type"] = std::move(Type);
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
  struct Scope {
    std::string Parent;
    uint64_t ParentNode = 0;
    std::map<std::string, std::pair<uint64_t, size_t>> Producers;
    std::set<std::string> Initializers, Inputs;
  };
  std::map<std::string, Scope> Scopes;
  std::string tensor(Proto R, const std::string &Scope,
                     bool Attribute = false) {
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
    if (Name.empty() && Attribute)
      Name = "value";
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
  Json attribute(Proto A, const std::string &Path, const std::string &Parent,
                 uint64_t Owner, unsigned Depth) {
    Json Attr = {{"offset", A.Pos}, {"end", A.End}};
    size_t Children = 0;
    while (A.Pos < A.End) {
      auto K = A.next();
      switch (K.Id) {
      case 1:
        Attr["name"] = A.label(K);
        break;
      case 20:
        Attr["type"] = integer(K);
        break;
      case 21:
        Attr["ref_attr_name"] = A.label(K);
        break;
      case 2:
        if (K.Wire != 5)
          throw Error("invalid ONNX float attribute");
        Attr["float_bits"] =
            llvm::support::endian::read32le(A.metadata(K.At, 4).data());
        break;
      case 3:
        Attr["int"] = std::bit_cast<int64_t>(integer(K));
        break;
      case 6:
      case 11: {
        auto Child = Path + "/graph" + std::to_string(Children++);
        Scopes[Child].Parent = Parent;
        Scopes[Child].ParentNode = Owner;
        graph(A.sub(K), Child, Depth + 1, false);
        Attr["graphs"].push_back(Child);
        break;
      }
      case 5:
      case 10: {
        auto Child = Path + "/tensor" + std::to_string(Children++);
        auto Name = tensor(A.sub(K), Child, true);
        Attr["tensors"].push_back(Child + "/" + Name);
        break;
      }
      case 4:
      case 7:
      case 8:
      case 9: {
        // Preserve string bytes by digest. Large numeric lists are represented
        // by their encoded fields and coordinates instead of a JSON value dump.
        P.B.entry();
        Json F = {{"field", K.Id},
                  {"wire", K.Wire},
                  {"offset", K.At},
                  {"end", K.End}};
        if (K.Wire == 0)
          F["integer"] = std::bit_cast<int64_t>(K.Value);
        else {
          if (K.End - K.At > P.B.HeaderLeft)
            throw Stop("ONNX attribute exceeds metadata budget");
          P.B.HeaderLeft -= K.End - K.At;
          F["sha256"] = hashRange(A.F, K.At, K.End - K.At, P.B);
        }
        Attr["fields"].push_back(std::move(F));
        break;
      }
      case 14:
      case 15:
        P.B.entry();
        Attr["types"].push_back(valueType(A.sub(K), Depth + 1));
        break;
      case 13:
        break; // Documentation is optional and may contain private text.
      default:
        P.B.entry();
        Attr["opaque_fields"].push_back({{"field", K.Id},
                                         {"wire", K.Wire},
                                         {"offset", K.At},
                                         {"end", K.End}});
        P.gap(Source, "ONNX attribute field preserved as an opaque range",
              {{"attribute", Path},
               {"field", K.Id},
               {"offset", K.At},
               {"end", K.End}});
      }
    }
    return Attr;
  }
  Json node(Proto R, const std::string &Id, const std::string &Parent,
            uint64_t Owner, unsigned Depth) {
    const uint64_t Begin = R.Pos;
    Json N = {{"id", Id},
              {"evidence_level", "observed"},
              {"confidence", "serialized"},
              {"artifact", Source},
              {"inputs", Json::array()},
              {"outputs", Json::array()},
              {"attributes", Json::array()}};
    while (R.Pos < R.End) {
      auto X = R.next();
      switch (X.Id) {
      case 1:
        P.B.entry();
        N["inputs"].push_back(R.label(X));
        N["input_fields"].push_back({{"offset", X.At}, {"end", X.End}});
        break;
      case 2:
        P.B.entry();
        N["outputs"].push_back(R.label(X));
        N["output_fields"].push_back({{"offset", X.At}, {"end", X.End}});
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
        auto Path = Id + "/attr" + std::to_string(N["attributes"].size());
        N["attributes"].push_back(
            attribute(R.sub(X), Path, Parent, Owner, Depth));
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
          {{"node", Id},
           {"offset", Begin},
           {"end", R.End},
           {"domain", N["domain"]},
           {"op_type", N["op_type"]}});
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
    auto &Context = Scopes[Path];
    auto &Producers = Context.Producers;
    auto &Inputs = Context.Inputs;
    auto &Initializers = Context.Initializers;
    SHA256Stream Topology;
    uint64_t MatchCount = 0;
    uint64_t Count = 0;
    while (R.Pos < R.End) {
      auto X = R.next();
      if (X.Id == (Function ? 1u : 2u))
        G["name"] = R.label(X);
      else if (X.Id == (Function ? 7u : 1u)) {
        P.B.entry();
        auto N = node(R.sub(X), Path + "/node" + std::to_string(Count), Path,
                      Count, Depth);
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
        for (auto Attr : N.at("attributes")) {
          Attr.erase("offset");
          Attr.erase("end");
          for (auto Key : {"fields", "opaque_fields"})
            if (auto It = Attr.find(Key); It != Attr.end())
              for (auto &F : *It) {
                F.erase("offset");
                F.erase("end");
              }
          Topology.update(llvm::StringRef(Attr.dump()));
        }
        auto Matches = [&](const char *Key) {
          return N.contains(Key) &&
                 llvm::StringRef(N.at(Key).get_ref<const std::string &>())
                     .contains_insensitive(P.B.Opts.Search);
        };
        if ((P.B.Opts.Search.empty() || Matches("id") || Matches("name") ||
             Matches("op_type") || Matches("domain")) &&
            MatchCount++ >= P.B.Opts.Offset &&
            G["nodes"].size() < P.B.Opts.Limit)
          G["nodes"].push_back(std::move(N));
        ++Count;
      } else if (Function && (X.Id == 4 || X.Id == 5)) {
        G[X.Id == 4 ? "inputs" : "outputs"].push_back(R.label(X));
        G[X.Id == 4 ? "input_fields" : "output_fields"].push_back(
            {{"offset", X.At}, {"end", X.End}});
      } else if (Function && X.Id == 6)
        G["attribute_names"].push_back(R.label(X));
      else if (Function && X.Id == 11) {
        P.B.entry();
        G["default_attributes"].push_back(attribute(
            R.sub(X),
            Path + "/default" + std::to_string(G["default_attributes"].size()),
            Path, 0, Depth));
      } else if (Function && X.Id == 13)
        G["overload"] = R.label(X);
      else if (Function && X.Id == 10)
        G["domain"] = R.label(X);
      else if (Function && X.Id == 9)
        G["opsets"].push_back(opset(R.sub(X)));
      else if (X.Id == (Function ? 12u : 13u)) {
        P.B.entry();
        G["value_info"].push_back(port(R.sub(X)));
      } else if (!Function && (X.Id == 11 || X.Id == 12)) {
        P.B.entry();
        G[X.Id == 11 ? "inputs" : "outputs"].push_back(port(R.sub(X)));
        G[X.Id == 11 ? "input_fields" : "output_fields"].push_back(
            {{"offset", X.At}, {"end", X.End}});
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
        Json Edge = Node.at("input_fields").at(Slot);
        Edge.update(Json{{"input_slot", Slot},
                         {"value", Value},
                         {"artifact", Source},
                         {"evidence_level", "observed"},
                         {"confidence", "serialized port name"}});
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
    Json Signature = Json::object();
    for (auto Key : {"name", "domain", "overload", "inputs", "outputs",
                     "value_info", "opsets", "attribute_names"})
      if (G.contains(Key))
        Signature[Key] = G[Key];
    Topology.update(llvm::StringRef(Signature.dump()));
    P.Fingerprints["graph-topology/" + Path] =
        llvm::toHex(Topology.final(), true);
    size_t OutputIndex = 0;
    for (auto &Out : G["outputs"]) {
      auto Name = Out.is_string() ? Out.get<std::string>()
                                  : Out.value("name", std::string{});
      Json E = {{"value", Name},
                {"evidence_level", "observed"},
                {"artifact", Source}};
      E.update(G.at("output_fields").at(OutputIndex++));
      E["confidence"] = "serialized port name";
      if (auto It = Producers.find(Name); It != Producers.end()) {
        E["source"] = "node";
        E["node"] = Path + "/node" + std::to_string(It->second.first);
        E["output_slot"] = It->second.second;
      } else
        E["source"] = Initializers.contains(Name) ? "initializer"
                      : Inputs.contains(Name) ? "graph-input"
                                              : "unresolved-or-enclosing-scope";
      G["output_edges"].push_back(std::move(E));
    }
    G["node_count"] = Count;
    G["node_offset"] = P.B.Opts.Offset;
    G["matching_node_count"] = MatchCount;
    G["evidence_level"] = "observed";
    G["parent_scope"] =
        Context.Parent.empty() ? Json(nullptr) : Json(Context.Parent);
    G["topology_sha256"] = P.Fingerprints["graph-topology/" + Path];
    ++P.GraphCount;
    if ((P.B.Opts.GraphScope.empty() || Path == P.B.Opts.GraphScope) &&
        P.MatchingGraphs++ >= P.B.Opts.GraphOffset &&
        P.Graphs.size() < P.B.Opts.Limit)
      P.Graphs.push_back(std::move(G));
  }
  void resolveCaptures() {
    for (auto &G : P.Graphs) {
      auto resolve = [&](Json &E) {
        if (E.at("source") != "unresolved-or-enclosing-scope")
          return;
        const auto &Name = E.at("value").get_ref<const std::string &>();
        auto *Child = &Scopes.at(G.at("id").get_ref<const std::string &>());
        while (!Child->Parent.empty()) {
          P.B.tick();
          auto &Parent = Scopes.at(Child->Parent);
          if (auto It = Parent.Producers.find(Name);
              It != Parent.Producers.end() &&
              It->second.first < Child->ParentNode) {
            E["source"] = "enclosing-node";
            E["node"] =
                Child->Parent + "/node" + std::to_string(It->second.first);
            E["output_slot"] = It->second.second;
            E["scope"] = Child->Parent;
            break;
          }
          if (Parent.Inputs.contains(Name) ||
              Parent.Initializers.contains(Name)) {
            E["source"] = Parent.Initializers.contains(Name)
                              ? "enclosing-initializer"
                              : "enclosing-input";
            E["scope"] = Child->Parent;
            break;
          }
          Child = &Parent;
        }
      };
      for (auto &N : G.at("nodes"))
        for (auto &E : N.at("input_edges"))
          resolve(E);
      if (G.contains("output_edges"))
        for (auto &E : G.at("output_edges"))
          resolve(E);
    }
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
  Reader.resolveCaptures();
  if (!IR || !Graph)
    throw Error("missing ONNX IR version or graph");
  if (IR > 14)
    P.gap(Name, "unqualified ONNX IR version", {{"version", IR}});
  P.Artifacts.push_back(
      {{"id", Name},
       {"format", "onnx"},
       {"observed_fields_sha256", llvm::toHex(Observed.final(), true)},
       {"hash_scope", "read protobuf framing, labels and scalar fields; "
                      "separate attribute digests exclude tensor data"},
       {"ir_version", IR},
       {"opsets", Ops},
       {"size", F.size()}});
}
} // namespace neverd::model
