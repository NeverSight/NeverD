//===- SessionSidecar.cpp - Atomic private Session sidecar output ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Shared atomic write owner for Session metadata adapters.
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/loader/MachO/DyldCacheLoad.h"
#include "neverd/support/AtomicOutput.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
using namespace neverd::sdk;

std::filesystem::path Session::projectInputPath() const {
  if (!Img.MachOSharedCache)
    return FilePath;
  auto Path = SanitizeSourcePath;
  Path += projectNamespaceSuffix();
  return Path;
}

std::string Session::projectNamespaceSuffix() const {
  if (!Img.MachOSharedCache)
    return {};
  const auto Digest =
      sha256(llvm::arrayRefFromStringRef(Img.MachOSharedCacheImagePath));
  return std::string(macho_loader::cache_load::kProjectSuffix) +
         llvm::toHex(Digest, true);
}

llvm::json::Object neverd::sdk::sharedCacheIdentity(const BinaryImage &Image) {
  using namespace macho_loader::cache_load;
  llvm::json::Array Files;
  for (const auto &Digest : Image.MachOSharedCacheFileSHA256)
    Files.push_back(llvm::toHex(Digest, true));
  return llvm::json::Object{
      {kImageKey, jsonSafeText(Image.MachOSharedCacheImagePath)},
      {kFilesKey, std::move(Files)},
      {kProjectionKey, true}};
}

bool neverd::sdk::matchesProjectInput(const llvm::json::Object &Left,
                                      const llvm::json::Object &Right) {
  const auto *L = Left.get(macho_loader::cache_load::kCacheKey);
  const auto *R = Right.get(macho_loader::cache_load::kCacheKey);
  return Left.getString("sha256") == Right.getString("sha256") &&
         ((!L && !R) || (L && R && *L == *R));
}

bool neverd::sdk::saveSidecar(Session *S, const std::filesystem::path &Path,
                              llvm::json::Value Values, llvm::StringRef Kind) {
  // A row's address in lowercase hexadecimal, as the workbench writes it:
  // one spelling, whichever tool wrote the file last.
  if (llvm::json::Array *Rows = Values.getAsArray())
    for (llvm::json::Value &Row : *Rows)
      if (llvm::json::Object *Object = Row.getAsObject())
        if (std::optional<llvm::StringRef> Address = Object->getString("addr"))
          (*Object)["addr"] = Address->lower();
  return saveSidecar(
      S, Path, [&](llvm::json::OStream &Output) { Output.value(Values); },
      Kind);
}

bool neverd::sdk::saveSidecar(
    Session *S, const std::filesystem::path &Path,
    llvm::function_ref<void(llvm::json::OStream &)> Emit,
    llvm::StringRef Kind) {
  // The project's files live in its folder beside the input.
  if (std::error_code Folder = project_files::prepare(S->projectInputPath())) {
    S->setError("cannot create the project folder: " + Folder.message());
    return false;
  }
  const auto UTF8Path = pathToUTF8(Path);
  auto Temporary = llvm::sys::fs::TempFile::create(
      UTF8Path + ".tmp-%%%%%%",
      llvm::sys::fs::owner_read | llvm::sys::fs::owner_write);
  if (!Temporary) {
    S->setError("cannot create " + Kind.str() +
                " temporary file: " + llvm::toString(Temporary.takeError()));
    return false;
  }
  std::error_code WriteError;
  {
    // TempFile owns the descriptor. Clear a handled stream error before its
    // destructor so ENOSPC/EFBIG becomes a C ABI error rather than LLVM abort.
    llvm::raw_fd_ostream OS(Temporary->FD, false);
    // Two-space indentation, sorted keys and a final newline, byte for byte
    // as the workbench writes the same files (the worker's sidecarText), so
    // a project diffs and merges line by line under version control.
    llvm::json::OStream Output(OS, 2);
    Emit(Output);
    OS << '\n';
    OS.flush();
    WriteError = OS.error();
    OS.clear_error();
  }
  if (WriteError) {
    llvm::consumeError(
        support::atomic_output::discardTemporaryOutput(*Temporary));
    S->setError("cannot write " + Kind.str() + ": " + WriteError.message());
    return false;
  }
  if (auto Error = support::atomic_output::closeAndCommitTemporaryOutput(
          *Temporary, UTF8Path)) {
    S->setError("cannot save " + Kind.str() + ": " +
                llvm::toString(std::move(Error)));
    return false;
  }
  // A file the project now has describes this build of the input.
  return Path == project_files::path(S->projectInputPath(),
                                     project_files::File::Manifest) ||
         recordProjectInput(S);
}

std::string neverd::sdk::inputSha256(Session &S) {
  if (!S.Img.InputFileSHA256) {
    if (S.MemoryInputBytes)
      return {};
    auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(S.FilePath));
    if (!Buffer)
      return {};
    S.Img.InputFileSHA256 = sha256(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>((*Buffer)->getBufferStart()),
        (*Buffer)->getBufferSize()));
  }
  return llvm::toHex(*S.Img.InputFileSHA256, /*LowerCase=*/true);
}

llvm::json::Object neverd::sdk::projectInput(Session &S) {
  std::error_code Error;
  const auto Size = std::filesystem::file_size(S.FilePath, Error);
  llvm::json::Object Input{
      {"name", jsonSafeText(pathToUTF8(S.FilePath.filename()))},
      {"size", Error ? 0 : static_cast<int64_t>(Size)},
      {"sha256", inputSha256(S)}};
  if (S.Img.MachOSharedCache)
    Input[macho_loader::cache_load::kCacheKey] = sharedCacheIdentity(S.Img);
  return Input;
}

std::optional<llvm::json::Object> neverd::sdk::recordedInput(const Session &S) {
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(project_files::path(
      S.projectInputPath(), project_files::File::Manifest)));
  if (!Buffer)
    return std::nullopt;
  llvm::Expected<llvm::json::Value> Manifest =
      llvm::json::parse((*Buffer)->getBuffer());
  if (!Manifest) {
    llvm::consumeError(Manifest.takeError());
    return std::nullopt;
  }
  const llvm::json::Object *Object = Manifest->getAsObject();
  const llvm::json::Object *Input =
      Object ? Object->getObject("input") : nullptr;
  if (!Input)
    return std::nullopt;
  return *Input;
}

bool neverd::sdk::recordProjectInput(Session *S) {
  if (S->MemoryInputBytes)
    return true;
  llvm::json::Object Input = projectInput(*S);
  if (Input.getString("sha256").value_or("").empty())
    return true;
  if (auto Recorded = recordedInput(*S);
      Recorded && matchesProjectInput(*Recorded, Input))
    return true;
  return saveSidecar(
      S,
      project_files::path(S->projectInputPath(), project_files::File::Manifest),
      llvm::json::Object{{"neverd_project", 1}, {"input", std::move(Input)}},
      "project");
}
