//===- NeverDCAPISigs.cpp - C API: FLIRT signatures -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// FLIRT signature loading, matching, and result queries.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/Support/JSON.h"

using namespace neverd;
using namespace neverd::sdk;

int neverd_apply_signatures(neverd_session_t Sess, const char *SigDir) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigDir)
    return -1;
  S->clearError();

  auto Err = S->SigDB.loadDirectory(SigDir);
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  std::vector<uint64_t> Entries;
  Entries.reserve(S->Functions.size());
  for (const auto &F : S->Functions)
    Entries.push_back(F.Entry);

  S->SigDB.apply(S->Img, Entries);

  auto NameMap = S->SigDB.buildNameMap();
  for (auto &F : S->Functions) {
    auto It = NameMap.find(F.Entry);
    if (It != NameMap.end() && !It->second.empty()) {
      if (S->Renames.find(F.Entry) == S->Renames.end())
        F.Name = It->second;
    }
  }

  return static_cast<int>(S->SigDB.matches().size());
}

int neverd_apply_signature_file(neverd_session_t Sess, const char *SigPath) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigPath)
    return -1;
  S->clearError();

  auto Err = S->SigDB.loadFile(SigPath);
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  std::vector<uint64_t> Entries;
  Entries.reserve(S->Functions.size());
  for (const auto &F : S->Functions)
    Entries.push_back(F.Entry);

  S->SigDB.apply(S->Img, Entries);

  auto NameMap = S->SigDB.buildNameMap();
  for (auto &F : S->Functions) {
    auto It = NameMap.find(F.Entry);
    if (It != NameMap.end() && !It->second.empty()) {
      if (S->Renames.find(F.Entry) == S->Renames.end())
        F.Name = It->second;
    }
  }

  return static_cast<int>(S->SigDB.matches().size());
}

int neverd_auto_apply_signatures(neverd_session_t Sess,
                                 const char *SigBaseDir) {
  auto *S = static_cast<Session *>(Sess);
  if (!S || !S->Loaded || !SigBaseDir)
    return -1;
  S->clearError();

  std::string Fmt;
  switch (S->Img.Format) {
  case BinaryFormat::COFF:
    Fmt = "pe";
    break;
  case BinaryFormat::ELF:
    Fmt = "elf";
    break;
  case BinaryFormat::MachO:
    Fmt = "macho";
    break;
  default:
    return 0;
  }

  std::string ArchDir;
  switch (S->Img.Arch) {
  case Arch::X64:
  case Arch::X86:
    ArchDir = "x86";
    break;
  case Arch::AArch64:
  case Arch::ARM:
    ArchDir = "arm";
    break;
  default:
    return 0;
  }

  std::string Bits = S->Img.is64Bit() ? "64" : "32";

  std::filesystem::path SigPath =
      std::filesystem::path(SigBaseDir) / Fmt / ArchDir / Bits;

  if (!std::filesystem::exists(SigPath))
    return 0;

  auto Err = S->SigDB.loadDirectory(SigPath);
  if (Err) {
    S->setError(llvm::toString(std::move(Err)));
    return -1;
  }

  std::vector<uint64_t> Entries;
  Entries.reserve(S->Functions.size());
  for (const auto &F : S->Functions)
    Entries.push_back(F.Entry);

  S->SigDB.apply(S->Img, Entries);

  auto NameMap = S->SigDB.buildNameMap();
  for (auto &F : S->Functions) {
    auto It = NameMap.find(F.Entry);
    if (It != NameMap.end() && !It->second.empty()) {
      if (S->Renames.find(F.Entry) == S->Renames.end())
        F.Name = It->second;
    }
  }

  return static_cast<int>(S->SigDB.matches().size());
}

// ===--------------------------------------------------------------------===//
// Signature generation utilities
// ===--------------------------------------------------------------------===//

unsigned short neverd_sig_compute_crc16(const unsigned char *Data, int Length) {
  if (!Data || Length <= 0)
    return 0;
  return sigs::SignatureMatcher::computeCRC16(Data,
                                              static_cast<size_t>(Length));
}

int neverd_sig_match_count(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return 0;
  return static_cast<int>(S->SigDB.matches().size());
}

const char *neverd_sig_matches_json(neverd_session_t Sess) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &M : S->SigDB.matches()) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(M.Address);
    Obj["name"] = M.Name;
    Obj["library"] = M.LibraryName;
    Obj["func_len"] = static_cast<int64_t>(M.FuncLen);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}
