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

#include "LoadOptions.h"
#include "SessionImpl.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/PlatformEvidence.h"
#include "neverd/loader/LoadCandidate.h"
#include "neverd/loader/Raw/ISAIdentify.h"
#include "neverd/support/AtomicOutput.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectWriteLock.h"
#include "neverd/support/StringScan.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

// ===--------------------------------------------------------------------===//
// Annotations
// ===--------------------------------------------------------------------===//

static std::filesystem::path annotationPath(const Session *S) {
  auto Path = S->FilePath;
  Path += ".neverd-annotations.json";
  return Path;
}

static bool saveSidecar(Session *S, const std::filesystem::path &Path,
                        llvm::json::Value Values, llvm::StringRef Kind) {
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
    OS << Values;
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

  if (auto Integer = Value.getAsUINT64())
    return *Integer;

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
  if (!S->requireFileBacked())
    return 1;
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
    // Store the address as a hex string, mirroring neverd_annotations_json,
    // to preserve full address precision across JSON consumers.
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
  if (!S->requireFileBacked())
    return 1;
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
      const auto PreviousOrigin = F.Origin;
      const auto PreviousRenames = S->Renames;
      S->Renames[F.Entry] = NewName;
      S->forgetEmittedSources();
      F.Name = NewName;
      F.Origin = NameOrigin::User;
      if (neverd_renames_save(Sess) != 0) {
        S->Renames = PreviousRenames;
        F.Name = PreviousName;
        F.Origin = PreviousOrigin;
        return -1;
      }
      return 0;
    }
  }
  S->setError("function not found: " + std::string(OldName));
  return -1;
}

int neverd_rename_addr(neverd_session_t Sess, neverd_va_t Addr,
                       const char *Name) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  const llvm::StringRef NewName = Name ? Name : "";
  constexpr size_t MaxNameBytes = 4096;
  if (NewName.size() > MaxNameBytes ||
      llvm::any_of(NewName,
                   [](unsigned char C) { return C <= ' ' || C == 0x7f; })) {
    S->setError("a name has at most 4096 bytes and no spaces or control "
                "characters");
    return -1;
  }
  if (!S->Img.readVA(Addr, 1)) {
    S->setError(vaHex(Addr) + " is not in the image");
    return -1;
  }
  (void)neverd_func_count(Sess);
  const auto PreviousRenames = S->Renames;
  const auto PreviousOriginals = S->OriginalNames;
  if (NewName.empty()) {
    S->Renames.erase(Addr);
  } else {
    // What the address was called before the user named it.
    if (!S->OriginalNames.count(Addr))
      if (const Symbol *Sym = S->Img.findSymbolAt(Addr); Sym && !Sym->IsFunc)
        S->OriginalNames[Addr] = Sym->Name;
    S->Renames[Addr] = NewName.str();
  }
  S->forgetEmittedSources();
  // A function entry takes the name in the function list too.
  S->refreshFunctionNames();
  if (neverd_renames_save(Sess) != 0) {
    S->Renames = PreviousRenames;
    S->OriginalNames = PreviousOriginals;
    S->refreshFunctionNames();
    return -1;
  }
  return 0;
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
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  auto Path = S->FilePath;
  Path += ".neverd-renames.json";
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
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  auto Path = S->FilePath;
  Path += ".neverd-renames.json";
  auto Reset = [&] {
    for (auto &F : S->Functions) {
      if (S->Renames.find(F.Entry) == S->Renames.end())
        continue;
      if (auto Original = S->OriginalNames.find(F.Entry);
          Original != S->OriginalNames.end())
        F.Name = Original->second;
    }
    S->Renames.clear();
    S->forgetEmittedSources();
    S->refreshFunctionNames();
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
        F.Origin = NameOrigin::User;
        break;
      }
    }
  }
  return 0;
}

// ===--------------------------------------------------------------------===//
// Function edits
// ===--------------------------------------------------------------------===//

static std::filesystem::path functionsPath(const Session *S) {
  auto Path = S->FilePath;
  Path += ".neverd-functions.json";
  return Path;
}

static llvm::json::Array functionEditRows(const Session *S) {
  llvm::json::Array Rows;
  for (const auto &[Entry, Created] : S->FunctionEdits)
    Rows.push_back(llvm::json::Object{
        {"addr", vaHex(Entry)}, {"state", Created ? "created" : "deleted"}});
  return Rows;
}

