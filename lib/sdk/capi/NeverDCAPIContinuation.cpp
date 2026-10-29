//===- NeverDCAPIContinuation.cpp - Explicit x64 state C API -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the shared state-source exporter without ordinary ABI inference.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIContinuation.h"

#include "SessionImpl.h"

#include "neverd/pipeline/X64ContinuationExport.h"

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

extern "C" const char *
neverd_x64_continuation_source_v1(neverd_session_t Sess,
                                  const char *RequestJSON, size_t RequestSize,
                                  int UseLLVM, const char **ManifestJSON) {
  if (ManifestJSON)
    *ManifestJSON = nullptr;
  auto *Session = neverd::sdk::toSession(Sess);
  if (!Session)
    return nullptr;
  const auto Fail = [&](llvm::StringRef Message) noexcept {
    try {
      Session->setError(Message.str());
    } catch (...) {
    }
  };
  try {
    Session->clearError();
    if (!Session->Loaded || !RequestJSON || !RequestSize ||
        RequestSize > neverd::limits::pipeline::kX64ContinuationRequestBytes ||
        (UseLLVM != 0 && UseLLVM != 1)) {
      Fail("x64 continuation export requires a loaded image, a bounded request "
           "and a valid C route");
      return nullptr;
    }
    auto Result = neverd::exportX64Continuation(
        Session->Img, llvm::StringRef(RequestJSON, RequestSize), bool(UseLLVM),
        Session->decompileDebug());
    if (!Result) {
      Fail(llvm::toString(Result.takeError()));
      return nullptr;
    }
    using Owned = std::unique_ptr<char, decltype(&std::free)>;
    Owned Source(neverd::sdk::dupStr(Result->Source), &std::free);
    Owned Manifest(ManifestJSON ? neverd::sdk::dupStr(Result->Manifest)
                                : nullptr,
                   &std::free);
    if (!Source || (ManifestJSON && !Manifest)) {
      Fail("x64 continuation source allocation failed");
      return nullptr;
    }
    if (ManifestJSON)
      *ManifestJSON = Manifest.release();
    return Source.release();
  } catch (const std::exception &Error) {
    Fail(Error.what());
  } catch (...) {
    Fail("x64 continuation export failed with an unexpected exception");
  }
  return nullptr;
}
