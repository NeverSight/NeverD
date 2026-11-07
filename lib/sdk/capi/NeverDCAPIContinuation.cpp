//===- NeverDCAPIContinuation.cpp - Explicit x86 state C API -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the shared state-source exporter without ordinary ABI inference.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIContinuation.h"

#include "SessionImpl.h"

#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/X64ContinuationExport.h"
#include "neverd/pipeline/X86LegacySourceExport.h"

#include "llvm/Support/Errc.h"

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

namespace {
template <typename Export>
const char *exportSource(neverd_session_t Sess, const char *RequestJSON,
                         size_t RequestSize, size_t RequestLimit,
                         const char **ManifestJSON, Export Emit) {
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
        RequestSize > RequestLimit) {
      Fail("state source export requires a loaded image and a bounded request");
      return nullptr;
    }
    auto Result = Emit(*Session, llvm::StringRef(RequestJSON, RequestSize));
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
      Fail("state source allocation failed");
      return nullptr;
    }
    if (ManifestJSON)
      *ManifestJSON = Manifest.release();
    return Source.release();
  } catch (const std::exception &Error) {
    Fail(Error.what());
  } catch (...) {
    Fail("state source export failed with an unexpected exception");
  }
  return nullptr;
}
} // namespace

extern "C" const char *
neverd_x64_continuation_source_v1(neverd_session_t Sess,
                                  const char *RequestJSON, size_t RequestSize,
                                  int UseLLVM, const char **ManifestJSON) {
  return exportSource(
      Sess, RequestJSON, RequestSize,
      neverd::limits::pipeline::kX64ContinuationRequestBytes, ManifestJSON,
      [UseLLVM](auto &Session, llvm::StringRef Text)
          -> llvm::Expected<neverd::X64ContinuationExport> {
        if (UseLLVM != 0 && UseLLVM != 1)
          return llvm::createStringError(
              llvm::errc::invalid_argument,
              "x64 continuation requires a valid C route");
        return neverd::exportX64Continuation(Session.Img, Text, bool(UseLLVM),
                                             Session.decompileDebug());
      });
}

extern "C" const char *neverd_x86_legacy_source_v1(neverd_session_t Sess,
                                                   const char *RequestJSON,
                                                   size_t RequestSize,
                                                   const char **ManifestJSON) {
  return exportSource(Sess, RequestJSON, RequestSize,
                      neverd::limits::pipeline::kX86LegacySourceRequestBytes,
                      ManifestJSON, [](auto &Session, llvm::StringRef Text) {
                        return neverd::exportX86LegacySource(Session.Img, Text);
                      });
}