int neverd_func_create(neverd_session_t Sess, neverd_va_t Entry) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  const Segment *Code = S->Img.getSegmentFor(Entry);
  if (!Code || !Code->isExecutable()) {
    S->setError(vaHex(Entry) + " is not in executable code");
    return -1;
  }
  // The user's last edit at an address decides over the image, the detector
  // and analysis, so a deleted function made again is a created one.
  if (!S->isDeletedFunction(Entry)) {
    (void)S->synchronizeFunctions();
    for (const FuncInfo &F : S->Functions)
      if (F.Entry == Entry) {
        S->setError("a function already starts at " + vaHex(Entry));
        return -1;
      }
  }
  S->FunctionEdits[Entry] = true;
  S->invalidatePipeline();
  return 0;
}

int neverd_func_delete(neverd_session_t Sess, neverd_va_t Entry) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  (void)S->synchronizeFunctions();
  if (llvm::none_of(S->Functions,
                    [&](const FuncInfo &F) { return F.Entry == Entry; })) {
    S->setError("no function starts at " + vaHex(Entry));
    return -1;
  }
  // Recorded for a created function too: the image, the detector or analysis
  // may find a function there as well.
  S->FunctionEdits[Entry] = false;
  S->invalidatePipeline();
  return 0;
}

const char *neverd_functions_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  return dupStr(jsonToString(llvm::json::Value(functionEditRows(S))));
}

int neverd_functions_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, functionsPath(S), functionEditRows(S), "function edits")
             ? 0
             : -1;
}

int neverd_functions_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  const auto Path = functionsPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect function edits: " + EC.message());
    return -1;
  }
  std::map<va_t, bool> Edits;
  if (Exists) {
    std::ifstream In(Path);
    if (!In.is_open()) {
      S->setError("cannot read function edits");
      return -1;
    }
    const std::string Content((std::istreambuf_iterator<char>(In)),
                              std::istreambuf_iterator<char>());
    auto Parsed = llvm::json::parse(Content);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("function edits sidecar is not valid JSON");
      return -1;
    }
    const auto *Rows = Parsed->getAsArray();
    if (!Rows) {
      S->setError("function edits sidecar is not an array");
      return -1;
    }
    for (const auto &Row : *Rows) {
      const auto *Object = Row.getAsObject();
      const llvm::json::Value *Address = Object ? Object->get("addr") : nullptr;
      const auto State = Object ? Object->getString("state") : std::nullopt;
      const auto Entry =
          Address ? parsePersistedAddress(*Address) : std::optional<va_t>();
      if (!Entry || !State || (*State != "created" && *State != "deleted")) {
        S->setError("function edits sidecar has an invalid row");
        return -1;
      }
      Edits[*Entry] = *State == "created";
    }
  }
  if (Edits == S->FunctionEdits)
    return 0;
  S->FunctionEdits = std::move(Edits);
  S->invalidatePipeline();
  return 0;
}

// ===--------------------------------------------------------------------===//
// Data items
// ===--------------------------------------------------------------------===//

