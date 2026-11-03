//===- Artifacts.cpp - Provider artifact verification
//----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify bounded artifacts before a process result can become successful.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Artifacts.h"

#include "neverd/plugin/Config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <random>
#ifdef _WIN32
#define NOMINMAX
// bcrypt.h requires the Windows SDK types; keep this include order.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace neverd::plugin {
bool verifyArtifacts(const std::filesystem::path &Directory, const Json &Result,
                     std::string &Error) {
  namespace fs = std::filesystem;
  if (!Result.contains("artifacts"))
    return true;
  std::error_code EC;
  const auto Root = fs::canonical(Directory, EC);
  if (EC) {
    Error = EC.message();
    return false;
  }
  for (const auto &Artifact : Result["artifacts"]) {
    const auto Name = Artifact["path"].get<std::string>();
    const auto Relative = fs::u8path(Name);
    auto Base = Relative.filename().string();
    std::transform(Base.begin(), Base.end(), Base.begin(),
                   [](unsigned char C) { return char(std::tolower(C)); });
    if (Relative.is_absolute() || Name.find(':') != std::string::npos ||
        Base == "receipt.json" ||
        std::find(Relative.begin(), Relative.end(), fs::path("..")) !=
            Relative.end()) {
      Error = "Artifact path escapes its task or names the host receipt";
      return false;
    }
    const auto Path = fs::canonical(Root / Relative, EC);
    if (EC || std::mismatch(Root.begin(), Root.end(), Path.begin(), Path.end())
                      .first != Root.end()) {
      Error = "Artifact is missing or outside its task directory";
      return false;
    }
    const auto Size = fs::file_size(Path, EC);
    if (EC || Size > limits::kMaxArtifactBytes) {
      Error = "Artifact exceeds its size limit or cannot be read";
      return false;
    }
    std::string Bytes(static_cast<size_t>(Size), '\0');
    std::ifstream File(Path, std::ios::binary);
    if (!File.read(Bytes.data(), Bytes.size()) ||
        File.peek() != std::char_traits<char>::eof()) {
      Error = "Artifact changed while reading";
      return false;
    }
    std::array<unsigned char, 32> Digest{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
      Error = "Cannot initialize SHA-256";
      return false;
    }
    const auto Status = BCryptHash(
        Algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(Bytes.data()),
        ULONG(Bytes.size()), Digest.data(), ULONG(Digest.size()));
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    if (Status < 0) {
      Error = "Cannot hash artifact";
      return false;
    }
#elif defined(__APPLE__)
    CC_SHA256(Bytes.data(), CC_LONG(Bytes.size()), Digest.data());
#else
    unsigned int Written = 0;
    if (!EVP_Digest(Bytes.data(), Bytes.size(), Digest.data(), &Written,
                    EVP_sha256(), nullptr)) {
      Error = "Cannot hash artifact";
      return false;
    }
#endif
    constexpr char Hex[] = "0123456789abcdef";
    std::string Text;
    for (auto Byte : Digest) {
      Text += Hex[Byte >> 4];
      Text += Hex[Byte & 15];
    }
    if (Artifact["sha256"] != Text) {
      Error = "Artifact SHA-256 does not match its result";
      return false;
    }
  }
  Error.clear();
  return true;
}

bool writeReceipt(const std::filesystem::path &Directory, const Json &Record,
                  std::string &Error) {
  return writeJsonFile(Directory / "receipt.json", Record, Error);
}

bool writeJsonFile(const std::filesystem::path &Destination, const Json &Record,
                   std::string &Error) {
  std::random_device Random;
  const auto Directory = Destination.parent_path();
  const auto Temporary = Directory / (".receipt-" + std::to_string(Random()) +
                                      std::to_string(Random()) + ".tmp");
  // stderr is an OS byte stream, unlike the validated UTF-8 protocol frames.
  const auto Bytes =
      Record.dump(2, ' ', false, Json::error_handler_t::replace) + '\n';
#ifdef _WIN32
  HANDLE File = CreateFileW(Temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (File == INVALID_HANDLE_VALUE) {
    Error = "Cannot create task receipt";
    return false;
  }
  DWORD Written = 0;
  const bool Saved =
      WriteFile(File, Bytes.data(), DWORD(Bytes.size()), &Written, nullptr) &&
      Written == Bytes.size() && FlushFileBuffers(File);
  const bool Closed = CloseHandle(File);
  if (!Saved || !Closed) {
    Error = "Cannot write task receipt";
    return false;
  }
  if (!MoveFileExW(Temporary.c_str(), Destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    Error = "Cannot commit task receipt (Windows error " +
            std::to_string(GetLastError()) + ")";
    return false;
  }
#else
  const int File =
      ::open(Temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (File < 0) {
    Error = "Cannot create task receipt";
    return false;
  }
  size_t Written = 0;
  while (Written < Bytes.size()) {
    const auto N =
        ::write(File, Bytes.data() + Written, Bytes.size() - Written);
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      break;
    Written += N;
  }
  const bool Saved = Written == Bytes.size() && ::fsync(File) == 0;
  const bool Closed = ::close(File) == 0;
  if (!Saved || !Closed) {
    Error = "Cannot write task receipt";
    return false;
  }
  std::error_code EC;
  std::filesystem::rename(Temporary, Destination, EC);
  if (EC) {
    Error = "Cannot commit task receipt: " + EC.message();
    return false;
  }
#endif
  Error.clear();
  return true;
}
} // namespace neverd::plugin
