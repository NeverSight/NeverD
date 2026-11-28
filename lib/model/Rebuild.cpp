//===- Rebuild.cpp - Verified out-of-place GGUF reconstruction ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rewrite only validated GGUF metadata, offsets and unowned regions.
//===----------------------------------------------------------------------===//

#include "GGUFLayout.h"
#include "ModelInternal.h"

#include "neverd/support/FilePath.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace neverd::model {
namespace {
struct TemplatePolicy {
  std::string Action, Key, Text;
};

TemplatePolicy policy(const char *Settings, Budget &B) {
  std::string_view Text = Settings ? Settings : "{}";
  if (Text.size() > limits::kTemplateBytes)
    throw Error("GGUF rebuild options exceed byte limit");
  auto J = parseObject(Text, B);
  for (auto &[K, V] : J.items())
    if (K != "template_policy" && K != "template_key" && K != "template_text")
      throw Error("unknown GGUF rebuild option");
  TemplatePolicy P{
      J.at("template_policy").get<std::string>(),
      J.value("template_key", std::string("tokenizer.chat_template")),
      J.value("template_text", std::string())};
  if (P.Action != "keep" && P.Action != "remove" && P.Action != "replace")
    throw Error("explicit template policy must be keep, remove or replace");
  if ((P.Action == "replace") != J.contains("template_text"))
    throw Error("template_text is required only for replacement");
  if (P.Action == "keep" && J.contains("template_key"))
    throw Error("keep preserves all template fields");
  validName(P.Key);
  if (P.Key != "tokenizer.chat_template" &&
      !P.Key.starts_with("tokenizer.chat_template."))
    throw Error("expected an embedded GGUF chat template key");
  return P;
}

uint64_t aligned(uint64_t V, uint64_t A) { return sum(V, (A - V % A) % A); }
void write64(llvm::raw_ostream &Out, uint64_t V, bool Big) {
  char Field[8];
  llvm::support::endian::write64(
      Field, V, Big ? llvm::endianness::big : llvm::endianness::little);
  Out.write(Field, 8);
}
void copyDirectory(File &F, uint64_t At, uint64_t End, llvm::raw_ostream &Out,
                   Budget &B) {
  std::array<char, limits::kChunk> Bytes;
  while (At < End) {
    B.tick();
    auto Chunk =
        std::span(Bytes).first(std::min<uint64_t>(Bytes.size(), End - At));
    F.read(At, Chunk);
    Out.write(Chunk.data(), Chunk.size());
    At += Chunk.size();
  }
}
void region(Json &Regions, uint64_t Start, uint64_t End) {
  if (Start < End)
    Regions.push_back({{"offset", Start}, {"length", End - Start}});
}

struct Payload {
  std::string Name;
  const Tensor *Source;
  uint64_t Relative = 0;
  std::string Hash;
};

std::string reconstruct(const fs::path &Source, const fs::path &Dest,
                        const TemplatePolicy &Policy, Options &O) {
  Budget B(O);
  const auto Name = pathToUTF8(Source.filename());
  Package Original(fs::canonical(fs::absolute(Source).parent_path()), Name, B);
  File &F = Original.file(Name);
  if (F.size() > limits::kRebuildBytes)
    throw Error("GGUF reconstruction byte budget exceeded");
  GGUFLayout Layout;
  readGGUF(Original, Name, &Layout);
  if (Original.GapCount || isNumberedGGUF(Name) ||
      !Original.Artifacts.front().at("split").empty())
    throw Error("reconstruction requires a fully inventoried standalone GGUF");
  auto Selected =
      std::find_if(Layout.Metadata.begin(), Layout.Metadata.end(),
                   [&](const auto &M) { return M.Key == Policy.Key; });
  if (Policy.Action != "keep" && Selected == Layout.Metadata.end())
    throw Error("requested embedded template does not exist");

  std::vector<Payload> Payloads;
  for (auto &[Id, T] : Original.Tensors)
    Payloads.push_back({Id, &T, 0, {}});
  llvm::sort(Payloads, [](const auto &L, const auto &R) {
    return L.Source->Offset < R.Source->Offset;
  });
  std::map<std::string, uint64_t> Relative;
  uint64_t Size = 0;
  for (auto &P : Payloads) {
    P.Relative = aligned(Size, Layout.Alignment);
    Relative.emplace(P.Name, P.Relative);
    Size = sum(P.Relative, *P.Source->Bytes);
  }
  const auto Root = fs::canonical(fs::absolute(Dest).parent_path());
  const auto Output = Root / Dest.filename();
  if (fs::symlink_status(Output).type() != fs::file_type::not_found)
    throw Error("GGUF output must be a new file");
  int FD;
  llvm::SmallString<256> TempName;
  if (llvm::sys::fs::createUniqueFile(pathToUTF8(Root / ".neverd-%%%%%%.tmp"),
                                      FD, TempName))
    throw Error("cannot create private output in destination directory");
  llvm::scope_exit Cleanup([&] { llvm::sys::fs::remove(TempName); });
  llvm::raw_fd_ostream Out(FD, true);
  bool Closed = false;
  llvm::scope_exit CheckStream([&] {
    if (!Closed)
      Out.close();
    Out.clear_error();
  });
  Json Removed = Json::array(), Padding = Json::array(),
       Patches = Json::array(), Rows = Json::array();
  copyDirectory(F, 0, 16, Out, B);
  write64(Out, Layout.Metadata.size() - (Policy.Action == "remove"),
          Layout.BigEndian);
  if (Policy.Action == "remove")
    Patches.push_back({{"field", "metadata_count"},
                       {"input_offset", 16},
                       {"output_offset", 16},
                       {"length", 8}});
  uint64_t Cursor = 24;
  for (const auto &M : Layout.Metadata) {
    if (M.Key != Policy.Key || Policy.Action == "keep")
      continue;
    copyDirectory(F, Cursor, M.Begin, Out, B);
    const uint64_t Begin = Out.tell();
    if (Policy.Action == "replace") {
      copyDirectory(F, M.Begin, M.Value, Out, B);
      write64(Out, Policy.Text.size(), Layout.BigEndian);
      Out.write(Policy.Text.data(), Policy.Text.size());
    }
    Patches.push_back({{"field", M.Key},
                       {"input_offset", M.Begin},
                       {"input_length", M.End - M.Begin},
                       {"output_offset", Begin},
                       {"output_length", Out.tell() - Begin},
                       {"old_encoded_value_sha256", M.ValueHash}});
    Cursor = M.End;
  }
  for (const auto &R : Layout.Records) {
    copyDirectory(F, Cursor, R.OffsetField, Out, B);
    const auto New = Relative.at(R.Name);
    if (New != Original.Tensors.at(R.Name).Offset - Layout.DataStart)
      Patches.push_back({{"field", "tensor_offset"},
                         {"tensor", R.Name},
                         {"input_offset", R.OffsetField},
                         {"output_offset", Out.tell()},
                         {"length", 8}});
    write64(Out, New, Layout.BigEndian);
    Cursor = R.OffsetField + 8;
  }
  copyDirectory(F, Cursor, Layout.DirectoryEnd, Out, B);
  const uint64_t DirectoryEnd = Out.tell();
  const uint64_t Start =
      Payloads.empty() ? DirectoryEnd : aligned(DirectoryEnd, Layout.Alignment);
  if (sum(Start, Size) > limits::kRebuildBytes)
    throw Error("rebuilt GGUF exceeds byte budget");
  Cursor = Layout.DirectoryEnd;
  for (auto &P : Payloads) {
    const auto At = Start + P.Relative;
    region(Removed, Cursor, P.Source->Offset);
    region(Padding, Out.tell(), At);
    Out.write_zeros(At - Out.tell());
    P.Hash = hashRange(F, P.Source->Offset, *P.Source->Bytes, B, &Out);
    if (Out.has_error())
      throw Error("GGUF output write failed");
    Cursor = P.Source->Offset + *P.Source->Bytes;
  }
  region(Removed, Cursor, F.size());
  Out.close();
  Closed = true;
  if (Out.has_error())
    throw Error("GGUF output close failed");
  const auto Temp = pathToUTF8(utf8Path(TempName.str().str()).filename());
  Package Result(Root, Temp, B);
  GGUFLayout Verified;
  readGGUF(Result, Temp, &Verified);
  if (Result.GapCount || Result.Tensors.size() != Original.Tensors.size() ||
      Verified.BigEndian != Layout.BigEndian ||
      Verified.Alignment != Layout.Alignment ||
      Verified.Metadata.size() !=
          Layout.Metadata.size() - (Policy.Action == "remove"))
    throw Error("reparsed GGUF inventory mismatch");
  size_t Index = 0;
  for (const auto &M : Layout.Metadata) {
    const bool Changed = M.Key == Policy.Key && Policy.Action != "keep";
    if (Changed && Policy.Action == "remove")
      continue;
    const auto &V = Verified.Metadata.at(Index++);
    if (M.Key != V.Key || (!Changed && M.ValueHash != V.ValueHash))
      throw Error("reparsed GGUF metadata mismatch");
    if (Changed) {
      if (Result.file(Temp).header(V.Value + 8, V.End - V.Value - 8, B) !=
          Policy.Text)
        throw Error("reparsed template differs from replacement");
      Patches.front()["new_encoded_value_sha256"] = V.ValueHash;
    }
  }
  File &Reopened = Result.file(Temp);
  for (const auto &P : Payloads) {
    const auto I = Result.Tensors.find(P.Name);
    if (I == Result.Tensors.end() || I->second.Type != P.Source->Type ||
        I->second.Shape != P.Source->Shape ||
        I->second.Bytes != P.Source->Bytes ||
        I->second.Offset != Start + P.Relative ||
        hashRange(Reopened, I->second.Offset, *P.Source->Bytes, B) != P.Hash)
      throw Error("reparsed GGUF tensor verification failed");
    Rows.push_back({{"name", P.Name},
                    {"input_offset", P.Source->Offset},
                    {"output_offset", I->second.Offset},
                    {"length", *P.Source->Bytes},
                    {"sha256", P.Hash}});
  }
  auto InputHash = hashRange(F, 0, F.size(), B);
  auto OutputHash = hashRange(Reopened, 0, Reopened.size(), B);
  auto Report = encodeReport({{"schema", "neverd.gguf-rebuild.v1"},
                              {"status", "verified"},
                              {"exit_code", 0},
                              {"input_sha256", InputHash},
                              {"output_sha256", OutputHash},
                              {"input_bytes", F.size()},
                              {"output_bytes", Reopened.size()},
                              {"template_policy", Policy.Action},
                              {"changed_fields", std::move(Patches)},
                              {"input_unreferenced_ranges", std::move(Removed)},
                              {"output_zero_padding", std::move(Padding)},
                              {"tensors", std::move(Rows)},
                              {"tensor_count", Payloads.size()},
                              {"behavior_evaluated", false}});
  B.tick();
  Original.verify();
  Result.verify();
  std::error_code EC;
  fs::create_hard_link(utf8Path(TempName.str().str()), Output, EC);
  if (EC)
    throw Error("cannot atomically publish new GGUF without overwriting");
  return Report;
}
} // namespace

std::string rebuildGGUF(const char *Input, const char *Output,
                        const char *Settings, std::function<bool()> Cancel) {
  Json Failure = {{"schema", "neverd.gguf-rebuild.v1"},
                  {"exit_code", 1},
                  {"status", "failed"}};
  try {
    if (!Input || !*Input || !Output || !*Output)
      throw Error("GGUF input and new output paths are required");
    Options O;
    O.Cancel = std::move(Cancel);
    Budget Admission(O);
    return reconstruct(utf8Path(Input), utf8Path(Output),
                       policy(Settings, Admission), O);
  } catch (const Stop &E) {
    Failure["status"] =
        std::string_view(E.what()) == "cancelled" ? "cancelled" : "partial";
    Failure["error"] = E.what();
  } catch (const Error &E) {
    Failure["error"] = E.what();
  } catch (const fs::filesystem_error &) {
    Failure["error"] = "GGUF filesystem operation failed";
  } catch (const Json::exception &) {
    Failure["error"] = "invalid GGUF template policy options";
  }
  return Failure.dump();
}
} // namespace neverd::model