namespace {
#define NEVERD_DATA_ITEM_KIND(Id, Spelling)                                    \
  constexpr llvm::StringLiteral Id(Spelling);
#include "neverd/DataNames.def"

/// The bytes of a value of the size \p Kind names (`qword`), or 0.
uint64_t sizedItemBytes(llvm::StringRef Kind) {
#define NEVERD_DATA_SIZE_NAME(SizeKeyword, Bytes, Prefix)                      \
  if (Kind == SizeKeyword)                                                     \
    return Bytes;
#include "neverd/DataNames.def"
  return 0;
}

std::filesystem::path itemsPath(const Session *S) {
  auto Path = S->FilePath;
  Path += ".neverd-items.json";
  return Path;
}

#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  constexpr llvm::StringLiteral k##Id##Base(Spelling);
#include "neverd/OperandFormats.def"

/// The operands of one instruction a format may name.
constexpr int MaxFormattedOperands = 8;

std::filesystem::path operandsPath(const Session *S) {
  auto Path = S->FilePath;
  Path += ".neverd-operands.json";
  return Path;
}

bool isOperandBase(llvm::StringRef Base) {
#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  if (Base == Spelling)                                                        \
    return true;
#include "neverd/OperandFormats.def"
  return false;
}

/// The format \p Object describes, or none with \p Error.
std::optional<Session::OperandFormat>
parseOperandFormat(const llvm::json::Object &Object, std::string &Error) {
  Session::OperandFormat Format;
  const auto Base = Object.getString("base");
  if (!Base || !isOperandBase(*Base)) {
    Error = "an operand's base is number, hex, decimal, binary, char or "
            "offset";
    return std::nullopt;
  }
  Format.Base = Base->str();
  for (const auto &[Key, Field] : {std::pair{"negate", &Format.Negate},
                                   std::pair{"invert", &Format.Invert}})
    if (const llvm::json::Value *Value = Object.get(Key)) {
      const auto Flag = Value->getAsBoolean();
      if (!Flag) {
        Error = std::string(Key) + " is true or false";
        return std::nullopt;
      }
      *Field = *Flag;
    }
  return Format;
}

llvm::json::Array operandFormatRows(const Session *S) {
  llvm::json::Array Rows;
  for (const auto &[Addr, Operands] : S->OperandFormats) {
    llvm::json::Array List;
    for (const auto &[Index, Format] : Operands)
      List.push_back(
          llvm::json::Object{{"operand", static_cast<int64_t>(Index)},
                             {"base", Format.Base},
                             {"negate", Format.Negate},
                             {"invert", Format.Invert}});
    Rows.push_back(llvm::json::Object{{"addr", vaHex(Addr)},
                                      {"operands", std::move(List)}});
  }
  return Rows;
}

/// The item \p Row describes at \p Addr, checked against the image; none
/// with \p Error.
std::optional<Session::DataItem> parseDataItem(const Session &S, va_t Addr,
                                               const llvm::json::Object &Row,
                                               std::string &Error) {
  Session::DataItem Item;
  const auto Kind = Row.getString("kind");
  if (!Kind) {
    Error = "a data item needs a kind";
    return std::nullopt;
  }
  Item.Kind = Kind->str();
  const auto Size = Row.getInteger("size");
  std::optional<strings::Encoding> Encoding;
  if (const uint64_t Bytes = sizedItemBytes(*Kind)) {
    if (Size && static_cast<uint64_t>(*Size) != Bytes) {
      Error = Item.Kind + " takes " + std::to_string(Bytes) + " bytes";
      return std::nullopt;
    }
    Item.Size = Bytes;
  } else if (*Kind == StringItemKind) {
    const auto Name = Row.getString("encoding");
    Encoding = Name ? strings::encodingNamed(*Name) : std::nullopt;
    if (!Encoding) {
      Error = "a string item needs a known encoding";
      return std::nullopt;
    }
    Item.Encoding = strings::encodingName(*Encoding).str();
    const unsigned Unit = strings::encodingUnitBytes(*Encoding);
    if (!Size || *Size < Unit || *Size % Unit) {
      Error = "a string item needs its size in whole units, its terminator "
              "included";
      return std::nullopt;
    }
    Item.Size = static_cast<uint64_t>(*Size);
  } else if (*Kind == UndefinedItemKind) {
    if (!Size || *Size < 1) {
      Error = "an undefined item needs its size";
      return std::nullopt;
    }
    Item.Size = static_cast<uint64_t>(*Size);
  } else if (*Kind == CodeItemKind) {
    // The SDK owns instruction validation for both new definitions and
    // persisted rows. Do not trust a size supplied by a frontend or sidecar.
    const Segment *Seg = S.Img.getSegmentFor(Addr);
    if (!Seg || !S.Img.isCodeAddress(Addr) || Addr < Seg->VA ||
        Addr - Seg->VA >= Seg->Data.size()) {
      Error = "code needs file-backed executable bytes";
      return std::nullopt;
    }
    const size_t Offset = static_cast<size_t>(Addr - Seg->VA);
    const size_t Available = static_cast<size_t>(
        std::min<uint64_t>(Seg->Data.size() - Offset, Seg->Size - Offset));
    Decoder Dec;
    if (!Dec.init(S.Img) || !Dec.selectMode(S.Img, Addr)) {
      Error = "unknown or conflicting native instruction mode";
      return std::nullopt;
    }
    DecodedInsn Insn{};
    const int Bytes = Dec.decodeOne(
        Seg->Data.data() + Offset, std::min<size_t>(Available, 16), Addr, Insn);
    if (Bytes <= 0 || !S.Img.isCodeRange(Addr, Bytes) ||
        (S.Img.Arch == Arch::ARM &&
         S.Img.instructionModeAt(Addr + Bytes - 1, Dec.currentMode()) !=
             Dec.currentMode())) {
      Error = "invalid or truncated instruction at " + vaHex(Addr);
      return std::nullopt;
    }
    if (Size && *Size != Bytes) {
      Error = "code size must match its decoded instruction";
      return std::nullopt;
    }
    Item.Size = static_cast<uint64_t>(Bytes);
  } else {
    Error = "unknown data item kind '" + Item.Kind + "'";
    return std::nullopt;
  }
  const Segment *Seg = S.Img.getSegmentFor(Addr);
  if (!Seg || Addr < Seg->VA || Item.Size > Seg->VA + Seg->Size - Addr) {
    Error = vaHex(Addr) + " to " + vaHex(Addr + Item.Size) +
            " is not in one segment";
    return std::nullopt;
  }
  if (Encoding) {
    // The string's last code unit is its terminator.
    const unsigned Unit = strings::encodingUnitBytes(*Encoding);
    const uint8_t *Last = S.Img.readVA(Addr + Item.Size - Unit, Unit);
    if (!Last || !std::all_of(Last, Last + Unit,
                              [](uint8_t Byte) { return Byte == 0; })) {
      Error = "the string at " + vaHex(Addr) + " does not end in a zero unit";
      return std::nullopt;
    }
  }
  return Item;
}

/// The address of an item in \p Items other than \p Except that shares a
/// byte with [\p Addr, \p Addr + \p Size).
std::optional<va_t>
overlappingItem(const std::map<va_t, Session::DataItem> &Items, va_t Addr,
                uint64_t Size, va_t Except) {
  auto It = Items.upper_bound(Addr);
  if (It != Items.begin()) {
    const auto Before = std::prev(It);
    if (Before->first != Except && Before->first + Before->second.Size > Addr)
      return Before->first;
  }
  for (; It != Items.end() && It->first < Addr + Size; ++It)
    if (It->first != Except)
      return It->first;
  return std::nullopt;
}

llvm::json::Array dataItemRows(const Session *S) {
  llvm::json::Array Rows;
  for (const auto &[Addr, Item] : S->DataItems) {
    llvm::json::Object Row{{"addr", vaHex(Addr)},
                           {"kind", Item.Kind},
                           {"size", static_cast<int64_t>(Item.Size)}};
    if (!Item.Encoding.empty())
      Row["encoding"] = Item.Encoding;
    Rows.push_back(std::move(Row));
  }
  return Rows;
}
} // namespace

