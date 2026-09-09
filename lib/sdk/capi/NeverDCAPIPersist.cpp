//===- NeverDCAPIPersist.cpp - C API: annotations and renames -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-address annotations and persistent function renaming.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/support/AtomicOutput.h"
#include "neverd/support/ProjectWriteLock.h"

#include "llvm/Support/JSON.h"

#include <cmath>
#include <fstream>
#include <optional>

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Annotations
// ===--------------------------------------------------------------------===//

static std::string annotationPath(const Session *S) {
  return S->FilePath.string() + ".neverd-annotations.json";
}

static bool saveSidecar(Session *S, llvm::StringRef Path,
                        llvm::json::Array Values, llvm::StringRef Kind) {
  auto Temporary = llvm::sys::fs::TempFile::create(
      (Path + ".tmp-%%%%%%").str(),
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
    OS << llvm::json::Value(std::move(Values));
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
          *Temporary, Path)) {
    S->setError("cannot save " + Kind.str() + ": " +
                llvm::toString(std::move(Error)));
    return false;
  }
  return true;
}

static std::optional<va_t>
parsePersistedAddress(const llvm::json::Value &Value) {
  if (auto Str = Value.getAsString()) {
    llvm::StringRef Ref(*Str);
    if (Ref.empty() || Ref.front() == '-')
      return std::nullopt;
    if (Ref.consume_front("0x") || Ref.consume_front("0X")) {
      if (Ref.empty())
        return std::nullopt;
    }
    va_t Addr = 0;
    if (Ref.getAsInteger(16, Addr))
      return std::nullopt;
    return Addr;
  }

  if (auto Num = Value.getAsNumber()) {
    if (!std::isfinite(*Num) || *Num < 0.0 || *Num >= 18446744073709551616.0 ||
        std::trunc(*Num) != *Num)
      return std::nullopt;
    return static_cast<va_t>(*Num);
  }
  return std::nullopt;
}

void neverd_annotation_set(neverd_session_t Sess, neverd_va_t Addr,
                           const char *Text) {
  auto *S = toSession(Sess);
  if (Text && Text[0])
    S->Annotations[Addr] = Text;
  else
    S->Annotations.erase(Addr);
}

void neverd_annotation_remove(neverd_session_t Sess, neverd_va_t Addr) {
  toSession(Sess)->Annotations.erase(Addr);
}

const char *neverd_annotation_get(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  auto It = S->Annotations.find(Addr);
  if (It == S->Annotations.end())
    return nullptr;
  return dupStr(It->second);
}

const char *neverd_annotations_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  llvm::json::Array Arr;
  for (const auto &[Addr, Text] : S->Annotations) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Addr);
    Obj["text"] = Text;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

int neverd_annotations_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return 1;
  }
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return 1;
  }
  llvm::json::Array Arr;
  for (const auto &[Addr, Text] : S->Annotations) {
    llvm::json::Object Obj;
    // Store the address as a hex string, mirroring neverd_annotations_json;
    // a JSON number is a double and would lose precision for VAs >= 2^53.
    Obj["addr"] = vaHex(Addr);
    Obj["text"] = Text;
    Arr.push_back(std::move(Obj));
  }
  return saveSidecar(S, annotationPath(S), std::move(Arr), "annotations") ? 0
                                                                          : 1;
}

int neverd_annotations_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded)
    return 1;
  auto Path = annotationPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect annotations: " + EC.message());
    return 1;
  }
  if (!Exists) {
    S->Annotations.clear();
    return 0;
  }
  std::ifstream In(Path);
  if (!In.is_open()) {
    S->setError("cannot read annotations");
    return 1;
  }
  std::string Content((std::istreambuf_iterator<char>(In)),
                      std::istreambuf_iterator<char>());
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("annotations sidecar is not valid JSON");
    return 1;
  }
  auto *Arr = Parsed->getAsArray();
  if (!Arr)
    return 1;
  S->Annotations.clear();
  for (const auto &Item : *Arr) {
    auto *Obj = Item.getAsObject();
    if (!Obj)
      continue;
    const llvm::json::Value *AddrVal = Obj->get("addr");
    auto Text = Obj->getString("text");
    if (!AddrVal || !Text)
      continue;
    std::optional<va_t> Addr = parsePersistedAddress(*AddrVal);
    if (Addr)
      S->Annotations[*Addr] = Text->str();
  }
  return 0;
}

// ===--------------------------------------------------------------------===//
// Symbol renaming
// ===--------------------------------------------------------------------===//

int neverd_rename_func(neverd_session_t Sess, const char *OldName,
                       const char *NewName) {
  auto *S = toSession(Sess);
  if (!S->Loaded || !OldName || !NewName)
    return -1;
  (void)neverd_func_count(Sess);

  for (auto &F : S->Functions) {
    if (F.Name == OldName) {
      const auto PreviousName = F.Name;
      const auto PreviousRenames = S->Renames;
      S->Renames[F.Entry] = NewName;
      F.Name = NewName;
      if (neverd_renames_save(Sess) != 0) {
        S->Renames = PreviousRenames;
        F.Name = PreviousName;
        return -1;
      }
      return 0;
    }
  }
  S->setError("function not found: " + std::string(OldName));
  return -1;
}

const char *neverd_renames_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  llvm::json::Array Arr;
  for (const auto &[Addr, NewName] : S->Renames) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Addr);
    auto It = S->OriginalNames.find(Addr);
    Obj["original"] = It != S->OriginalNames.end() ? It->second : "";
    Obj["renamed"] = NewName;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

int neverd_renames_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  auto Path = S->FilePath.string() + ".neverd-renames.json";
  llvm::json::Array Arr;
  for (const auto &[Addr, NewName] : S->Renames) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Addr);
    auto It = S->OriginalNames.find(Addr);
    Obj["original"] = It != S->OriginalNames.end() ? It->second : "";
    Obj["renamed"] = NewName;
    Arr.push_back(std::move(Obj));
  }
  return saveSidecar(S, Path, std::move(Arr), "renames") ? 0 : -1;
}

int neverd_renames_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->Loaded)
    return -1;
  auto Path = S->FilePath.string() + ".neverd-renames.json";
  auto Reset = [&] {
    for (auto &F : S->Functions) {
      if (S->Renames.find(F.Entry) == S->Renames.end())
        continue;
      if (auto Original = S->OriginalNames.find(F.Entry);
          Original != S->OriginalNames.end())
        F.Name = Original->second;
    }
    S->Renames.clear();
  };
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect renames: " + EC.message());
    return -1;
  }
  if (!Exists) {
    Reset();
    return 0;
  }
  std::ifstream In(Path);
  if (!In.is_open()) {
    S->setError("cannot read renames");
    return -1;
  }
  std::string Content((std::istreambuf_iterator<char>(In)),
                      std::istreambuf_iterator<char>());
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("renames sidecar is not valid JSON");
    return -1;
  }
  auto *Arr = Parsed->getAsArray();
  if (!Arr)
    return -1;
  Reset();
  for (const auto &V : *Arr) {
    auto *Obj = V.getAsObject();
    if (!Obj)
      continue;
    const llvm::json::Value *AddrVal = Obj->get("addr");
    auto Renamed = Obj->getString("renamed");
    if (!AddrVal || !Renamed)
      continue;
    std::optional<va_t> Addr = parsePersistedAddress(*AddrVal);
    if (!Addr)
      continue;
    S->Renames[*Addr] = Renamed->str();
    for (auto &F : S->Functions) {
      if (F.Entry == *Addr) {
        F.Name = Renamed->str();
        break;
      }
    }
  }
  return 0;
}
