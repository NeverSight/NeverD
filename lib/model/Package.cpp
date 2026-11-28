//===- Package.cpp - Local packages, Safetensors and shard ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local packages, Safetensors and shard ownership.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "neverd/support/FilePath.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <tuple>

namespace neverd::model {
namespace {
using Names = std::vector<std::string>;
bool checkpoint(std::string_view S) {
  return S.ends_with(".pt") || S.ends_with(".pth") || S.ends_with(".bin") ||
         S.ends_with(".ckpt") || S.ends_with(".pickle") || S.ends_with(".pkl");
}
constexpr std::string_view Companions[] = {
    "chat_template.jinja",      "chat_template.json",
    "tokenizer_config.json",    "config.json",
    "adapter_config.json",      "special_tokens_map.json",
    "generation_config.json",   "processor_config.json",
    "preprocessor_config.json", "video_preprocessor_config.json"};
bool companion(std::string_view S) {
  return std::find(std::begin(Companions), std::end(Companions), S) !=
         std::end(Companions);
}
std::string_view opaqueFormat(std::string_view Name) {
  if (Name == "requirements.txt" || Name == "pyproject.toml")
    return "dependency-manifest";
  if (Name == "README.md" || Name.starts_with("LICENSE") ||
      Name.ends_with(".sig") || Name.ends_with(".attestation") ||
      Name.ends_with(".sigstore.json"))
    return "provenance";
  if (Name.ends_with(".py"))
    return "python-source";
  if (Name.ends_with(".dll") || Name.ends_with(".so") ||
      Name.ends_with(".dylib") || Name.ends_with(".pyd"))
    return "native-library";
  if (Name.ends_with(".engine") || Name.ends_with(".plan"))
    return "compiled-engine";
  return {};
}
struct GGUFPart {
  std::string First;
  unsigned Number, Count;
};
std::optional<GGUFPart> splitPart(llvm::StringRef Name) {
  if (!Name.consume_back(".gguf") || Name.size() < 15)
    return {};
  auto Tail = Name.take_back(15);
  if (Tail.front() != '-' || Tail.substr(6, 4) != "-of-" ||
      !std::all_of(Tail.begin() + 1, Tail.begin() + 6, llvm::isDigit) ||
      !std::all_of(Tail.end() - 5, Tail.end(), llvm::isDigit))
    return {};
  unsigned Part, Count;
  if (Tail.substr(1, 5).getAsInteger(10, Part) ||
      Tail.take_back(5).getAsInteger(10, Count))
    return {};
  return GGUFPart{
      (Name.drop_back(15) + "-00001" + Tail.drop_front(6) + ".gguf").str(),
      Part, Count};
}
bool member(std::string_view S) {
  return companion(S) || checkpoint(S) || !opaqueFormat(S).empty() ||
         S.ends_with(".gguf") || S.ends_with(".safetensors") ||
         S.ends_with(".safetensors.index.json") || S.ends_with(".onnx");
}
void list(const fs::path &Root, Names &Files, Names &Dirs, Budget &B) {
  for (const auto &Entry : fs::directory_iterator(Root)) {
    B.entry();
    B.tick();
    auto Name = pathToUTF8(Entry.path().filename());
    if (member(Name))
      Files.push_back(Name);
    else if (!Name.starts_with('.') && Entry.is_directory())
      Dirs.push_back(Name);
  }
  std::sort(Files.begin(), Files.end());
  std::sort(Dirs.begin(), Dirs.end());
}
Json jsonFile(Package &P, const std::string &Name) {
  File &F = P.file(Name);
  if (F.size() > limits::kJsonBytes)
    throw Error("JSON member exceeds byte limit");
  auto Text = F.header(0, F.size(), P.B);
  P.Fingerprints["metadata-file/" + Name] = digest(Text);
  return parseObject(Text, P.B);
}
void templateValues(Package &P, const std::string &Name, const Json &T) {
  auto add = [&](const std::string &Field, const std::string &Variant,
                 const Json &Value) {
    validName(Variant);
    if (!Value.is_string())
      throw Error("invalid companion template value");
    inspectTemplate(P, Name, Field, Value.get_ref<const std::string &>(), {},
                    Variant);
  };
  if (T.is_string())
    add("chat_template", "default", T);
  else if (T.is_array()) {
    std::set<std::string> Names;
    for (size_t I = 0; I < T.size(); ++I) {
      auto Variant = T.at(I).at("name").get<std::string>();
      if (!Names.insert(Variant).second)
        throw Error("duplicate named template");
      add("chat_template/" + std::to_string(I) + "/template", Variant,
          T.at(I).at("template"));
    }
  } else if (T.is_object()) {
    for (auto &[K, V] : T.items())
      add("chat_template/" + K, K, V);
  } else
    throw Error("unsupported companion template encoding");
}
void readCompanion(Package &P, const std::string &Name) {
  File &F = P.file(Name);
  Json Declarations = Json::object();
  if (Name.ends_with(".jinja")) {
    if (F.size() > limits::kTemplateBytes)
      throw Error("companion template exceeds byte limit");
    inspectTemplate(P, Name, "", F.header(0, F.size(), P.B), 0);
  } else {
    Json J = jsonFile(P, Name);
    if (auto T = J.find("chat_template"); T != J.end())
      templateValues(P, Name, *T);
    else if (Name == "chat_template.json")
      throw Error("companion lacks chat_template field");
    if (Name == "config.json") {
      auto declarations = [&](const Json &Config, llvm::StringRef Prefix) {
        for (auto Field :
             {"model_type", "num_hidden_layers", "tie_word_embeddings"}) {
          auto It = Config.find(Field);
          if (It == Config.end())
            continue;
          auto Kind = std::string_view(Field) == "num_hidden_layers"
                          ? "layer_count"
                      : std::string_view(Field) == "tie_word_embeddings"
                          ? "weight_tying"
                          : "architecture";
          if ((Kind == std::string_view("layer_count") &&
               !It->is_number_unsigned()) ||
              (Kind == std::string_view("weight_tying") && !It->is_boolean()) ||
              (Kind == std::string_view("architecture") && !It->is_string()))
            throw Error("invalid model structure declaration");
          if (It->is_string())
            validName(It->get_ref<const std::string &>());
          P.Declarations.push_back({{"artifact", Name},
                                    {"field", (Prefix + Field).str()},
                                    {"kind", Kind},
                                    {"value", *It},
                                    {"evidence_level", "declared"}});
        }
      };
      declarations(J, "");
      if (auto Text = J.find("text_config");
          Text != J.end() && Text->is_object())
        declarations(*Text, "text_config.");
      if (auto A = J.find("model_type"); A != J.end()) {
        auto S = A->get<std::string>();
        validName(S);
        P.Architectures.insert(S);
        Declarations["model_type"] = S;
      }
      if (auto A = J.find("architectures"); A != J.end() && !A->is_null()) {
        if (!A->is_array())
          throw Error("invalid declared model classes");
        for (const auto &Name : *A) {
          const auto &Class = Name.get_ref<const std::string &>();
          validName(Class);
          Declarations["architectures"].push_back(Class);
        }
      }
      if (auto V = J.find("quantization_config"); V != J.end())
        P.Fingerprints["quantization"] = digest(V->dump());
      else if (auto V = J.find("quantization"); V != J.end())
        P.Fingerprints["quantization"] = digest(V->dump());
    }
    inspectReferences(P, Name, J);
    if (J.contains("auto_map") && !J["auto_map"].empty())
      P.finding("package.custom-loader", Name, "review", "loader-declaration",
                "declared",
                "Configuration declares custom loader classes. Review the "
                "loader before enabling model code.",
                {{"field", "auto_map"}});
    if (J.value("trust_remote_code", Json(nullptr)) == true)
      P.finding("package.remote-code-enabled", Name, "review",
                "loader-declaration", "declared",
                "Configuration requests remote code loading. Review the loader "
                "settings.",
                {{"field", "trust_remote_code"}});
    if (Name == "adapter_config.json") {
      if (J.contains("peft_type") && J["peft_type"].is_string())
        Declarations["adapter_type"] = J["peft_type"];
      if (J.contains("r") && J["r"].is_number_unsigned())
        Declarations["rank"] = J["r"];
      if (J.contains("target_modules") && J["target_modules"].is_array())
        Declarations["target_module_count"] = J["target_modules"].size();
      if (J.contains("base_model_name_or_path"))
        Declarations["base_identity_sha256"] =
            digest(J["base_model_name_or_path"].dump());
      for (auto K : {"peft_type", "base_model_name_or_path", "target_modules",
                     "r", "lora_alpha"})
        if (J.contains(K))
          P.Fingerprints[std::string("adapter/") + K] = digest(J[K].dump());
    }
    P.Fingerprints["config/" + Name] = digest(J.dump());
  }
  P.Artifacts.push_back({{"id", Name},
                         {"format", "companion"},
                         {"size", F.size()},
                         {"declarations", std::move(Declarations)}});
  if (auto It = P.Fingerprints.find("metadata-file/" + Name);
      It != P.Fingerprints.end())
    P.Artifacts.back()["file_sha256"] = It->second;
}
std::string relativeMember(const Json &V) {
  if (!V.is_string())
    throw Error("invalid shard reference");
  auto S = V.get<std::string>();
  validName(S);
  if (S.find_first_of("\\:") != S.npos || S.starts_with('/'))
    throw Error("nonlocal shard reference");
  for (const auto &C : utf8Path(S))
    if (C == ".." || C == ".")
      throw Error("unsafe shard reference");
  return S;
}
using Mapping = std::map<std::string, std::string>;
void index(Package &P, const std::string &Name, Mapping &M,
           std::set<std::string> &Shards) {
  auto J = jsonFile(P, Name);
  auto &Map = J.at("weight_map");
  if (!Map.is_object() || Map.empty())
    throw Error("invalid shard index weight_map");
  for (auto &[Tensor, Ref] : Map.items()) {
    P.B.entry();
    validName(Tensor);
    auto Shard = relativeMember(Ref);
    if (!Shard.ends_with(".safetensors"))
      throw Error("unsupported indexed shard type");
    if (!M.emplace(Tensor, Shard).second)
      throw Error("tensor appears in multiple indexes");
    Shards.insert(Shard);
  }
  P.Artifacts.push_back(
      {{"id", Name},
       {"format", "safetensors-index"},
       {"mapped_tensors", Map.size()},
       {"file_sha256", P.Fingerprints.at("metadata-file/" + Name)}});
}
void protectedRead(Package &P, const std::string &Name, const auto &Fn) {
  try {
    Fn();
  } catch (const Stop &) {
    throw;
  } catch (const LayoutError &E) {
    P.finding("layout.invalid-range", Name, "review", "container", "observed",
              E.what(), E.Evidence);
    P.gap(Name, "tensor layout validation stopped", E.Evidence);
  } catch (const Error &E) {
    P.gap(Name, E.what());
  } catch (const Json::exception &) {
    P.gap(Name, "malformed or missing typed JSON field");
  }
}
void inspect(Package &P, const Names &Files,
             const std::optional<std::string> &Selected) {
  Mapping Map;
  std::set<std::string> Shards;
  bool IndexSeen = false;
  // A selected checkpoint inventories only that file. A directory or index
  // selects a package and verifies every declared shard mapping.
  for (const auto &Name : Files) {
    bool Chosen = !Selected || *Selected == Name;
    if (companion(Name))
      protectedRead(P, Name, [&] { readCompanion(P, Name); });
    else if (Chosen && Name.ends_with(".safetensors.index.json")) {
      IndexSeen = true;
      protectedRead(P, Name, [&] { index(P, Name, Map, Shards); });
    } else if (Chosen && Name.ends_with(".safetensors"))
      Shards.insert(Name);
    else if (Chosen)
      protectedRead(P, Name, [&] {
        if (Name.ends_with(".onnx"))
          readONNX(P, Name);
        else if (checkpoint(Name))
          inspectCheckpoint(P, Name);
        else if (auto Format = opaqueFormat(Name); !Format.empty())
          inspectComponent(P, Name, Format);
        else
          readGGUF(P, Name);
      });
  }
  for (const auto &Name : Shards)
    protectedRead(P, Name, [&] { readSafetensors(P, Name); });
  if (IndexSeen) {
    for (const auto &[Name, Owner] : Map) {
      auto It = P.Tensors.find(Name);
      if (It == P.Tensors.end() || It->second.FileId != Owner)
        P.gap(Owner, "indexed tensor missing or assigned to another shard",
              {{"tensor", Name}});
    }
    for (const auto &[Name, T] : P.Tensors)
      if (Shards.contains(T.FileId) && !Map.contains(Name))
        P.gap(T.FileId, "unindexed tensor", {{"tensor", Name}});
  }
  for (auto Name :
       {"tokenizer.json", "vocab.json", "merges.txt", "tokenizer.model"}) {
    if (fs::symlink_status(P.Root / Name).type() == fs::file_type::not_found)
      continue;
    protectedRead(P, Name, [&] {
      File &F = P.file(Name);
      Json Artifact = {{"id", Name},
                       {"format", "tokenizer"},
                       {"size", F.size()},
                       {"hash_scope", "not requested"}};
      if (!P.B.Opts.Baseline.empty()) {
        if (F.size() > P.B.HeaderLeft)
          throw Error("tokenizer digest exceeds remaining metadata budget");
        P.B.HeaderLeft -= F.size();
        auto Hash = hashRange(F, 0, F.size(), P.B);
        P.Fingerprints[std::string("tokenizer/") + Name] = Hash;
        Artifact["file_sha256"] = Hash;
        Artifact["hash_scope"] = "complete file";
      }
      P.Artifacts.push_back(std::move(Artifact));
    });
  }
  if (P.Architectures.size() > 1)
    P.gap(P.Id, "conflicting declared architectures");
  std::set<uint64_t> Parts;
  std::optional<uint64_t> Total, TotalTensors;
  bool SplitError = false;
  for (const auto &A : P.Artifacts) {
    if (A.at("format") != "gguf")
      continue;
    auto Part = splitPart(A.at("id").get_ref<const std::string &>());
    if (A.at("split").empty()) {
      if (Part)
        P.gap(
            A.at("id"), "numbered GGUF part lacks split declarations",
            {{"filename_part", Part->Number}, {"filename_count", Part->Count}});
      continue;
    }
    auto &S = A["split"];
    if (!S.contains("split.no") || !S.contains("split.count") ||
        !S["split.no"].is_number_unsigned() ||
        !S["split.count"].is_number_unsigned()) {
      SplitError = true;
      continue;
    }
    auto N = natural(S["split.count"]), I = natural(S["split.no"]);
    if (Part && (Part->Count != N || !Part->Number || Part->Number - 1 != I))
      P.gap(A.at("id"), "GGUF split filename disagrees with declarations",
            {{"filename_part", Part->Number},
             {"filename_count", Part->Count},
             {"split.no", I},
             {"split.count", N}});
    if (!N || I >= N || (Total && *Total != N) || !Parts.insert(I).second)
      SplitError = true;
    Total = N;
    if (S.contains("split.tensors.count")) {
      auto &C = S["split.tensors.count"];
      if (!C.is_number_integer() || C.get<int64_t>() < 0)
        SplitError = true;
      else {
        auto T = C.get<uint64_t>();
        if (TotalTensors && *TotalTensors != T)
          SplitError = true;
        TotalTensors = T;
      }
    }
  }
  size_t GGUFCount =
      std::count_if(P.Artifacts.begin(), P.Artifacts.end(), [](const Json &A) {
        return A.value("format", std::string{}) == "gguf";
      });
  if (SplitError || (GGUFCount > 1 && Parts.size() != GGUFCount) ||
      (Total && Parts.size() != *Total) ||
      (TotalTensors && P.Tensors.size() != *TotalTensors))
    P.gap(P.Id, "missing or inconsistent GGUF split inventory");
  inspectStructure(P);
}
} // namespace
bool isNumberedGGUF(llvm::StringRef Name) {
  return splitPart(Name).has_value();
}
std::vector<std::unique_ptr<Package>> readInputs(const fs::path &Input,
                                                 Budget &B) {
  if (Input.empty())
    throw Error("a local model path is required");
  bool Directory = fs::is_directory(Input);
  auto Root =
      fs::canonical(Directory ? Input : fs::absolute(Input).parent_path());
  std::vector<std::unique_ptr<Package>> Result;
  auto add = [&](fs::path R, std::string Id, const Names &F,
                 std::optional<std::string> S) {
    auto P = std::make_unique<Package>(std::move(R), std::move(Id), B);
    try {
      inspect(*P, F, S);
    } catch (const Stop &E) {
      P->gap(P->Id, E.what());
    }
    Result.push_back(std::move(P));
  };
  auto directory = [&](const fs::path &R, const std::string &Id,
                       const Names &F) {
    Names Common, Weights;
    std::map<std::string, Names> Models;
    for (const auto &N : F) {
      if (companion(N) || !opaqueFormat(N).empty())
        Common.push_back(N);
      else if (N.ends_with(".gguf")) {
        auto Part = splitPart(N);
        Models[Part ? Part->First : N].push_back(N);
      } else if (N.ends_with(".onnx"))
        Models[N].push_back(N);
      else
        Weights.push_back(N);
    }
    if (!Weights.empty() || Models.empty()) {
      Weights.insert(Weights.end(), Common.begin(), Common.end());
      add(R, Id, Weights, {});
    }
    for (auto &[Model, Parts] : Models) {
      Parts.insert(Parts.end(), Common.begin(), Common.end());
      add(R, Id + "/" + Model, Parts, {});
    }
  };
  Names Files, Dirs;
  if (Directory)
    list(Root, Files, Dirs, B);
  if (!Directory) {
    auto Name = pathToUTF8(Input.filename());
    validName(Name);
    if (auto Part = splitPart(Name)) {
      list(Root, Files, Dirs, B);
      Names Parts;
      for (auto &N : Files) {
        auto Member = splitPart(N);
        if (companion(N) || (Member && Member->First == Part->First))
          Parts.push_back(N);
      }
      if (!fs::exists(Input))
        throw Error("selected GGUF part is missing");
      add(Root, Part->First, Parts, {});
    } else {
      for (auto Local : Companions) {
        if (Name != Local &&
            fs::symlink_status(Root / Local).type() != fs::file_type::not_found)
          Files.emplace_back(Local);
      }
      Files.push_back(Name);
      std::sort(Files.begin(), Files.end());
      add(Root, Name, Files, Name);
    }
  } else if (!Files.empty())
    directory(Root, pathToUTF8(Root.filename()), Files);
  else
    for (const auto &Dir : Dirs) {
      auto Child = memberPath(Root, utf8Path(Dir));
      Names F, D;
      list(Child, F, D, B);
      if (!F.empty())
        directory(Child, Dir, F);
    }
  if (Result.empty())
    throw Error(
        "no supported model members in the selected directory or its children");
  return Result;
}

void readSafetensors(Package &P, const std::string &Name) {
  File &F = P.file(Name);
  auto Prefix = F.header(0, 8, P.B);
  uint64_t N = 0;
  for (unsigned I = 0; I < 8; ++I)
    N |= uint64_t(static_cast<unsigned char>(Prefix[I])) << (8 * I);
  if (!N || N > limits::kJsonBytes)
    throw Error("invalid Safetensors header length");
  auto H = F.header(8, N, P.B);
  if (H.front() != '{')
    throw Error("invalid Safetensors JSON prefix");
  Json J = parseObject(H, P.B);
  uint64_t Start = 8 + N;
  static const std::map<std::string, unsigned> Bits = {
      {"BOOL", 8},    {"U8", 8},   {"I8", 8},   {"F8_E4M3", 8}, {"F8_E5M2", 8},
      {"F8_E8M0", 8}, {"I16", 16}, {"U16", 16}, {"F16", 16},    {"BF16", 16},
      {"I32", 32},    {"U32", 32}, {"F32", 32}, {"I64", 64},    {"U64", 64},
      {"F64", 64},    {"C64", 64}, {"F4", 4},   {"F6_E2M3", 6}, {"F6_E3M2", 6}};
  std::vector<std::pair<std::string, Tensor>> List;
  for (auto &[Id, V] : J.items()) {
    if (Id == "__metadata__") {
      if (!V.is_object())
        throw Error("invalid Safetensors metadata");
      for (auto &[K, Value] : V.items())
        if (!Value.is_string())
          throw Error("non-string Safetensors metadata");
      continue;
    }
    P.B.tick();
    validName(Id);
    Tensor T;
    T.FileId = Name;
    T.Type = V.at("dtype").get<std::string>();
    validName(T.Type);
    auto &Shape = V.at("shape"), &Offsets = V.at("data_offsets");
    if (!Shape.is_array() || Shape.size() > limits::kDepth ||
        !Offsets.is_array() || Offsets.size() != 2)
      throw Error("invalid Safetensors dimensions or offsets");
    uint64_t Elements = 1;
    for (auto &D : Shape) {
      T.Shape.push_back(natural(D));
      Elements = product(Elements, T.Shape.back());
    }
    T.Elements = Elements;
    auto Begin = natural(Offsets[0]), End = natural(Offsets[1]);
    if (End < Begin || End > F.size() - Start)
      throw LayoutError("out-of-bounds Safetensors range",
                        {{"tensor", Id},
                         {"data_start", Start},
                         {"relative_start", Begin},
                         {"relative_end", End}});
    T.Offset = Start + Begin;
    T.Bytes = End - Begin;
    if (auto It = Bits.find(T.Type); It != Bits.end()) {
      auto Total = product(Elements, It->second);
      if (Total % 8 || Total / 8 != *T.Bytes)
        throw LayoutError("Safetensors shape and storage size disagree",
                          {{"tensor", Id},
                           {"offset", T.Offset},
                           {"declared_bytes", *T.Bytes}});
    } else
      P.gap(Name, "unknown Safetensors dtype",
            {{"tensor", Id}, {"dtype", T.Type}});
    List.emplace_back(Id, std::move(T));
  }
  std::sort(List.begin(), List.end(), [](auto &L, auto &R) {
    return std::tie(L.second.Offset, L.second.Bytes, L.first) <
           std::tie(R.second.Offset, R.second.Bytes, R.first);
  });
  uint64_t End = Start;
  for (auto &[Id, T] : List) {
    if (T.Offset != End)
      throw LayoutError(
          "Safetensors data has a hole or overlap",
          {{"tensor", Id}, {"offset", T.Offset}, {"expected_offset", End}});
    End += *T.Bytes;
  }
  if (End != F.size())
    throw LayoutError("unindexed trailing Safetensors bytes",
                      {{"offset", End}, {"length", F.size() - End}});
  for (auto &[Id, T] : List)
    P.addTensor(std::move(Id), std::move(T));
  P.Artifacts.push_back({{"id", Name},
                         {"format", "safetensors"},
                         {"size", F.size()},
                         {"tensor_count", List.size()},
                         {"data_offset", Start},
                         {"header_sha256", digest(H)}});
}
} // namespace neverd::model