int neverd_item_set(neverd_session_t Sess, neverd_va_t Addr,
                    const char *RowJson) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  auto Parsed = llvm::json::parse(RowJson ? RowJson : "");
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("a data item is a JSON object");
    return -1;
  }
  const auto *Row = Parsed->getAsObject();
  if (!Row) {
    S->setError("a data item is a JSON object");
    return -1;
  }
  std::string Error;
  auto Item = parseDataItem(*S, Addr, *Row, Error);
  if (!Item) {
    S->setError(Error);
    return -1;
  }
  // An item replaces the one at its address. Bytes the user undefined give
  // way to it and stay undefined around it; it shares no byte with any other
  // item.
  const va_t End = Addr + Item->Size;
  std::vector<va_t> Carved;
  std::vector<std::pair<va_t, uint64_t>> Pieces;
  auto It = S->DataItems.upper_bound(Addr);
  if (It != S->DataItems.begin() &&
      std::prev(It)->first + std::prev(It)->second.Size > Addr)
    --It;
  for (; It != S->DataItems.end() && It->first < End; ++It) {
    const auto &[At, Other] = *It;
    if (Other.Kind != UndefinedItemKind) {
      if (At == Addr)
        continue;
      S->setError(vaHex(Addr) + " overlaps the item at " + vaHex(At));
      return -1;
    }
    Carved.push_back(At);
    if (At < Addr)
      Pieces.emplace_back(At, Addr - At);
    if (At + Other.Size > End)
      Pieces.emplace_back(End, At + Other.Size - End);
  }
  for (const va_t At : Carved)
    S->DataItems.erase(At);
  for (const auto &[At, Size] : Pieces)
    S->DataItems[At] = {std::string(UndefinedItemKind), Size, {}};
  S->DataItems[Addr] = std::move(*Item);
  return 0;
}

