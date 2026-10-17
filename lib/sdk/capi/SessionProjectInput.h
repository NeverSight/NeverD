//===- SessionProjectInput.h - Shared private input observations -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file One input identity for sidecar recording and public information.
#ifndef NEVERD_SDK_CAPI_SESSIONPROJECTINPUT_H
#define NEVERD_SDK_CAPI_SESSIONPROJECTINPUT_H
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/loader/InputDigest.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd::sdk {
/// The SHA-256 of the input as loaded, in lowercase hexadecimal: the one the
/// loader took while it read the input, else one hash of the file, kept.
/// Empty when the file cannot be read.
inline std::string inputSha256(Session &S) {
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

/// The loaded input as a project's project.json names it.
inline llvm::json::Object projectInput(Session &S) {
  std::error_code Error;
  const auto Size = std::filesystem::file_size(S.FilePath, Error);
  return llvm::json::Object{
      {"name", jsonSafeText(pathToUTF8(S.FilePath.filename()))},
      {"size", Error ? 0 : static_cast<int64_t>(Size)},
      {"sha256", inputSha256(S)}};
}

/// The input the project's project.json names, if it names one.
inline std::optional<llvm::json::Object> recordedInput(const Session &S) {
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(
      project_files::path(S.FilePath, project_files::File::Manifest)));
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

} // namespace neverd::sdk
#endif
