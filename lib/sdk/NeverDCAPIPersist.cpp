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
    if (!std::isfinite(*Num) || *Num < 0.0 ||
        *Num >= 18446744073709551616.0 || std::trunc(*Num) != *Num)
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
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return 1;
  }
  auto Path = annotationPath(S);
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC) {
    S->setError("cannot write annotations: " + EC.message());
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
  OS << llvm::json::Value(std::move(Arr));
  return 0;
}

int neverd_annotations_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return 1;
  auto Path = annotationPath(S);
  std::ifstream In(Path);
  if (!In.is_open())
    return 0;
  std::string Content((std::istreambuf_iterator<char>(In)),
                      std::istreambuf_iterator<char>());
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
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
      S->Renames[F.Entry] = NewName;
      F.Name = NewName;
      neverd_renames_save(Sess);
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
  if (!S->Loaded)
    return -1;
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
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC) {
    S->setError("cannot write renames: " + EC.message());
    return -1;
  }
  OS << llvm::json::Value(std::move(Arr));
  return 0;
}

int neverd_renames_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return -1;
  auto Path = S->FilePath.string() + ".neverd-renames.json";
  std::ifstream In(Path);
  if (!In.is_open())
    return 0;
  std::string Content((std::istreambuf_iterator<char>(In)),
                      std::istreambuf_iterator<char>());
  auto Parsed = llvm::json::parse(Content);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return -1;
  }
  auto *Arr = Parsed->getAsArray();
  if (!Arr)
    return -1;
  S->Renames.clear();
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