int neverd_item_clear(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->DataItems.erase(Addr)) {
    S->setError("no data item starts at " + vaHex(Addr));
    return -1;
  }
  return 0;
}

int neverd_operand_format_set(neverd_session_t Sess, neverd_va_t Addr,
                              int Operand, const char *FormatJson) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return -1;
  }
  if (Operand < 0 || Operand >= MaxFormattedOperands) {
    S->setError("an operand index is 0 to 7");
    return -1;
  }
  if (!S->Img.readVA(Addr, 1)) {
    S->setError(vaHex(Addr) + " is not in the image");
    return -1;
  }
  // Formats belong to instructions, which lie in executable code.
  if (llvm::none_of(S->Img.Segments, [&](const Segment &Seg) {
        return Seg.isExecutable() && Seg.contains(Addr);
      })) {
    S->setError(vaHex(Addr) + " is not in executable code");
    return -1;
  }
  std::optional<Session::OperandFormat> Format;
  if (FormatJson) {
    auto Parsed = llvm::json::parse(FormatJson);
    const auto *Object = Parsed ? Parsed->getAsObject() : nullptr;
    if (!Object) {
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
      S->setError("an operand format is a JSON object");
      return -1;
    }
    std::string Error;
    Format = parseOperandFormat(*Object, Error);
    if (!Format) {
      S->setError(Error);
      return -1;
    }
  }
  // The listing's own spelling is no format of the user's.
  auto &Operands = S->OperandFormats[Addr];
  if (!Format ||
      (Format->Base == kNumberBase && !Format->Negate && !Format->Invert))
    Operands.erase(static_cast<unsigned>(Operand));
  else
    Operands[static_cast<unsigned>(Operand)] = std::move(*Format);
  if (Operands.empty())
    S->OperandFormats.erase(Addr);
  return 0;
}

const char *neverd_operand_formats_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  return dupStr(jsonToString(llvm::json::Value(operandFormatRows(S))));
}

int neverd_operand_formats_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, operandsPath(S), operandFormatRows(S),
                     "operand formats")
             ? 0
             : -1;
}

int neverd_operand_formats_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  const auto Path = operandsPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect operand formats: " + EC.message());
    return -1;
  }
  std::map<va_t, std::map<unsigned, Session::OperandFormat>> Formats;
  if (Exists) {
    std::ifstream In(Path);
    if (!In.is_open()) {
      S->setError("cannot read operand formats");
      return -1;
    }
    const std::string Content((std::istreambuf_iterator<char>(In)),
                              std::istreambuf_iterator<char>());
    auto Parsed = llvm::json::parse(Content);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("operand formats sidecar is not valid JSON");
      return -1;
    }
    const auto *Rows = Parsed->getAsArray();
    if (!Rows) {
      S->setError("operand formats sidecar is not an array");
      return -1;
    }
    for (const auto &Value : *Rows) {
      const auto *Row = Value.getAsObject();
      const llvm::json::Value *Address = Row ? Row->get("addr") : nullptr;
      const auto Addr =
          Address ? parsePersistedAddress(*Address) : std::optional<va_t>();
      const auto *Operands = Row ? Row->getArray("operands") : nullptr;
      if (!Addr || !Operands) {
        S->setError("operand formats sidecar has an invalid row");
        return -1;
      }
      for (const auto &Entry : *Operands) {
        const auto *Object = Entry.getAsObject();
        const auto Index =
            Object ? Object->getInteger("operand") : std::nullopt;
        std::string Error;
        auto Format =
            Object ? parseOperandFormat(*Object, Error) : std::nullopt;
        if (!Index || *Index < 0 || *Index >= MaxFormattedOperands || !Format) {
          S->setError("operand formats sidecar has an invalid operand" +
                      (Error.empty() ? std::string() : ": " + Error));
          return -1;
        }
        Formats[*Addr][static_cast<unsigned>(*Index)] = std::move(*Format);
      }
    }
  }
  S->OperandFormats = std::move(Formats);
  return 0;
}

