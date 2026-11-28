//===- ModelInternal.h - Validated model ranges and evidence --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validated model ranges and evidence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_MODEL_MODELINTERNAL_H
#define NEVERD_LIB_MODEL_MODELINTERNAL_H
#include "neverd/model/Artifact.h"

#include "llvm/Support/FileSystem.h"

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace neverd::model {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct Error : std::runtime_error {
  using runtime_error::runtime_error;
};
struct LayoutError : Error {
  Json Evidence;
  LayoutError(const char *Message, Json Evidence)
      : Error(Message), Evidence(std::move(Evidence)) {}
};
struct Stop : Error {
  using Error::Error;
};
struct Budget {
  const Options &Opts;
  uint64_t HeaderLeft = limits::kHeaderBytes, WorkLeft = limits::kWork;
  uint64_t GapLeft, ScaleLeft, CompareLeft;
  size_t Files = 0, Entries = 0;
  explicit Budget(const Options &O)
      : Opts(O), GapLeft(O.GapBytes), ScaleLeft(O.ScaleBytes),
        CompareLeft(O.CompareBytes) {}
  void tick(uint64_t Count = 1);
  void entry();
};
uint64_t product(uint64_t A, uint64_t B);
uint64_t sum(uint64_t A, uint64_t B);
uint64_t natural(const Json &J);
std::string digest(std::string_view Data);
Json parseObject(std::string_view Data, Budget &B);
void validName(std::string_view S);
fs::path utf8Path(std::string_view Name);
fs::path memberPath(const fs::path &Root, const fs::path &Relative);
struct File {
  fs::path Root, Relative;
  llvm::sys::fs::file_status Snapshot;
  llvm::sys::fs::file_t Handle = llvm::sys::fs::kInvalidFile;
  uint64_t ReadBytes = 0;
  explicit File(fs::path Root, fs::path Relative);
  ~File();
  File(const File &) = delete;
  std::string snapshotID() const;
  void verify() const;
  uint64_t size() const { return Snapshot.getSize(); }
  void read(uint64_t At, std::span<char> Out);
  std::string header(uint64_t At, uint64_t Length, Budget &B);
};
std::string hashRange(File &Input, uint64_t Offset, uint64_t Length, Budget &B,
                      llvm::raw_ostream *Copy = nullptr);
struct BlockLayout {
  unsigned Elements, Bytes;
};
struct Tensor {
  std::string FileId, Type;
  std::vector<uint64_t> Shape;
  uint64_t Offset = 0, Elements = 0;
  std::optional<uint64_t> Bytes;
  std::optional<BlockLayout> Packing;
  bool HasOffset = true;
};
struct Package {
  explicit Package(fs::path Root, std::string Id, Budget &B)
      : Root(std::move(Root)), Id(std::move(Id)), B(B) {}
  fs::path Root;
  std::string Id;
  Budget &B;
  std::map<std::string, std::unique_ptr<File>> Files;
  std::map<std::string, Tensor> Tensors;
  std::map<std::string, Json> Templates;
  std::map<std::string, std::string> Fingerprints;
  std::map<std::string, std::pair<std::string, std::string>> Variants;
  std::set<std::string> Architectures;
  Json Artifacts = Json::array(), Findings = Json::array(),
       Gaps = Json::array();
  Json Metadata = Json::array(), Graphs = Json::array(),
       Layers = Json::object(), ProjectionLayouts = Json::array();
  size_t FindingCount = 0, ReviewCount = 0, GapCount = 0, MetadataCount = 0;
  uint64_t WeightBytesRead = 0;
  File &file(const std::string &Name);
  std::string snapshotID() const;
  void addTensor(std::string Name, Tensor T);
  void finding(std::string Rule, std::string Source, std::string Severity,
               std::string Category, std::string Confidence,
               std::string Message, Json Evidence);
  void gap(std::string Source, std::string Reason,
           Json Evidence = Json::object());
  void verify() const;
};
void readGGUF(Package &P, const std::string &Name);
void readSafetensors(Package &P, const std::string &Name);
void readONNX(Package &P, const std::string &Name);
void inspectTemplate(Package &P, const std::string &FileId,
                     const std::string &Field, std::string_view Text,
                     std::optional<uint64_t> Offset = {},
                     std::string Variant = "default");
std::vector<std::unique_ptr<Package>> readInputs(const fs::path &Input,
                                                 Budget &B);
Json projectionLayouts(const Package &P);
Json compare(Package &Candidate, Package &Base);
} // namespace neverd::model
#endif // NEVERD_LIB_MODEL_MODELINTERNAL_H
