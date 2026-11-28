//===- Artifact.cpp - Seekable model inputs and report publication --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Seekable model inputs and report publication.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "neverd/support/FilePath.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace neverd::model {
namespace sysfs = llvm::sys::fs;
void Budget::tick(uint64_t Count) {
  if (Opts.Cancel && Opts.Cancel())
    throw Stop("cancelled");
  if (Count > WorkLeft)
    throw Stop("parser work budget exhausted");
  WorkLeft -= Count;
}
void Budget::entry() {
  if (Entries == limits::kEntries)
    throw Stop("inventory entry budget exhausted");
  ++Entries;
}
uint64_t sum(uint64_t A, uint64_t B) {
  if (B > UINT64_MAX - A)
    throw Error("byte offset overflow");
  return A + B;
}
uint64_t product(uint64_t A, uint64_t B) {
  if (A && B > UINT64_MAX / A)
    throw Error("shape or length overflow");
  return A * B;
}
uint64_t natural(const Json &J) {
  if (!J.is_number_unsigned())
    throw Error("expected an unsigned integer");
  return J.get<uint64_t>();
}
std::string digest(std::string_view Data) {
  return llvm::toHex(neverd::sha256(llvm::arrayRefFromStringRef(Data)), true);
}
void validName(std::string_view S) {
  if (S.empty() || S.size() > limits::kNameBytes || !llvm::json::isUTF8(S) ||
      S.find('\0') != S.npos)
    throw Error("invalid or oversized identifier");
}
fs::path utf8Path(std::string_view Name) {
  return fs::path(std::u8string(Name.begin(), Name.end()));
}
fs::path memberPath(const fs::path &Root, const fs::path &Relative) {
  if (Relative.empty() || Relative.has_root_path())
    throw Error("absolute or empty member path");
  for (const auto &Part : Relative)
    if (Part == "..")
      throw Error("parent path in package member");
  std::error_code EC;
  auto Path = fs::canonical(Root / Relative, EC);
  if (EC)
    throw Error("missing or inaccessible package member");
  auto Local = Path.lexically_relative(Root);
  if (Local.empty() || *Local.begin() == "..")
    throw Error("package member escapes selected root");
  return Path;
}
File::File(fs::path R, fs::path Name)
    : Root(std::move(R)), Relative(std::move(Name)) {
  const auto Path = pathToUTF8(memberPath(Root, Relative));
  sysfs::file_status Before;
  if (sysfs::status(Path, Before) || !sysfs::is_regular_file(Before))
    throw Error("input is not a local regular file");
  auto Open = sysfs::openNativeFileForRead(Path);
  if (!Open) {
    llvm::consumeError(Open.takeError());
    throw Error("cannot open package member");
  }
  Handle = *Open;
  if (sysfs::status(Handle, Snapshot) ||
      Snapshot.getUniqueID() != Before.getUniqueID()) {
    sysfs::closeFile(Handle);
    throw Error("file changed while opening");
  }
}
File::~File() { sysfs::closeFile(Handle); }
std::string File::snapshotID() const {
  const auto ID = Snapshot.getUniqueID();
  return digest(
      Json::array(
          {pathToUTF8(Relative), ID.getDevice(), ID.getFile(), size(),
           Snapshot.getLastModificationTime().time_since_epoch().count()})
          .dump());
}
void File::verify() const {
  sysfs::file_status Now;
  const auto Path = pathToUTF8(memberPath(Root, Relative));
  if (sysfs::status(Path, Now) || Now.getUniqueID() != Snapshot.getUniqueID() ||
      Now.getSize() != size() ||
      Now.getLastModificationTime() != Snapshot.getLastModificationTime())
    throw Error("file changed during inspection");
}
void File::read(uint64_t At, std::span<char> Out) {
  if (At > size() || Out.size() > size() - At)
    throw Error("truncated file range");
  while (!Out.empty()) {
    auto N = sysfs::readNativeFileSlice(Handle, {Out.data(), Out.size()}, At);
    if (!N) {
      llvm::consumeError(N.takeError());
      throw Error("file range read failed");
    }
    if (!*N)
      throw Error("short file read");
    ReadBytes += *N;
    At += *N;
    Out = Out.subspan(*N);
  }
}
std::string File::header(uint64_t At, uint64_t Length, Budget &B) {
  B.tick();
  if (At > size() || Length > size() - At)
    throw Error("truncated metadata");
  if (Length > B.HeaderLeft)
    throw Stop("metadata byte budget exhausted");
  B.HeaderLeft -= Length;
  std::string Data(Length, '\0');
  read(At, Data);
  return Data;
}
std::string hashRange(File &Input, uint64_t Offset, uint64_t Length, Budget &B,
                      llvm::raw_ostream *Copy) {
  if (Offset > Input.size() || Length > Input.size() - Offset)
    throw Error("hash range outside file");
  SHA256Stream Hash;
  std::array<char, limits::kChunk> Buffer;
  while (Length) {
    B.tick();
    auto Chunk =
        std::span(Buffer).first(std::min<uint64_t>(Length, Buffer.size()));
    Input.read(Offset, Chunk);
    Hash.update(llvm::StringRef(Chunk.data(), Chunk.size()));
    if (Copy)
      Copy->write(Chunk.data(), Chunk.size());
    Offset += Chunk.size();
    Length -= Chunk.size();
  }
  return llvm::toHex(Hash.final(), true);
}
File &Package::file(const std::string &Name) {
  if (auto I = Files.find(Name); I != Files.end())
    return *I->second;
  if (B.Files >= limits::kFiles)
    throw Stop("file count budget exhausted");
  ++B.Files;
  return *Files.emplace(Name, std::make_unique<File>(Root, utf8Path(Name)))
              .first->second;
}
void Package::addTensor(std::string Name, Tensor T) {
  B.entry();
  validName(Name);
  if (!Tensors.emplace(Name, std::move(T)).second)
    throw Error("duplicate tensor identity");
  // Layer numbers are a name-based view, including MLX module prefixes.
  llvm::StringRef S(Name);
  auto Tail =
      S.starts_with("blk.") ? S.drop_front(4) : S.split(".layers.").second;
  if (Tail.empty() && S.starts_with("layers."))
    Tail = S.drop_front(7);
  auto Parts = Tail.split('.');
  unsigned Layer;
  if (!Parts.second.empty() && !Parts.first.getAsInteger(10, Layer)) {
    auto &Count = Layers[std::to_string(Layer)];
    Count = Count.is_null() ? 1 : Count.get<unsigned>() + 1;
  }
}
void Package::finding(std::string Rule, std::string Source,
                      std::string Severity, std::string Category,
                      std::string Confidence, std::string Message,
                      Json Evidence) {
  ++FindingCount;
  if (Severity == "review")
    ++ReviewCount;
  if (Findings.size() < limits::kFindings)
    Findings.push_back({{"rule", Rule},
                        {"source", Source},
                        {"severity", Severity},
                        {"category", Category},
                        {"confidence", Confidence},
                        {"message", Message},
                        {"evidence", std::move(Evidence)}});
}
void Package::gap(std::string Source, std::string Reason, Json Evidence) {
  ++GapCount;
  if (Gaps.size() < limits::kFindings)
    Gaps.push_back({{"source", Source},
                    {"reason", Reason},
                    {"evidence", std::move(Evidence)}});
}
std::string Package::snapshotID() const {
  SHA256Stream Hash;
  for (const auto &[Name, F] : Files)
    Hash.update(llvm::StringRef(F->snapshotID()));
  return llvm::toHex(Hash.final(), true);
}
void Package::verify() const {
  for (const auto &[Name, F] : Files)
    F->verify();
}
namespace {
class ReportOutput final
    : public nlohmann::detail::output_adapter_protocol<char> {
public:
  std::string Text;
  void write_character(char C) override { write_characters(&C, 1); }
  void write_characters(const char *Chars, size_t N) override {
    if (N > limits::kReportBytes - Text.size())
      throw Stop("report byte limit reached; request a smaller page or select "
                 "one package");
    Text.append(Chars, N);
  }
};
Json publish(const Package &P, const Options &O) {
  Json Rows = Json::array();
  Json Members = Json::array();
  size_t Index = 0;
  uint64_t Read = 0, FileBytes = 0, Known = 0;
  for (const auto &[Name, F] : P.Files) {
    Members.push_back(
        {{"id", Name}, {"size", F->size()}, {"snapshot_id", F->snapshotID()}});
    Read = sum(Read, F->ReadBytes);
    FileBytes = sum(FileBytes, F->size());
  }
  for (const auto &[Name, T] : P.Tensors) {
    if (T.Bytes)
      Known = sum(Known, *T.Bytes);
    if (Index++ < O.Offset || Rows.size() >= O.Limit)
      continue;
    Rows.push_back(
        {{"name", Name},
         {"artifact", T.FileId},
         {"type", T.Type},
         {"shape", T.Shape},
         {"logical_elements", T.Elements},
         {"offset", T.HasOffset ? Json(T.Offset) : Json(nullptr)},
         {"storage_bytes", T.Bytes ? Json(*T.Bytes) : Json(nullptr)}});
    if (T.Packing)
      Rows.back()["storage_block"] = {{"elements", T.Packing->Elements},
                                      {"bytes", T.Packing->Bytes}};
  }
  Json Templates = Json::array();
  for (const auto &[Source, Record] : P.Templates)
    Templates.push_back(Record);
  return {{"id", P.Id},
          {"snapshot_id", P.snapshotID()},
          {"members", std::move(Members)},
          {"artifacts", P.Artifacts},
          {"declared_architectures", P.Architectures},
          {"named_layer_tensor_counts", P.Layers},
          {"inferred_projection_layouts", P.ProjectionLayouts},
          {"graphs", P.Graphs},
          {"templates", Templates},
          {"metadata_count", P.MetadataCount},
          {"metadata", P.Metadata},
          {"tensor_count", P.Tensors.size()},
          {"tensor_offset", O.Offset},
          {"tensors", std::move(Rows)},
          {"identified_storage_bytes", Known},
          {"file_bytes", FileBytes},
          {"bytes_read", Read},
          {"tensor_payload_bytes_read", P.WeightBytesRead},
          {"payload_coverage", P.WeightBytesRead ? "selected ranges only"
                                                 : "no tensor values read"},
          {"whole_file_hashes_computed", false},
          {"findings", P.Findings},
          {"finding_count", P.FindingCount},
          {"coverage_gaps", P.Gaps},
          {"gap_count", P.GapCount}};
}
} // namespace
std::string scan(const fs::path &Input, const Options &O) {
  Json Report = {
      {"schema", "neverd.model-scan.v1"},
      {"packages", Json::array()},
      {"reader_revision", 2},
      {"scope", "static file layout, templates and package declarations"},
      {"behavior_evaluated", false}};
  try {
    if (O.Limit > limits::kMaxPage)
      throw Error("tensor page size exceeds limit");
    Budget B(O);
    auto Packages = readInputs(Input, B);
    if (O.Range) {
      if (Packages.size() != 1)
        throw Error("select one package for a tensor range");
      auto &P = *Packages.front();
      auto I = P.Tensors.find(O.Range->Name);
      if (I == P.Tensors.end() || !I->second.Bytes)
        throw Error("tensor has no validated storage range");
      auto &T = I->second;
      if (O.Range->Offset > *T.Bytes ||
          O.Range->Length > *T.Bytes - O.Range->Offset ||
          O.Range->Length > limits::kCompareBytes)
        throw Error("requested tensor window exceeds storage or byte limit");
      auto At = sum(T.Offset, O.Range->Offset);
      auto &F = P.file(T.FileId);
      F.verify();
      auto Hash = hashRange(F, At, O.Range->Length, B);
      P.WeightBytesRead += O.Range->Length;
      F.verify();
      Report["tensor_range"] = {{"tensor", I->first},
                                {"artifact", T.FileId},
                                {"offset", At},
                                {"length", O.Range->Length},
                                {"sha256", Hash}};
    }
    if (!O.Baseline.empty()) {
      auto Base = readInputs(O.Baseline, B);
      if (Base.size() != 1 || Packages.size() != 1)
        throw Error("select one candidate and one baseline package");
      auto Compared = compare(*Packages.front(), *Base.front());
      Packages.front()->verify();
      Base.front()->verify();
      Report["comparison"] = std::move(Compared);
      Report["baseline"] = publish(*Base.front(), O);
    }
    bool Partial = false, Review = false;
    if (Report.contains("baseline"))
      Partial = Report["baseline"]["gap_count"] != 0 ||
                !Report["comparison"]["comparison_complete"].get<bool>();
    for (const auto &P : Packages) {
      P->verify();
      Partial |= P->GapCount != 0;
      Review |= P->ReviewCount != 0;
      Report["packages"].push_back(publish(*P, O));
    }
    bool Cancelled = O.Cancel && O.Cancel();
    Report["status"] = Cancelled ? "cancelled"
                       : Partial ? "partial"
                       : Review  ? "review"
                                 : "inspected";
    Report["exit_code"] = Cancelled || Partial ? 1 : Review ? 2 : 0;
  } catch (const Stop &E) {
    Report["status"] = O.Cancel && O.Cancel() ? "cancelled" : "partial";
    Report["error"] = E.what();
    Report["exit_code"] = 1;
  } catch (const Error &E) {
    Report.erase("tensor_range");
    Report.erase("comparison");
    Report.erase("baseline");
    Report["status"] = "failed";
    Report["error"] = E.what();
    Report["exit_code"] = 1;
  } catch (const fs::filesystem_error &) {
    Report["status"] = "failed";
    Report["error"] = "filesystem operation failed";
    Report["exit_code"] = 1;
  } catch (const Json::exception &) {
    Report["status"] = "failed";
    Report["error"] = "invalid typed metadata";
    Report["exit_code"] = 1;
  }
  try {
    auto Out = std::make_shared<ReportOutput>();
    nlohmann::detail::serializer<Json> Encoder(Out, ' ',
                                               Json::error_handler_t::replace);
    Encoder.dump(Report, false, true, 0);
    return std::move(Out->Text);
  } catch (const Stop &E) {
    return Json{{"schema", "neverd.model-scan.v1"},
                {"status", "partial"},
                {"exit_code", 1},
                {"error", E.what()}}
        .dump();
  }
}
std::string scanJSON(const char *Path, const char *Settings,
                     std::function<bool()> Cancel) {
  try {
    Options O;
    O.Cancel = std::move(Cancel);
    Budget Admission(O);
    std::string_view Text = Settings ? Settings : "{}";
    if (Text.size() > limits::kTemplateBytes)
      throw Error("model options exceed byte limit");
    auto J = parseObject(Text, Admission);
    if (auto Embedded = J.find("options_json"); Embedded != J.end()) {
      auto Combined =
          parseObject(Embedded->get_ref<const std::string &>(), Admission);
      J.erase(Embedded);
      Combined.update(J);
      J = std::move(Combined);
    }
    for (auto &[Key, V] : J.items()) {
      if (Key == "baseline") {
        auto S = V.get<std::string>();
        validName(S);
        O.Baseline = utf8Path(S);
      } else if (Key == "tensor_range") {
        auto Name = V.at("name").get<std::string>();
        validName(Name);
        O.Range = TensorRange{std::move(Name), natural(V.at("offset")),
                              natural(V.at("length"))};
      } else if (Key == "external_data")
        O.ExternalData = V.get<bool>();
      else if (Key == "scale_bytes")
        O.ScaleBytes = natural(V);
      else if (Key == "gap_bytes")
        O.GapBytes = natural(V);
      else if (Key == "compare_bytes")
        O.CompareBytes = natural(V);
      else if (Key == "offset")
        O.Offset = natural(V);
      else if (Key == "limit") {
        auto N = natural(V);
        if (N > limits::kMaxPage)
          throw Error("model page size exceeds limit");
        O.Limit = N;
      } else
        throw Error("unknown model scan option");
    }
    if (J.contains("compare_bytes") && O.Baseline.empty())
      throw Error("weight comparison requires a baseline");
    return scan(utf8Path(Path ? Path : ""), O);
  } catch (const Stop &E) {
    return Json{{"schema", "neverd.model-scan.v1"},
                {"status", "cancelled"},
                {"exit_code", 1},
                {"error", E.what()}}
        .dump();
  } catch (const Error &E) {
    return Json{{"schema", "neverd.model-scan.v1"},
                {"status", "failed"},
                {"exit_code", 1},
                {"error", E.what()}}
        .dump();
  } catch (const Json::exception &) {
    return R"({"schema":"neverd.model-scan.v1","status":"failed","exit_code":1,"error":"invalid model scan option"})";
  }
}
} // namespace neverd::model