const char *neverd_items_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  return dupStr(jsonToString(llvm::json::Value(dataItemRows(S))));
}

int neverd_items_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, itemsPath(S), dataItemRows(S), "data items") ? 0 : -1;
}

int neverd_items_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  const auto Path = itemsPath(S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect data items: " + EC.message());
    return -1;
  }
  std::map<va_t, Session::DataItem> Items;
  if (Exists) {
    std::ifstream In(Path);
    if (!In.is_open()) {
      S->setError("cannot read data items");
      return -1;
    }
    const std::string Content((std::istreambuf_iterator<char>(In)),
                              std::istreambuf_iterator<char>());
    auto Parsed = llvm::json::parse(Content);
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("data items sidecar is not valid JSON");
      return -1;
    }
    const auto *Rows = Parsed->getAsArray();
    if (!Rows) {
      S->setError("data items sidecar is not an array");
      return -1;
    }
    for (const auto &Value : *Rows) {
      const auto *Row = Value.getAsObject();
      const llvm::json::Value *Address = Row ? Row->get("addr") : nullptr;
      const auto Addr =
          Address ? parsePersistedAddress(*Address) : std::optional<va_t>();
      std::string Error;
      auto Item = Addr ? parseDataItem(*S, *Addr, *Row, Error) : std::nullopt;
      if (!Item || overlappingItem(Items, *Addr, Item->Size, InvalidVA)) {
        S->setError("data items sidecar has an invalid row" +
                    (Error.empty() ? std::string() : ": " + Error));
        return -1;
      }
      Items[*Addr] = std::move(*Item);
    }
  }
  S->DataItems = std::move(Items);
  return 0;
}

// ===--------------------------------------------------------------------===//
// Load options
// ===--------------------------------------------------------------------===//

