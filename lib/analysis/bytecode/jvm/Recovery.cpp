//===- Recovery.cpp - Offline class and JAR recovery ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Index physical members before selecting a runtime view. Method bodies and
/// source rendering are requested independently of the inventory.
//===----------------------------------------------------------------------===//

#include "../ReportStream.h"
#include "Class.h"

#include "neverd/web/Error.h"
#include "neverd/web/PackageArchive.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

using namespace llvm;
namespace neverd::jvm {
namespace {
struct Options {
  unsigned Release = 0;
  bool Preview = false, Inventory = false;
  int Component = -1;
  std::string ClassName, Method;
  std::vector<std::string> Dependencies;
  size_t Expanded = limits::ExpandedBytes, Member = limits::MemberBytes,
         Work = limits::MethodWork, Items = limits::Items;
};
[[noreturn]] void option(StringRef Why) {
  throw Failure{"invalid_request", "invalid_option", Why.str(), 0};
}
Options options(const json::Object &O) {
  Options Q;
  for (auto &[Key, V] : O) {
    auto K = StringRef(Key);
    if (K == "release" || K == "component" || K == "max_member_bytes" ||
        K == "max_expanded_bytes" || K == "max_method_work" ||
        K == "max_job_items") {
      auto I = V.getAsInteger();
      if (!I)
        option("Expected an integer JVM option");
      if (K == "component") {
        if (*I < -1 || *I > INT_MAX)
          option("Invalid component index");
        Q.Component = *I;
        continue;
      }
      if (*I < 0)
        option("Negative JVM limit or release");
      auto N = size_t(*I);
      if (K == "release") {
        if (N > LastRelease)
          option("Unknown Java runtime release");
        Q.Release = N;
      } else if (K == "max_expanded_bytes" && N <= limits::ExpandedBytes)
        Q.Expanded = N;
      else if (K == "max_member_bytes" && N <= limits::MemberBytes)
        Q.Member = N;
      else if (K == "max_method_work" && N <= limits::MethodWork)
        Q.Work = N;
      else if (K == "max_job_items" && N <= limits::Items)
        Q.Items = N;
      else
        option("JVM option exceeds its maximum");
    } else if (K == "preview" || K == "inventory") {
      auto B = V.getAsBoolean();
      if (!B)
        option("Expected a boolean JVM option");
      (K == "preview" ? Q.Preview : Q.Inventory) = *B;
    } else if (K == "method" || K == "class") {
      auto S = V.getAsString();
      if (!S || S->contains('\0'))
        option("Invalid JVM selector");
      (K == "method" ? Q.Method : Q.ClassName) = S->str();
    } else if (K == "dependencies") {
      auto *A = V.getAsArray();
      if (!A || A->size() > limits::Dependencies)
        option("Invalid dependency list");
      for (auto &Entry : *A) {
        auto S = Entry.getAsString();
        if (!S || S->empty() || S->contains('\0'))
          option("Invalid dependency path");
        Q.Dependencies.push_back(S->str());
      }
    } else
      option("Unknown JVM recovery option");
  }
  if (Q.Preview && Q.Release < 12)
    option("Preview requires an explicit runtime release of 12 or later");
  return Q;
}
struct Input {
  std::string Name, Bytes, Hash;
};
struct Component {
  unsigned Id, InputIndex, Release = 0;
  std::string Path, Logical, SHA, Kind, Data;
  bool Eligible = true, Selected = false;
  std::optional<unsigned> Parent;
  uint64_t StoredOffset = 0, StoredBytes = 0, Size = 0;
  std::optional<Class> Parsed;
  std::optional<Failure> Error;
};
struct Index {
  std::deque<Component> Components;
  std::map<std::string, std::vector<unsigned>> Definitions;
  std::map<std::pair<unsigned, std::string>, unsigned> Sources;
  Hierarchy Parents;
  size_t Charge = 0, TextCharge = 0, Retained = 0;
};
struct InputFile {
  static std::string read(StringRef Path, size_t Available) {
    if (Path.empty() || Path.contains('\0'))
      option("Invalid input path");
    auto P = std::filesystem::path(std::u8string(Path.begin(), Path.end()));
    std::error_code EC;
    auto N = std::filesystem::file_size(P, EC);
    if (EC)
      throw Failure{"io_error", "file_read",
                    "Cannot read a regular local JVM input", 0};
    if (N > Available)
      throw Failure{"budget_exceeded", "input_bytes",
                    "JVM input byte budget exceeded", 0};
    std::string B(N, '\0');
    std::ifstream F(P, std::ios::binary);
    if (!F.read(B.data(), N) || F.peek() != std::char_traits<char>::eof() ||
        F.bad())
      throw Failure{"io_error", "file_extent",
                    "Cannot read a stable JVM input extent", 0};
    return B;
  }
};
bool multiRelease(StringRef Manifest) {
  // Only unfolded main attributes apply to the whole archive.
  std::string Key, Value;
  std::optional<bool> Enabled;
  auto finish = [&] {
    if (StringRef(Key).equals_insensitive("Multi-Release")) {
      if (Enabled)
        invalid(0, "jar_manifest",
                "Duplicate Multi-Release manifest attribute");
      Enabled = StringRef(Value).equals_insensitive("true");
    }
  };
  while (!Manifest.empty()) {
    size_t End = Manifest.find_first_of("\r\n");
    StringRef Line = Manifest.take_front(End);
    if (End == StringRef::npos)
      Manifest = {};
    else {
      bool CR = Manifest[End] == '\r';
      Manifest = Manifest.drop_front(End + 1);
      if (CR)
        Manifest.consume_front("\n");
    }
    if (Line.empty())
      break;
    if (Line.consume_front(" ")) {
      if (Key.empty())
        invalid(0, "jar_manifest", "Orphan manifest continuation");
      Value += Line;
      continue;
    }
    finish();
    auto [Name, Rest] = Line.split(':');
    if (Name.empty() || !Rest.consume_front(" "))
      invalid(0, "jar_manifest", "Invalid JAR manifest header");
    Key = Name.str();
    Value = Rest.str();
  }
  finish();
  return Enabled.value_or(false);
}
void variant(Component &M) {
  StringRef P = M.Path;
  M.Logical = M.Path;
  if (!P.consume_front("META-INF/versions/"))
    return;
  auto [N, Name] = P.split('/');
  unsigned V;
  if (N.empty() || N.front() == '0' ||
      N.find_first_not_of("0123456789") != StringRef::npos ||
      N.getAsInteger(10, V) || V < 9 || Name.empty() ||
      Name.starts_with("META-INF/")) {
    M.Eligible = false;
    return;
  }
  M.Release = V;
  M.Logical = Name.str();
}
void parse(Component &M, Job &J) {
  if (M.Error || M.Kind != "class")
    return;
  try {
    M.Parsed = readClass(M.Data, J);
    if (M.Parent && M.Release && M.Parsed->Major > 44 + uint64_t(M.Release))
      invalid(6, "variant_version",
              "Class version exceeds its physical JAR variant");
  } catch (const Failure &F) {
    if (StringRef(F.Status) == "cancelled" ||
        (StringRef(F.Code) == "job_items" || StringRef(F.Code) == "job_bytes"))
      throw;
    M.Error = F;
  }
}
bool previewAllowed(const Class &C, const Options &Q) {
  if (C.Major >= 56 && C.Minor == 65535)
    return Q.Preview && Q.Release == C.Major - 44;
  return !Q.Release || C.Major <= 44 + Q.Release;
}
bool classPathMatches(const Component &M) {
  if (!M.Parent)
    return true;
  auto Path = StringRef(M.Logical);
  return Path.consume_back(".class") && Path == M.Parsed->Name;
}
std::shared_ptr<Index> index(ArrayRef<Input> Inputs, const Options &Q, Job &J) {
  auto T = std::make_shared<Index>();
  auto add = [&](unsigned InputIndex, std::string Name) -> Component & {
    if (!J.Members)
      throw Failure{"budget_exceeded", "member_count",
                    "JVM component budget exceeded", 0};
    --J.Members;
    J.charge(1, 0);
    return T->Components.emplace_back(Component{
        unsigned(T->Components.size()), InputIndex, 0, std::move(Name)});
  };
  for (unsigned I = 0; I < Inputs.size(); ++I) {
    const auto &In = Inputs[I];
    auto &Root = add(I, In.Name);
    Root.Logical = Root.Path;
    Root.SHA = In.Hash;
    Root.Size = In.Bytes.size();
    Root.Selected = true;
    StringRef Bytes = In.Bytes;
    auto Ext = sys::path::extension(In.Name);
    bool Jar = Ext.equals_insensitive(".jar") ||
               Bytes.starts_with(StringRef("PK\x03\x04", 4)) ||
               Bytes.starts_with(StringRef("PK\x05\x06", 4));
    if (!Jar) {
      Root.Kind = Ext.equals_insensitive(".java") ? "source" : "class";
      if (Bytes.size() > Q.Member)
        Root.Error = Failure{"budget_exceeded", "member_bytes",
                             "JVM member byte budget exceeded", 0};
      else {
        Root.Data = Bytes.str();
        parse(Root, J);
      }
      continue;
    }
    Root.Kind = "jar";
    web::PackageArchive A;
    try {
      A = web::readJarArchive(Bytes, J.Expanded, J.Stop);
    } catch (const web::Error &E) {
      auto Code = StringRef(E.what());
      if (Code == "cancelled")
        J.check(0);
      throw Failure{Code.contains("budget") || Code.contains("ratio")
                        ? "budget_exceeded"
                        : "malformed",
                    "jar_container", E.what(), 0};
    }
    J.Expanded -= A.ExpandedBytes;
    bool Multi = false, HasManifest = false;
    for (auto &Z : A.Members)
      if (StringRef(Z.Path).equals_insensitive("META-INF/MANIFEST.MF") &&
          Z.available()) {
        if (Z.Size > Q.Member)
          throw Failure{"budget_exceeded", "manifest_bytes",
                        "JAR manifest exceeds member budget", 0};
        if (HasManifest)
          invalid(0, "jar_manifest", "Ambiguous JAR manifests");
        HasManifest = true;
        Multi = multiRelease(Z.Content.read(0, Z.Size));
      }
    Root.Eligible = Multi;
    std::map<std::string, unsigned> Chosen;
    for (auto &Z : A.Members) {
      auto &M = add(I, Z.Path);
      M.Parent = Root.Id;
      M.SHA = Z.BlobHash;
      M.Size = Z.Size;
      M.StoredOffset = Z.StoredOffset;
      M.StoredBytes = Z.StoredSize;
      variant(M);
      StringRef P = M.Logical;
      M.Kind =
          Z.Kind == "directory"   ? "directory"
          : P.ends_with(".class") ? "class"
          : P.ends_with(".java")  ? "source"
          : P.ends_with_insensitive(".jar") || P.ends_with_insensitive(".zip")
              ? "nested_archive"
              : "resource";
      if (!Z.available() && Z.Kind != "directory")
        M.Error = Failure{"unsupported", "zip_member", Z.UnavailableReason, 0};
      else if (M.Kind == "nested_archive")
        M.Error = Failure{"unsupported", "nested_archive",
                          "Nested archive retained as an opaque component", 0};
      else if (M.Kind == "class" || M.Kind == "source") {
        if (M.Size > Q.Member)
          M.Error = Failure{"budget_exceeded", "member_bytes",
                            "JVM member byte budget exceeded", 0};
        else {
          M.Data = Z.Content.read(0, Z.Size);
          parse(M, J);
        }
      }
      if (!M.Eligible || M.Release && (!Multi || M.Release > Q.Release))
        continue;
      auto [It, New] = Chosen.try_emplace(M.Logical, M.Id);
      if (!New && T->Components[It->second].Release < M.Release)
        It->second = M.Id;
    }
    for (auto &[Name, Id] : Chosen)
      T->Components[Id].Selected = true;
  }
  for (auto &M : T->Components)
    if (M.Parsed && !M.Error && M.Selected && classPathMatches(M) &&
        previewAllowed(*M.Parsed, Q))
      T->Definitions[M.Parsed->Name].push_back(M.Id);
  for (auto &M : T->Components)
    if (M.Selected && M.Kind == "source" && !M.Error)
      T->Sources.emplace(std::make_pair(M.InputIndex, M.Logical), M.Id);
  for (auto &[Name, Ids] : T->Definitions) {
    if (Ids.size() != 1)
      continue;
    auto &C = *T->Components[Ids.front()].Parsed;
    auto &Parents = T->Parents[Name];
    Parents = C.Interfaces;
    if (!C.Super.empty())
      Parents.push_back(C.Super);
  }
  T->Charge = Q.Items - J.Items;
  T->TextCharge = limits::MaterializedBytes - J.TextBytes;
  for (auto &M : T->Components)
    T->Retained += M.Data.size();
  return T;
}
json::Object report() {
  return json::Object{
      {"schema", "language-recovery-v1"},
      {"language", "Java"},
      {"profile", "jvm-class-jar-v1"},
      {"status", "unsupported"},
      {"evidence",
       json::Object{{"dialect", "JVM"}, {"producer_revision", nullptr}}},
      {"root", nullptr},
      {"functions", json::Array{}},
      {"components", json::Array{}},
      {"relations", json::Array{}},
      {"diagnostics", json::Array{}}};
}
void failed(json::Object &R, const Failure &F) {
  R["status"] = F.Status;
  R.getArray("diagnostics")->emplace_back(diagnostic(F));
}
std::string jniPart(StringRef S) {
  SmallVector<UTF16> Units;
  if (!convertUTF8ToUTF16String(S, Units))
    return {};
  std::string R;
  for (unsigned C : Units) {
    if (C < 128 && isAlnum(char(C)))
      R += char(C);
    else if (C == '/')
      R += '_';
    else if (C == '_')
      R += "_1";
    else if (C == ';')
      R += "_2";
    else if (C == '[')
      R += "_3";
    else
      R += "_0" + utohexstr(C, true, 4);
  }
  return R;
}
void relations(json::Array &A, const Component &X, const Index &T) {
  const auto &C = *X.Parsed;
  for (auto &Name : C.References) {
    json::Object O{
        {"kind", "class_reference"}, {"from", X.Id}, {"target", Name}};
    auto It = T.Definitions.find(Name);
    if (It == T.Definitions.end())
      O["resolution"] = "missing";
    else {
      json::Array Candidates;
      for (auto Id : It->second)
        Candidates.emplace_back(Id);
      O["resolution"] = It->second.size() == 1 ? "local" : "ambiguous";
      O["first_definition"] = It->second.front();
      O["candidates"] = std::move(Candidates);
    }
    A.emplace_back(std::move(O));
  }
}
void methodEdges(json::Array &Edges, const Class &C, const Member &M,
                 const json::Object &Row, unsigned Id, unsigned F) {
  if (M.Flags & 0x100) {
    auto Short = "Java_" + jniPart(C.Name) + "_" + jniPart(M.Name);
    auto Args = StringRef(M.Desc).drop_front().split(')').first;
    Edges.emplace_back(
        json::Object{{"kind", "jni_native"},
                     {"from", Id},
                     {"method", F},
                     {"resolution", "unresolved"},
                     {"short_name", Short},
                     {"long_name", Short + "__" + jniPart(Args)}});
  }
  for (auto &V : *Row.getArray("instructions")) {
    const auto &I = *V.getAsObject();
    auto Owner = I.getString("owner"), N = I.getString("name");
    StringRef Kind;
    if (I.getInteger("bootstrap_index"))
      Kind = "dynamic_bootstrap";
    else if (Owner == "java/lang/Class" && N == "forName" ||
             Owner == "java/lang/ClassLoader" && N == "loadClass")
      Kind = "dynamic_class_loading";
    else if (Owner == "java/lang/System" && (N == "load" || N == "loadLibrary"))
      Kind = "native_library";
    else if (Owner && Owner->starts_with("java/lang/reflect/"))
      Kind = "reflection";
    if (Kind.empty())
      continue;
    json::Object O{{"kind", Kind},
                   {"from", Id},
                   {"method", F},
                   {"pc", *I.getInteger("pc")},
                   {"resolution", "unresolved"}};
    for (auto Key :
         {"owner", "name", "descriptor", "bootstrap_index", "pool_index"})
      if (auto *Val = I.get(Key))
        O[Key] = *Val;
    Edges.emplace_back(std::move(O));
  }
}
json::Object view(const Index &T, const Options &Q, Job &J, const Input &Root) {
  auto R = report();
  R["input_sha256"] = Root.Hash;
  R["input_bytes"] = Root.Bytes.size();
  auto &E = *R.getObject("evidence");
  E["runtime_release"] = Q.Release;
  E["enable_preview"] = Q.Preview;
  auto &Functions = *R.getArray("functions"),
       &Components = *R.getArray("components"),
       &Edges = *R.getArray("relations");
  bool Matched = false, Partial = false, Source = false, Extracted = false;
  unsigned FunctionId = 0;
  int RootId = -1;
  for (auto &M : T.Components) {
    J.check(0);
    json::Object O{
        {"id", M.Id},
        {"name", M.Path},
        {"logical_name", M.Logical},
        {"kind", M.Kind},
        {"sha256", M.SHA.empty() ? json::Value(nullptr) : json::Value(M.SHA)},
        {"bytes", M.Size},
        {"input_index", M.InputIndex},
        {"release", M.Release},
        {"selected", M.Selected},
        {"diagnostics", json::Array{}}};
    if (M.Parent) {
      O["parent"] = *M.Parent;
      O["stored_offset"] = M.StoredOffset;
      O["stored_bytes"] = M.StoredBytes;
    }
    if (M.Kind == "jar")
      O["multi_release"] = M.Eligible;
    bool Selected = Q.Component >= 0 ? M.Id == unsigned(Q.Component)
                                     : !M.InputIndex && M.Selected;
    Selected &=
        Q.ClassName.empty() || (M.Parsed && M.Parsed->Name == Q.ClassName);
    O["requested"] = Selected;
    if (M.Error) {
      O["status"] = M.Error->Status;
      O.getArray("diagnostics")->emplace_back(diagnostic(*M.Error));
      Partial |= Selected;
      if (!M.Parent && !M.InputIndex && T.Components.size() == 1)
        failed(R, *M.Error);
    } else if (M.Kind == "source") {
      bool Valid = json::isUTF8(M.Data) && !StringRef(M.Data).contains('\0');
      O["status"] = Valid ? "extracted_source" : "unsupported";
      if (!Valid) {
        Failure F{"unsupported", "source_encoding",
                  "Attached Java source requires NUL-free UTF-8", 0};
        O.getArray("diagnostics")->emplace_back(diagnostic(F));
        Partial |= Selected;
        if (!M.Parent && !M.InputIndex)
          failed(R, F);
      }
      if (Selected && !Q.Inventory && Q.Method.empty() && Valid) {
        Functions.emplace_back(json::Object{{"id", FunctionId},
                                            {"component", M.Id},
                                            {"name", M.Path},
                                            {"status", "extracted_source"},
                                            {"source", M.Data},
                                            {"offset", 0},
                                            {"end_offset", M.Size},
                                            {"instructions", json::Array{}},
                                            {"diagnostics", json::Array{}}});
        if (RootId < 0)
          RootId = FunctionId;
        Extracted = true;
        Matched = true;
      }
      ++FunctionId;
    } else if (M.Parsed) {
      auto &C = *M.Parsed;
      Matched |= Selected && Q.Method.empty();
      O["status"] = "metadata_only";
      O["inventory"] =
          classInventory(C, Selected && (Q.Component >= 0 || !M.Parent ||
                                         !Q.ClassName.empty()));
      auto Identity = web::identity(
          "jvm-class",
          {Root.Hash, T.Components[M.Parent.value_or(M.Id)].SHA, M.Path,
           std::to_string(M.Release), std::to_string(Q.Release), C.Name});
      O["identity"] = Identity;
      O["classpath_name_matches"] = classPathMatches(M);
      if (M.Selected)
        relations(Edges, M, T);
      for (const auto &A : C.Attributes) {
        auto &Attr = *A.getAsObject();
        if (Attr.getString("name") != "SourceFile")
          continue;
        auto File = Attr.getString("value");
        if (!File || File->contains('/') || File->contains('\\'))
          continue;
        auto Slash = StringRef(M.Logical).rfind('/');
        auto Candidate = Slash == StringRef::npos
                             ? File->str()
                             : M.Logical.substr(0, Slash + 1) + File->str();
        auto S = T.Sources.find({M.InputIndex, Candidate});
        if (S != T.Sources.end())
          Edges.emplace_back(json::Object{{"kind", "source_file_attribute"},
                                          {"from", M.Id},
                                          {"to", S->second},
                                          {"resolution", "named_attachment"}});
      }
      bool Allowed = previewAllowed(C, Q);
      O["runtime_eligible"] = Allowed;
      if ((Selected || M.Selected) && !Allowed) {
        O.getArray("diagnostics")
            ->emplace_back(json::Object{
                {"code", "runtime_profile"},
                {"message",
                 "Class requires a compatible release and preview policy"}});
      }
      json::Array Methods;
      for (auto &Meth : C.Methods) {
        auto Key = Meth.Name + Meth.Desc;
        json::Object Summary{{"id", FunctionId},
                             {"name", Meth.Name},
                             {"descriptor", Meth.Desc},
                             {"flags", Meth.Flags},
                             {"offset", Meth.At}};
        if (Meth.Body) {
          Summary["code_offset"] = Meth.Body->At;
          Summary["code_bytes"] = Meth.Body->Bytes.size();
        }
        if (Selected && (Q.Method.empty() || Q.Method == Key)) {
          Matched = true;
          if (!Q.Inventory && Allowed) {
            auto F = analyzeMethod(C, Meth, J);
            F["id"] = FunctionId;
            F["component"] = M.Id;
            F["class"] = C.Name;
            F["method_key"] = Key;
            F["class_identity"] = Identity;
            methodEdges(Edges, C, Meth, F, M.Id, FunctionId);
            auto Status = *F.getString("status");
            Summary["status"] = Status.str();
            if (Status == "reconstructed_source") {
              Source = true;
              if (RootId < 0)
                RootId = FunctionId;
            } else
              Partial |= Status != "metadata_only";
            Functions.emplace_back(std::move(F));
          }
        }
        Methods.emplace_back(std::move(Summary));
        ++FunctionId;
      }
      O["methods"] = std::move(Methods);
    } else
      O["status"] = "metadata_only";
    Components.emplace_back(std::move(O));
  }
  R["root"] = RootId >= 0 ? json::Value(RootId) : json::Value(nullptr);
  if (!R.getArray("diagnostics")->empty())
    return R;
  if (Q.Component >= int(T.Components.size()) ||
      ((!Q.Method.empty() || !Q.ClassName.empty()) && !Matched)) {
    failed(R, {"invalid_request", "selection",
               "No matching local class or method", 0});
    return R;
  }
  R["status"] = Partial     ? "partial_reconstruction"
                : Source    ? "reconstructed_source"
                : Extracted ? "extracted_source"
                            : "metadata_only";
  return R;
}
// A single immutable index can serve successive method requests in either
// frontend. Re-read and hash local files before a hit. Per-call budgets still
// account for indexing work. No path-only or ambient-classpath lookup.
struct Cache {
  std::mutex Mutex;
  std::string Key;
  std::shared_ptr<const Index> Value;
};
std::shared_ptr<const Index> indexed(ArrayRef<Input> Ins, const Options &Q,
                                     Job &J, bool Cacheable) {
  if (!Cacheable)
    return index(Ins, Q, J);
  static Cache Last;
  std::string Key;
  for (auto &I : Ins) {
    Key += std::to_string(I.Name.size()) + "/" + I.Name + I.Hash;
  }
  Key += ":" + std::to_string(Q.Release) + ":" + std::to_string(Q.Member) +
         ":" + std::to_string(Q.Expanded) + ":" + std::to_string(Q.Items);
  Key += Q.Preview ? ":preview" : ":standard";
  {
    std::lock_guard Guard(Last.Mutex);
    if (Key == Last.Key && Last.Value) {
      J.charge(Last.Value->Charge, 0);
      J.text(Last.Value->TextCharge, 0);
      return Last.Value;
    }
  }
  auto T = index(Ins, Q, J);
  if (T->Retained <= limits::CachedMemberBytes &&
      T->Charge <= limits::CachedItems &&
      T->TextCharge <= limits::CachedMemberBytes) {
    std::lock_guard Guard(Last.Mutex);
    Last.Key = std::move(Key);
    Last.Value = T;
  }
  return T;
}
json::Object run(std::vector<Input> Inputs, const Options &Q, Cancel Stop,
                 bool Cacheable) {
  Job J{Stop, Q.Items, Q.Work, Q.Member, Q.Expanded};
  size_t Bytes = 0;
  for (auto &I : Inputs) {
    J.check(0);
    Bytes += I.Bytes.size();
  }
  if (Bytes > limits::InputBytes)
    throw Failure{"budget_exceeded", "input_bytes",
                  "JVM input byte budget exceeded", 0};
  auto &First = Inputs.front();
  First.Hash = web::sha256(First.Bytes);
  try {
    for (auto &D : Q.Dependencies) {
      J.check(0);
      auto B = InputFile::read(D, limits::InputBytes - Bytes);
      Bytes += B.size();
      auto Hash = web::sha256(B);
      Inputs.push_back(
          {sys::path::filename(D).str(), std::move(B), std::move(Hash)});
    }
    auto T = indexed(Inputs, Q, J, Cacheable);
    J.Parents = &T->Parents;
    return view(*T, Q, J, Inputs.front());
  } catch (const Failure &F) {
    auto R = report();
    R["input_sha256"] = Inputs.front().Hash;
    R["input_bytes"] = Inputs.front().Bytes.size();
    failed(R, F);
    return R;
  }
}
std::string serialize(json::Object R) {
  json::Value Full(std::move(R));
  std::string S;
  {
    neverd::bytecode::ReportStream OS(S, limits::ReportBytes);
    OS << Full;
    OS.flush();
    if (!OS.exceeded())
      return S;
  }
  auto Summary = report();
  for (auto K : {"input_sha256", "input_bytes"})
    if (auto *V = Full.getAsObject()->get(K))
      Summary[K] = *V;
  failed(Summary, {"budget_exceeded", "report_bytes",
                   "JVM report exceeds export budget", 0});
  S.clear();
  raw_string_ostream(S) << json::Value(std::move(Summary));
  return S;
}
} // namespace
json::Object recover(StringRef Bytes, StringRef Name,
                     const json::Object &Options, Cancel Stop) {
  try {
    if (Bytes.size() > limits::InputBytes)
      throw Failure{"budget_exceeded", "input_bytes",
                    "JVM input byte budget exceeded", 0};
    return run({Input{Name.str(), Bytes.str(), {}}}, options(Options), Stop,
               false);
  } catch (const Failure &F) {
    auto R = report();
    failed(R, F);
    return R;
  }
}
std::string recoverFileJSON(StringRef Path, StringRef OptionsJSON,
                            Cancel Stop) {
  try {
    if (Stop())
      throw Failure{"cancelled", "cancelled", "JVM recovery cancelled", 0};
    auto Parsed = json::parse(OptionsJSON.empty() ? "{}" : OptionsJSON);
    if (!Parsed) {
      consumeError(Parsed.takeError());
      option("Invalid JVM options JSON");
    }
    if (!Parsed->getAsObject())
      option("JVM options must be a JSON object");
    auto Q = options(*Parsed->getAsObject());
    auto B = InputFile::read(Path, limits::InputBytes);
    return serialize(
        run({Input{sys::path::filename(Path).str(), std::move(B), {}}}, Q, Stop,
            true));
  } catch (const Failure &F) {
    auto R = report();
    failed(R, F);
    return serialize(std::move(R));
  }
}
} // namespace neverd::jvm