namespace {

/// The loader name that reads a file as its header or contents say, and
/// the platform name that reads a binary file's platform from its code.
constexpr llvm::StringLiteral AutomaticLoaderName("auto");
/// Who chose a binary file's platform: detection, or the user.
constexpr llvm::StringLiteral DetectedPlatformSource("detected");
constexpr llvm::StringLiteral ChosenPlatformSource("user");

std::filesystem::path loadOptionsPath(const std::filesystem::path &Input) {
  auto Path = Input;
  Path += ".neverd-load.json";
  return Path;
}

llvm::Error loadOptionsError(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

/// A number of the load options: an integer, or hexadecimal digits with or
/// without 0x; \p Missing when the key is absent.
std::optional<uint64_t> optionNumber(const llvm::json::Object &Object,
                                     llvm::StringRef Key, uint64_t Missing) {
  const llvm::json::Value *Value = Object.get(Key);
  if (!Value)
    return Missing;
  if (auto Integer = Value->getAsUINT64())
    return *Integer;
  if (auto Text = Value->getAsString()) {
    llvm::StringRef Digits = *Text;
    if (!Digits.consume_front("0x"))
      Digits.consume_front("0X");
    uint64_t Result = 0;
    if (!Digits.empty() && !Digits.getAsInteger(16, Result))
      return Result;
  }
  return std::nullopt;
}

} // namespace

llvm::Expected<LoaderChoice>
neverd::sdk::parseLoadOptions(llvm::StringRef Text) {
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Object = Parsed->getAsObject();
  if (!Object)
    return loadOptionsError("load options are not a JSON object");
  const auto Loader = Object->getString("loader");
  LoaderChoice Choice;
  if (!Loader || *Loader == AutomaticLoaderName)
    return Choice;
  if (*Loader == getLoadRowLoader(LoadRow::EVM)) {
    Choice.Format = BinaryFormat::EVM;
    return Choice;
  }
  if (*Loader != getLoadRowLoader(LoadRow::Binary))
    return loadOptionsError("unknown loader " + *Loader +
                            "; the loaders are auto, evm and binary");
  Choice.Format = BinaryFormat::Raw;
  RawLoadOptions &Options = Choice.Raw;
  // No processor, or auto, has the load read it from the bytes.
  const llvm::StringRef Named =
      Object->getString("processor").value_or(AutomaticLoaderName);
  if (!Named.empty() && Named != AutomaticLoaderName) {
    const auto Processor = parseRawProcessor(Named);
    if (!Processor)
      return loadOptionsError("a binary file is read as auto, x86, x86_64, "
                              "arm, thumb or aarch64");
    std::tie(Options.TheArch, Options.Mode) = *Processor;
    Options.ProcessorDetected =
        Object->getString("processor_source") == DetectedPlatformSource;
    Options.ProcessorEvidence =
        Object->getString("processor_evidence").value_or("").str();
  }
  const auto Base = optionNumber(*Object, "base", 0);
  const auto Offset = optionNumber(*Object, "offset", 0);
  const auto Size = optionNumber(*Object, "size", 0);
  if (!Base || !Offset || !Size)
    return loadOptionsError("base, offset and size are numbers");
  Options.Base = *Base;
  Options.Offset = *Offset;
  Options.Size = *Size;
  if (Object->get("entry")) {
    const auto Entry = optionNumber(*Object, "entry", 0);
    if (!Entry)
      return loadOptionsError("entry is a number");
    Options.Entry = *Entry;
  }
  // The platform whose conventions the code follows; auto reads it from the
  // code when the file loads.
  if (const auto Platform = Object->getString("platform");
      Platform && *Platform != AutomaticLoaderName) {
    Options.Platform = parseRawPlatform(*Platform);
    if (!Options.Platform)
      return loadOptionsError("platform is auto, sysv, windows or darwin");
    Options.PlatformDetected =
        Object->getString("platform_source") == DetectedPlatformSource;
    Options.PlatformEvidence =
        Object->getString("platform_evidence").value_or("").str();
  }
  return Choice;
}

std::string neverd::sdk::loadOptionsJson(const LoaderChoice &Choice) {
  llvm::json::Object Object;
  switch (Choice.Format) {
  case BinaryFormat::EVM:
    Object["loader"] = getLoadRowLoader(LoadRow::EVM);
    break;
  case BinaryFormat::Raw: {
    const RawLoadOptions &Raw = Choice.Raw;
    Object["loader"] = getLoadRowLoader(LoadRow::Binary);
    if (Raw.TheArch == Arch::Unknown) {
      Object["processor"] = AutomaticLoaderName;
    } else {
      Object["processor"] = getRawProcessorName(Raw.TheArch, Raw.Mode);
      Object["processor_source"] =
          Raw.ProcessorDetected ? DetectedPlatformSource : ChosenPlatformSource;
      if (!Raw.ProcessorEvidence.empty())
        Object["processor_evidence"] = Raw.ProcessorEvidence;
    }
    Object["base"] = vaHex(Raw.Base);
    Object["offset"] = vaHex(Raw.Offset);
    Object["size"] = vaHex(Raw.Size);
    if (Raw.Entry)
      Object["entry"] = vaHex(*Raw.Entry);
    if (Raw.Platform) {
      Object["platform"] = getRawPlatformName(*Raw.Platform);
      Object["platform_source"] =
          Raw.PlatformDetected ? DetectedPlatformSource : ChosenPlatformSource;
      if (!Raw.PlatformEvidence.empty())
        Object["platform_evidence"] = Raw.PlatformEvidence;
    } else {
      Object["platform"] = AutomaticLoaderName;
    }
    break;
  }
  default:
    Object["loader"] = AutomaticLoaderName;
    break;
  }
  return jsonToString(llvm::json::Value(std::move(Object)));
}

llvm::Expected<LoaderChoice>
neverd::sdk::readLoadOptionsSidecar(const std::filesystem::path &Input) {
  const auto Path = loadOptionsPath(Input);
  std::error_code EC;
  if (!std::filesystem::exists(Path, EC))
    return LoaderChoice();
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path));
  if (!Buffer)
    return loadOptionsError("cannot read the load options sidecar");
  auto Options = parseLoadOptions((*Buffer)->getBuffer());
  if (!Options)
    return loadOptionsError("load options sidecar: " +
                            llvm::toString(Options.takeError()));
  return Options;
}

llvm::Error
neverd::sdk::settleBinaryFileProcessor(const std::filesystem::path &Input,
                                       LoaderChoice &Choice) {
  RawLoadOptions &Raw = Choice.Raw;
  if (Choice.Format != BinaryFormat::Raw || Raw.TheArch != Arch::Unknown)
    return llvm::Error::success();
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Input),
                                            /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return loadOptionsError("cannot read the binary file");
  // The bytes the load reads.
  llvm::ArrayRef<uint8_t> Bytes =
      llvm::arrayRefFromStringRef((*Buffer)->getBuffer());
  Bytes = Bytes.drop_front(std::min<uint64_t>(Raw.Offset, Bytes.size()));
  if (Raw.Size)
    Bytes = Bytes.take_front(std::min<uint64_t>(Raw.Size, Bytes.size()));
  const ISAIdentification Found = identifyISA(Bytes);
  const llvm::StringRef Processor = Found.detectedProcessor();
  using Verdict = ISAIdentification::Verdict;
  if (Processor.empty()) {
    switch (Found.Outcome) {
    case Verdict::NoCode:
      return loadOptionsError("no part of the file looks like code of an "
                              "instruction set NeverD knows; name the "
                              "processor to read it as");
    case Verdict::Settled:
      return loadOptionsError(llvm::formatv(
          "the code looks like {0}, which NeverD cannot decode; name a "
          "processor to read it as one anyway",
          Found.Guesses.front().Name));
    case Verdict::WidthUnclear:
      return loadOptionsError(llvm::formatv(
          "the code looks like {0} or {1}, and its instructions do not tell "
          "which; name the processor to read it as",
          Found.Guesses[0].Name, Found.Guesses[1].Name));
    case Verdict::Unclear:
      return loadOptionsError(
          llvm::formatv("the bytes do not show one processor clearly ({0}); "
                        "name the processor to read it as",
                        Found.describe()));
    }
  }
  // Instructions off their alignment would decode as other instructions.
  if (!Found.Fingerprint && Found.CodeOffset)
    return loadOptionsError(llvm::formatv(
        "the code looks like {0}, whose instructions align to {1} bytes from "
        "offset {2} of the bytes read; set the file offset to {3}",
        Found.Guesses.front().Name, Found.CodeUnit, Found.CodeOffset,
        Raw.Offset + Found.CodeOffset));
  std::tie(Raw.TheArch, Raw.Mode) = *parseRawProcessor(Processor);
  Raw.ProcessorDetected = true;
  Raw.ProcessorEvidence = Found.describe();
  return llvm::Error::success();
}

void neverd::sdk::settleBinaryFilePlatform(BinaryImage &Img,
                                           LoaderChoice &Choice) {
  if (Img.Format != BinaryFormat::Raw ||
      Img.ConventionFormat != BinaryFormat::Unknown)
    return;
  Decoder Dec;
  PlatformEvidence Evidence;
  if (Dec.init(Img))
    Evidence = readPlatformEvidence(Img, Dec);
  Img.ConventionFormat = Evidence.Platform;
  Choice.Raw.Platform = Evidence.Platform;
  Choice.Raw.PlatformDetected = true;
  Choice.Raw.PlatformEvidence = Evidence.describe();
}

int neverd_session_set_load_options(neverd_session_t Sess,
                                    const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!OptionsJson) {
    S->RequestedLoad.reset();
    return 0;
  }
  auto Choice = parseLoadOptions(OptionsJson);
  if (!Choice) {
    S->setError(llvm::toString(Choice.takeError()));
    return -1;
  }
  S->RequestedLoad = *Choice;
  return 0;
}

const char *neverd_session_load_options_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S && S->Loaded ? loadOptionsJson(S->LoadedChoice)
                               : std::string("{}"));
}

int neverd_load_options_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked())
    return -1;
  if (!S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  const auto Path = loadOptionsPath(S->FilePath);
  // A file read as its header or contents say needs no sidecar.
  if (S->LoadedChoice.Format == BinaryFormat::Unknown) {
    std::error_code EC;
    std::filesystem::remove(Path, EC);
    if (EC) {
      S->setError("cannot remove the load options sidecar: " + EC.message());
      return -1;
    }
    return 0;
  }
  auto Value = llvm::json::parse(loadOptionsJson(S->LoadedChoice));
  if (!Value) {
    S->setError(llvm::toString(Value.takeError()));
    return -1;
  }
  return saveSidecar(S, Path, std::move(*Value), "load options") ? 0 : -1;
}
