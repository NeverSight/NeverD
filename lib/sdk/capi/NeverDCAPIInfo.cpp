//===- NeverDCAPIInfo.cpp - C API: binary information queries -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Information panels for loaded images and address resolution.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "LibraryPresentation.h"
#include "SessionImpl.h"

#include "neverd/backend/c/dialect/SourceDialect.h"
#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionEncoding.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/loader/InputDigest.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/StringScan.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/CRC.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

using namespace neverd;
using namespace neverd::sdk;

namespace {
/// The most bytes neverd_decode_text_json decodes at once.
constexpr int kMaxDecodedTextBytes = 65536;
} // namespace

namespace {

/// Add the name C code calls the import \p Symbol by, when its format's
/// decoration makes that differ from the symbol (Mach-O `_printf`).
void addCName(llvm::json::Object &Row, llvm::StringRef Symbol,
              const BinaryImage &Img) {
  const llvm::StringRef CName = cNameOfSymbol(Symbol, Img.Format, Img.Arch);
  if (CName != Symbol)
    Row["c_name"] = jsonSafeText(CName);
}

bool hasMainEntryPoint(const BinaryImage &Img) {
  return Img.Entry != 0 || Img.Arch == Arch::EVM;
}

/// Render one compiler trailer. The whole point of reporting a trailer is that
/// it says who built the contract, so the version and the source address are
/// named rather than left as an opaque byte count.
llvm::json::Object describeContractMetadata(const evm::ContractMetadata &Meta) {
  llvm::json::Object Trailer;
  Trailer["container"] = evm::metadataContainerName(Meta.Container);
  Trailer["offset"] = static_cast<int64_t>(Meta.Offset);
  Trailer["size"] = static_cast<int64_t>(Meta.Size);
  Trailer["language"] = evm::metadataLanguageName(Meta.language());
  if (std::string Version = Meta.compilerVersion(); !Version.empty())
    Trailer["compiler_version"] = std::move(Version);
  if (const evm::MetadataEntry *Hash = Meta.sourceHash()) {
    llvm::json::Object Source;
    Source["kind"] = Hash->Key;
    Source["value"] = llvm::toHex(Hash->Value.Bytes, /*LowerCase=*/true);
    Trailer["source_hash"] = std::move(Source);
  }

  llvm::json::Array Keys;
  for (const evm::MetadataEntry &Entry : Meta.Entries)
    Keys.push_back(Entry.Key);
  Trailer["keys"] = std::move(Keys);

  // A sequence footer describes the runtime code the constructor returns,
  // which is a layout no other part of the input states.
  for (const evm::MetadataSequenceEntry &Entry : Meta.Sequence) {
    if (Entry.Value.Kind != evm::MetadataValueKind::Unsigned)
      continue;
    Trailer[Entry.Element->Name] = static_cast<int64_t>(Entry.Value.Unsigned);
  }
  if (const evm::MetadataSequenceEntry *Integrity =
          Meta.find(evm::MetadataSequenceElement::IntegrityHash))
    Trailer[Integrity->Element->Name] =
        llvm::toHex(Integrity->Value.Bytes, /*LowerCase=*/true);
  return Trailer;
}

llvm::json::Object describeEVMImage(const evm::ImageMetadata &Meta) {
  llvm::json::Object EVM;
  const evm::BytecodeContainerInfo &Container =
      evm::getBytecodeContainerInfo(Meta.Container);
  EVM["source"] = evm::bytecodeSourceName(Meta.Source);
  EVM["container"] = Container.Name;
  EVM["hardfork"] = evm::hardforkName(Meta.Fork);
  EVM["runtime_extracted"] = Meta.RuntimeExtracted;
  EVM["metadata_stripped"] = Meta.MetadataStripped;
  if (!Container.EIP.empty())
    EVM["container_eip"] = Container.EIP;
  // Whether the marker means anything at the fork being analyzed is a separate
  // fact from what the bytes say, and both are worth reporting.
  if (std::optional<evm::Hardfork> Activated =
          evm::bytecodeContainerActivation(Meta.Container)) {
    EVM["container_activated_at"] = evm::hardforkName(*Activated);
    EVM["container_active"] = evm::hardforkAtLeast(Meta.Fork, *Activated);
  }
  if (!Meta.DelegateTarget.empty())
    EVM["delegate_target"] =
        "0x" + llvm::toHex(Meta.DelegateTarget, /*LowerCase=*/true);
  if (Meta.InputMetadata)
    EVM["input_metadata"] = describeContractMetadata(*Meta.InputMetadata);
  if (Meta.RuntimeMetadata)
    EVM["runtime_metadata"] = describeContractMetadata(*Meta.RuntimeMetadata);
  return EVM;
}

} // namespace

llvm::json::Object Session::functionIdentity(va_t Entry) const {
  const auto At =
      std::lower_bound(Functions.begin(), Functions.end(), Entry,
                       [](const auto &F, va_t E) { return F.Entry < E; });
  if (At == Functions.end() || At->Entry != Entry)
    return {};
  const auto &F = *At;
  std::string Display =
      F.Origin == NameOrigin::User ? F.Name : demangledName(F.Name);
  llvm::json::Array Annotations;
  const sigs::LibraryRecognition *Whole = nullptr;
  bool Ambiguous = false;
  const bool Current = PipeRan && PipeResult.Success &&
                       PipelineFeatureGeneration == SigDB.featureGeneration();
  const auto Indexed = PipeResult.LibraryRecognitionsByFunction.find(Entry);
  const bool Analyzed =
      Current && Indexed != PipeResult.LibraryRecognitionsByFunction.end();
  if (Analyzed)
    for (size_t Index : Indexed->second) {
      const auto &R = PipeResult.LibraryRecognitions[Index];
      Annotations.push_back(libraryRecognitionJSON(R));
      if (R.Scope == sigs::LibraryFeatureScope::WholeFunction) {
        Ambiguous |= Whole != nullptr;
        Whole = &R;
      }
    }
  const bool AnnotatedDisplay =
      Whole && !Ambiguous && F.Origin < NameOrigin::Stated;
  if (AnnotatedDisplay)
    Display = Whole->DisplayName;
  const char *State =
      SigDB.featurePacks().empty() ? "disabled"
      : !Analyzed                  ? "pending"
      : PipeResult.LibraryRecognitionBudgetExhausted.contains(Entry)
          ? "budget-exhausted"
      : Annotations.empty() ? "no-match"
                            : "matched";
  return llvm::json::Object{{"name", jsonSafeText(F.Name)},
                            {"display_name", jsonSafeText(Display)},
                            {"linkage_name", jsonSafeText(F.LinkageName)},
                            {"name_origin", nameOriginText(F.Origin)},
                            {"display_origin", AnnotatedDisplay
                                                   ? "recognition"
                                                   : nameOriginText(F.Origin)},
                            {"linkage_origin", nameOriginText(F.LinkageOrigin)},
                            {"addr", vaHex(Entry)},
                            {"size", static_cast<int64_t>(F.Size)},
                            {"recognition_state", State},
                            {"library_annotations", std::move(Annotations)}};
}

// ===--------------------------------------------------------------------===//
// Info panels (JSON)
// ===--------------------------------------------------------------------===//

const char *neverd_imports_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  // Executable veneers forwarding to each import: exact registrations, and
  // function entries inside import-stub machinery whose thunk decodes to one.
  const auto &Imports = S->Img.Imports;
  std::vector<std::vector<va_t>> Stubs(Imports.size());
  const auto NoteStub = [&](va_t Addr, const Import *Imp) {
    if (!Imp || Imp < Imports.data() || Imp >= Imports.data() + Imports.size())
      return;
    auto &List = Stubs[static_cast<size_t>(Imp - Imports.data())];
    if (llvm::find(List, Addr) == List.end())
      List.push_back(Addr);
  };
  for (const auto &[Addr, Index] : S->Img.ImportStubIndices)
    if (Index < Imports.size())
      NoteStub(Addr, &Imports[Index]);
  if (S->synchronizeFunctions())
    for (const auto &F : S->Functions)
      if (S->Img.isImportStubAt(F.Entry))
        NoteStub(F.Entry, S->Img.findImportStubAt(F.Entry));

  llvm::json::Array Arr;
  for (size_t I = 0; I < Imports.size(); ++I) {
    const auto &Imp = Imports[I];
    llvm::json::Object Obj;
    Obj["module"] = jsonSafeText(Imp.Module);
    Obj["name"] = jsonSafeText(Imp.Name);
    addCName(Obj, Imp.Name, S->Img);
    Obj["ordinal"] = static_cast<int64_t>(Imp.Ordinal);
    Obj["iat_addr"] = vaHex(Imp.IATAddr);
    llvm::sort(Stubs[I]);
    llvm::json::Array StubArr;
    for (va_t Addr : Stubs[I])
      StubArr.push_back(vaHex(Addr));
    Obj["stubs"] = std::move(StubArr);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_import_slots_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return dupStr(std::string("[]"));
  const auto Collection = S->Img.collectImportStorageSlots();
  std::map<va_t, std::pair<std::string, int64_t>> Slots;
  std::set<va_t> Conflicts(Collection.Conflicts.begin(),
                           Collection.Conflicts.end());
  const auto Note = [&](va_t Addr, const std::string &Name, int64_t Addend) {
    if (Name.empty())
      return;
    auto [It, Inserted] = Slots.try_emplace(Addr, Name, Addend);
    if (!Inserted && (It->second.first != Name || It->second.second != Addend))
      Conflicts.insert(Addr);
  };
  for (const auto &[Addr, Slot] : Collection.Slots)
    Note(Addr, Slot.Name, Slot.Addend);
  if (S->Img.Format == BinaryFormat::ELF && !S->Img.IsRelocatable)
    for (const auto &Rel : S->Img.Relocations)
      if (elf_loader::isELFSlotBinding(S->Img.Arch, Rel.Type))
        Note(Rel.Address, Rel.SymbolName, Rel.Addend);
  llvm::json::Array Arr;
  for (const auto &[Addr, Slot] : Slots) {
    if (Conflicts.count(Addr))
      continue;
    llvm::json::Object Obj{{"addr", vaHex(Addr)},
                           {"name", jsonSafeText(Slot.first)},
                           {"addend", Slot.second}};
    addCName(Obj, Slot.first, S->Img);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_unwind_frame_json(neverd_session_t Sess,
                                     neverd_va_t Address) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded)
    return dupStr(std::string("null"));
  const ExceptionFunction *F = S->Img.ExceptionMetadata.findFunction(Address);
  if (!F || !F->CodeRange.isValid())
    return dupStr(std::string("null"));
  llvm::json::Object Obj{{"begin", vaHex(F->CodeRange.Begin)},
                         {"end", vaHex(F->CodeRange.End)},
                         {"encoding", getExceptionEncodingName(F->Encoding)},
                         {"language_data", F->hasLanguageTable()}};
  if (!F->PersonalityName.empty())
    Obj["personality"] = jsonSafeText(F->PersonalityName);
  return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
}

const char *neverd_exports_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Exp : S->Img.Exports) {
    llvm::json::Object Obj;
    Obj["name"] = jsonSafeText(Exp.Name);
    Obj["ordinal"] = static_cast<int64_t>(Exp.Ordinal);
    Obj["addr"] = vaHex(Exp.Addr);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_segments_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Seg : S->Img.Segments) {
    llvm::json::Object Obj;
    Obj["name"] = jsonSafeText(Seg.Name);
    Obj["va"] = vaHex(Seg.VA);
    Obj["size"] = vaHex(Seg.Size);
    std::string Flags;
    Flags += Seg.isReadable() ? 'R' : '-';
    Flags += Seg.isWritable() ? 'W' : '-';
    Flags += Seg.isExecutable() ? 'X' : '-';
    Obj["flags"] = Flags;
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_strings_json(neverd_session_t Sess, int MinLength) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  unsigned MinLen = MinLength > 0 ? static_cast<unsigned>(MinLength) : 4;
  // Streamed in the sorted key order a json::Object prints.
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  llvm::json::OStream J(OS);
  J.array([&] {
    for (const auto &Seg : S->Img.Segments) {
      if (Seg.isExecutable())
        continue;
      const uint8_t *Data = Seg.Data.data();
      size_t Len = Seg.Data.size();
      size_t RunStart = 0, RunLen = 0;
      for (size_t I = 0; I <= Len; ++I) {
        uint8_t B = (I < Len) ? Data[I] : 0;
        bool IsPrintable = (B >= 0x20 && B < 0x7F) || B == '\t' || B == '\n';
        if (IsPrintable) {
          if (RunLen == 0)
            RunStart = I;
          ++RunLen;
        } else {
          if (RunLen >= MinLen && B == 0)
            J.object([&] {
              J.attribute("addr", vaHex(Seg.VA + RunStart));
              J.attribute("length", static_cast<int64_t>(RunLen));
              J.attribute(
                  "value",
                  llvm::StringRef(
                      reinterpret_cast<const char *>(Data + RunStart), RunLen));
            });
          RunLen = 0;
        }
      }
    }
  });
  OS.flush();
  return dupStr(Buf);
}

/// One row of neverd_strings_ex_json(), keys in the order a json::Object
/// prints them.
static void writeStringRow(llvm::json::OStream &J, const ImageString &String) {
  J.object([&] {
    J.attribute("addr", vaHex(String.Address));
    J.attribute("chars", static_cast<int64_t>(String.Chars));
    J.attribute("encoding", strings::encodingName(String.Kind));
    J.attribute("length", static_cast<int64_t>(String.Bytes));
    J.attribute("value", String.Text);
  });
}

const char *neverd_strings_ex_json(neverd_session_t Sess,
                                   const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded)
    return dupStr(std::string("[]"));
  strings::ScanOptions Options;
  if (const auto Error = parseStringOptions(OptionsJson, Options);
      !Error.empty()) {
    S->setError(Error);
    return nullptr;
  }
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  llvm::json::OStream J(OS);
  J.array([&] {
    for (const ImageString &String : imageStrings(*S, Options))
      writeStringRow(J, String);
  });
  OS.flush();
  return dupStr(Buf);
}

const char *neverd_strings_page_json(neverd_session_t Sess,
                                     const char *OptionsJson,
                                     neverd_va_t FirstAddr, int MaxRows) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  strings::ScanOptions Options;
  if (const auto Error = parseStringOptions(OptionsJson, Options);
      !Error.empty()) {
    S->setError(Error);
    return nullptr;
  }
  constexpr int MaxRowsPerPage = 65536;
  const size_t Limit =
      static_cast<size_t>(std::clamp(MaxRows, 1, MaxRowsPerPage));
  // The scan runs once per options; a page reads it from its cursor.
  static const std::vector<ImageString> None;
  const auto &Strings = S->Loaded ? imageStrings(*S, Options) : None;
  const auto Begin =
      std::lower_bound(Strings.begin(), Strings.end(), FirstAddr,
                       [](const ImageString &String, va_t Value) {
                         return String.Address < Value;
                       });
  // A page never splits the strings that start at one address.
  auto End = Begin;
  for (size_t Count = 0; End != Strings.end(); ++End, ++Count)
    if (Count >= Limit && End->Address != std::prev(End)->Address)
      break;
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  llvm::json::OStream J(OS);
  J.object([&] {
    J.attribute("next_addr", End != Strings.end()
                                 ? llvm::json::Value(vaHex(End->Address))
                                 : llvm::json::Value(nullptr));
    J.attributeArray("strings", [&] {
      for (auto It = Begin; It != End; ++It)
        writeStringRow(J, *It);
    });
  });
  OS.flush();
  return dupStr(Buf);
}

const char *neverd_string_at(neverd_session_t Sess, neverd_va_t Addr,
                             const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  strings::ScanOptions Options;
  Options.MinLength = 1;
  if (const auto Error = parseStringOptions(OptionsJson, Options);
      !Error.empty()) {
    S->setError(Error);
    return nullptr;
  }
  // A string that does not end within this many bytes is not read.
  constexpr size_t Window = 65536;
  const Segment *Seg = S->Img.getSegmentFor(Addr);
  const uint64_t Offset = Seg && Addr >= Seg->VA ? Addr - Seg->VA : 0;
  if (!Seg || Addr < Seg->VA || Offset >= Seg->Data.size()) {
    S->setError("no initialized data at " + vaHex(Addr));
    return nullptr;
  }
  const llvm::ArrayRef<uint8_t> Bytes(
      Seg->Data.data() + Offset,
      std::min<size_t>(Window, Seg->Data.size() - static_cast<size_t>(Offset)));
  std::optional<strings::FoundString> First;
  strings::scan(Bytes, Options, [&](strings::FoundString &&Found) {
    if (!First)
      First = std::move(Found);
  });
  if (!First || First->Offset != 0) {
    S->setError("no string starts at " + vaHex(Addr));
    return nullptr;
  }
  llvm::json::Object Row{
      {"addr", vaHex(Addr)},
      {"chars", static_cast<int64_t>(First->Chars)},
      {"encoding", strings::encodingName(First->Kind)},
      {"length", static_cast<int64_t>(First->Bytes)},
      {"unit", static_cast<int64_t>(strings::encodingUnitBytes(First->Kind))},
      {"value", std::move(First->Text)}};
  return dupStr(jsonToString(llvm::json::Value(std::move(Row))));
}

const char *neverd_string_encodings_json(void) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  llvm::json::OStream J(OS);
  const strings::ScanOptions Defaults;
  J.array([&] {
    for (unsigned I = 0;; ++I) {
      const auto Encoding = static_cast<strings::Encoding>(I);
      if (strings::encodingName(Encoding).empty())
        break;
      J.object([&] {
        J.attribute("default",
                    (Defaults.Encodings & strings::encodingBit(Encoding)) != 0);
        J.attribute("legacy", strings::isLegacyEncoding(Encoding));
        J.attribute("name", strings::encodingName(Encoding));
        J.attribute("spelling", strings::encodingSpelling(Encoding));
        J.attribute("unit",
                    static_cast<int64_t>(strings::encodingUnitBytes(Encoding)));
      });
    }
  });
  OS.flush();
  return dupStr(Buf);
}

const char *neverd_fold_case(const char *Text) {
  return Text ? dupStr(strings::foldCase(Text)) : nullptr;
}

const char *neverd_demangle(const char *Name) {
  return Name ? dupStr(demangledName(Name)) : nullptr;
}

const char *neverd_decode_text_json(const unsigned char *Bytes, int Size,
                                    const char *Encoding) {
  const auto Kind = Encoding ? strings::encodingNamed(Encoding) : std::nullopt;
  if (!Kind || Size < 0 || Size > kMaxDecodedTextBytes || (Size && !Bytes))
    return nullptr;
  std::vector<std::optional<std::string>> Cells(static_cast<size_t>(Size));
  strings::decode(
      llvm::ArrayRef<uint8_t>(Bytes, static_cast<size_t>(Size)), *Kind,
      [&](const strings::DecodedCharacter &Character) {
        std::string Text;
        bool Shown = Character.Codes > 0;
        for (unsigned I = 0; I < Character.Codes; ++I) {
          // A combining mark shows with the letter before it.
          Shown &= I > 0 || strings::isShownCharacter(Character.Code[I]);
          char Buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
          char *End = Buffer;
          llvm::ConvertCodePointToUTF8(Character.Code[I], End);
          Text.append(Buffer, End);
        }
        const uint64_t Last = std::min<uint64_t>(
            Cells.size(), Character.Offset + Character.Bytes);
        if (Shown)
          Cells[Character.Offset] = std::move(Text);
        for (uint64_t I = Character.Offset + 1; Shown && I < Last; ++I)
          Cells[I] = std::string();
      });
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  llvm::json::OStream J(OS);
  J.object([&] {
    J.attributeArray("cells", [&] {
      for (const auto &Cell : Cells)
        if (Cell)
          J.value(*Cell);
        else
          J.value(nullptr);
    });
  });
  OS.flush();
  return dupStr(Buf);
}

const char *neverd_sections_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Sec : S->Img.Sections) {
    llvm::json::Object Obj;
    Obj["name"] = jsonSafeText(Sec.Name);
    Obj["segment"] = jsonSafeText(Sec.SegmentName);
    Obj["va"] = vaHex(Sec.VA);
    Obj["size"] = static_cast<int64_t>(Sec.Size);
    Obj["file_off"] = static_cast<int64_t>(Sec.FileOff);
    Obj["file_sz"] = static_cast<int64_t>(Sec.FileSz);
    std::string Flags;
    Flags += Sec.isReadable() ? 'R' : '-';
    Flags += Sec.isWritable() ? 'W' : '-';
    Flags += Sec.isExecutable() ? 'X' : '-';
    Obj["flags"] = Flags;
    Obj["alignment"] = static_cast<int64_t>(Sec.Alignment);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

/// The symbols as rows, streamed in the sorted key order a json::Object
/// prints (building an object tree per symbol costs more than reading the
/// symbols).  The user's names replace the symbols' and name, in a row of
/// their own, an address no symbol names; \p Data keeps data alone.
static const char *symbolRows(Session &S, bool Data) {
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  llvm::json::OStream J(OS);
  const auto Row = [&](va_t Addr, llvm::StringRef Name, uint64_t Size) {
    J.object([&] {
      J.attribute("addr", vaHex(Addr));
      J.attribute("name", jsonSafeText(Name));
      J.attribute("size", static_cast<int64_t>(Size));
    });
  };
  std::set<va_t> Renamed;
  J.array([&] {
    for (const auto &Sym : S.Img.Symbols) {
      if (Data && Sym.IsFunc)
        continue;
      const auto Rename = S.Renames.find(Sym.Addr);
      if (Rename == S.Renames.end()) {
        Row(Sym.Addr, Sym.Name, Sym.Size);
        continue;
      }
      Renamed.insert(Sym.Addr);
      Row(Sym.Addr, Rename->second, Sym.Size);
    }
    for (const auto &[Addr, Name] : S.Renames) {
      if (Renamed.count(Addr) ||
          (Data && llvm::any_of(S.Functions, [&](const auto &F) {
             return F.Entry == Addr;
           })))
        continue;
      Row(Addr, Name, 0);
    }
  });
  OS.flush();
  return dupStr(Buf);
}

const char *neverd_symbols_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");
  return symbolRows(*S, false);
}

const char *neverd_data_symbols_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");
  return symbolRows(*S, true);
}

const char *neverd_relocs_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Rel : S->Img.Relocations) {
    llvm::json::Object Obj;
    Obj["addr"] = vaHex(Rel.Address);
    Obj["type"] = static_cast<int64_t>(Rel.Type);
    Obj["symbol"] = jsonSafeText(Rel.SymbolName);
    Obj["section"] = jsonSafeText(Rel.SectionName);
    Obj["addend"] = Rel.Addend;
    Obj["sym_index"] = static_cast<int64_t>(Rel.SymbolIndex);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_headers_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("{}");
  (void)S->synchronizeFunctions();

  llvm::json::Object Root;
  Root["entry"] = vaHex(S->Img.Entry);
  Root["base"] = vaHex(S->Img.Base);
  Root["arch"] = getArchName(S->Img.Arch);
  Root["instruction_mode"] = getInstructionModeName(S->Img.Mode);
  Root["format"] = S->Img.getFormatName();
  Root["bits"] = S->Img.is64Bit() ? 64 : 32;
  Root["file_path"] = jsonSafeText(pathToUTF8(S->FilePath));

  Root["file_size"] = static_cast<int64_t>(S->inputFileSize());

  Root["segment_count"] = static_cast<int64_t>(S->Img.Segments.size());
  Root["section_count"] = static_cast<int64_t>(S->Img.Sections.size());
  Root["func_count"] = static_cast<int64_t>(S->Functions.size());
  Root["symbol_count"] = static_cast<int64_t>(S->Img.Symbols.size());
  Root["import_count"] = static_cast<int64_t>(S->Img.Imports.size());
  Root["export_count"] = static_cast<int64_t>(S->Img.Exports.size());
  Root["reloc_count"] = static_cast<int64_t>(S->Img.Relocations.size());
  Root["base_reloc_count"] =
      static_cast<int64_t>(S->Img.BaseRelocations.size());

  if (S->Img.SBF) {
    const sbf::Metadata &Metadata = *S->Img.SBF;
    llvm::json::Object SBF;
    SBF["version"] = sbf::versionName(Metadata.Version);
    SBF["version_display"] = sbf::versionDisplayName(Metadata.Version);
    SBF["elf_flags"] = static_cast<int64_t>(Metadata.ELFFlags);
    SBF["machine"] = static_cast<int64_t>(Metadata.Machine);
    llvm::StringRef MachineName = sbf::kUnknownELFMachineName;
    if (Metadata.Machine == sbf::kELFMachineBPF)
      MachineName = sbf::kELFMachineBPFName;
    else if (Metadata.Machine == sbf::kELFMachineSBPF)
      MachineName = sbf::kELFMachineSBPFName;
    SBF["machine_name"] = MachineName;
    SBF["layout"] =
        Metadata.StrictLayout ? sbf::kStrictLayoutName : sbf::kLegacyLayoutName;
    SBF["debug_enrichment"] =
        sbf::debugEnrichmentStatusName(Metadata.DebugEnrichment);
    llvm::json::Object Text;
    Text["file_offset"] = static_cast<int64_t>(Metadata.TextFile.Offset);
    Text["file_size"] = static_cast<int64_t>(Metadata.TextFile.Size);
    Text["vm_address"] = vaHex(Metadata.TextVM.Address);
    Text["vm_size"] = static_cast<int64_t>(Metadata.TextVM.Size);
    SBF["text"] = std::move(Text);
    llvm::json::Object Rodata;
    Rodata["file_offset"] = static_cast<int64_t>(Metadata.RodataFile.Offset);
    Rodata["file_size"] = static_cast<int64_t>(Metadata.RodataFile.Size);
    Rodata["vm_address"] = vaHex(Metadata.RodataVM.Address);
    Rodata["vm_size"] = static_cast<int64_t>(Metadata.RodataVM.Size);
    SBF["rodata"] = std::move(Rodata);
    Root["sbf"] = std::move(SBF);
  }

  if (S->Img.EVM)
    Root["evm"] = describeEVMImage(*S->Img.EVM);

  // The language whose runtime built the image, which decides how its names
  // read.  Evidence comes from LanguageRuntime.def, the version from the
  // image's own release string.
  const LanguageRuntimeInfo &Language = S->Img.ExceptionMetadata.Runtime;
  llvm::json::Object LanguageInfo;
  LanguageInfo["runtime"] = getSourceLanguageRuntimeName(Language.Runtime);
  if (!Language.Version.empty())
    LanguageInfo["version"] = jsonSafeText(Language.Version);
  llvm::json::Array Secondary;
  for (SourceLanguageRuntime Runtime : Language.SecondaryRuntimes)
    Secondary.push_back(getSourceLanguageRuntimeName(Runtime));
  LanguageInfo["secondary"] = std::move(Secondary);
  llvm::json::Array Evidence;
  for (const std::string &Item : Language.Evidence)
    Evidence.push_back(jsonSafeText(Item));
  LanguageInfo["evidence"] = std::move(Evidence);
  // The languages its pseudocode reads in: C, and the program's own.
  llvm::json::Array Pseudocode;
  for (SourceDialect Dialect : offeredSourceDialects(Language))
    Pseudocode.push_back(sourceDialectKey(Dialect));
  LanguageInfo["pseudocode"] = std::move(Pseudocode);
  Root["language"] = std::move(LanguageInfo);

  llvm::json::Object Dyn;
  const auto &DI = S->Img.DynInfo;
  if (!DI.SOName.empty())
    Dyn["soname"] = DI.SOName;
  if (!DI.NeededLibs.empty()) {
    llvm::json::Array Needed;
    for (const auto &Lib : DI.NeededLibs)
      Needed.push_back(Lib);
    Dyn["needed"] = std::move(Needed);
  }
  if (!DI.RPaths.empty()) {
    llvm::json::Array RP;
    for (const auto &P : DI.RPaths)
      RP.push_back(P);
    Dyn["rpaths"] = std::move(RP);
  }
  if (DI.InitAddr)
    Dyn["init_addr"] = vaHex(DI.InitAddr);
  if (DI.FiniAddr)
    Dyn["fini_addr"] = vaHex(DI.FiniAddr);
  if (!DI.PreinitArray.empty()) {
    llvm::json::Array PA;
    for (auto A : DI.PreinitArray)
      PA.push_back(vaHex(A));
    Dyn["preinit_array"] = std::move(PA);
  }
  if (!DI.InitArray.empty()) {
    llvm::json::Array IA;
    for (auto A : DI.InitArray)
      IA.push_back(vaHex(A));
    Dyn["init_array"] = std::move(IA);
  }
  if (!DI.FiniArray.empty()) {
    llvm::json::Array FA;
    for (auto A : DI.FiniArray)
      FA.push_back(vaHex(A));
    Dyn["fini_array"] = std::move(FA);
  }
  if (!DI.PDBPath.empty())
    Dyn["pdb_path"] = DI.PDBPath;
  if (!DI.UUID.empty())
    Dyn["uuid"] = DI.UUID;
  if (!DI.MinOSVersion.empty())
    Dyn["min_os_version"] = DI.MinOSVersion;
  if (DI.SecurityCookieRVA)
    Dyn["security_cookie_rva"] = vaHex(DI.SecurityCookieRVA);
  if (DI.GuardCFCheckFunctionRVA)
    Dyn["guard_cf_check_rva"] = vaHex(DI.GuardCFCheckFunctionRVA);
  Root["dynamic"] = std::move(Dyn);

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

const char *neverd_entrypoints_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S || !S->Loaded) {
    if (S)
      S->setError("session not loaded");
    return dupStr("[]");
  }
  const auto &Img = S->Img;
  const auto &DI = Img.DynInfo;

  llvm::json::Array Arr;

  auto addEntry = [&](const char *Type, neverd::va_t Addr,
                      const std::string &Name, bool AllowZero = false) {
    if (!Addr && !AllowZero)
      return;
    llvm::json::Object O;
    O["type"] = Type;
    O["addr"] = vaHex(Addr);
    O["name"] = jsonSafeText(Name);
    Arr.push_back(std::move(O));
  };

  addEntry("entry", Img.Entry, Img.getFunctionNameAt(Img.Entry),
           hasMainEntryPoint(Img));
  addEntry("init", DI.InitAddr, Img.getFunctionNameAt(DI.InitAddr));
  addEntry("fini", DI.FiniAddr, Img.getFunctionNameAt(DI.FiniAddr));

  for (va_t Addr : DI.PreinitArray)
    addEntry("preinit_array", Addr, Img.getFunctionNameAt(Addr));
  for (va_t Addr : DI.InitArray)
    addEntry("init_array", Addr, Img.getFunctionNameAt(Addr));
  for (va_t Addr : DI.FiniArray)
    addEntry("fini_array", Addr, Img.getFunctionNameAt(Addr));

  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

/// The SHA-256 of the input as loaded, in lowercase hexadecimal: the one the
/// loader took while it read the input, else one hash of the file, kept.
/// Empty when the file cannot be read.
static std::string inputSha256(Session &S) {
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

const char *neverd_session_input_sha256(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  return dupStr(S->Loaded ? inputSha256(*S) : std::string());
}

const char *neverd_dashboard_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("{}");
  (void)S->synchronizeFunctions();

  llvm::json::Object Root;

  llvm::json::Object File;
  File["path"] = jsonSafeText(pathToUTF8(S->FilePath));
  File["name"] = jsonSafeText(pathToUTF8(S->FilePath.filename()));
  File["format"] = S->Img.getFormatName();
  File["arch"] = getArchName(S->Img.Arch);
  File["instruction_mode"] = getInstructionModeName(S->Img.Mode);
  File["bits"] = S->Img.is64Bit() ? 64 : 32;
  File["entry"] = vaHex(S->Img.Entry);
  File["base"] = vaHex(S->Img.Base);
  File["endian"] = "LE";
  File["size"] = static_cast<int64_t>(S->inputFileSize());
  Root["file"] = std::move(File);

  llvm::json::Object Hashes;
  std::ifstream Ifs;
  if (S->MemoryInputBytes) {
    if (S->Img.InputFileSHA256)
      Hashes["sha256"] = llvm::toHex(*S->Img.InputFileSHA256, true);
  } else {
    Ifs.open(S->FilePath, std::ios::binary);
  }
  if (Ifs.is_open()) {
    llvm::MD5 Md5;
    uint32_t Crc = 0;
    char Buf[8192];
    while (Ifs.read(Buf, sizeof(Buf)) || Ifs.gcount() > 0) {
      auto Count = static_cast<size_t>(Ifs.gcount());
      const llvm::ArrayRef<uint8_t> Chunk(
          reinterpret_cast<const uint8_t *>(Buf), Count);
      Md5.update(Chunk);
      Crc = llvm::crc32(Crc, Chunk);
    }
    llvm::MD5::MD5Result Md5Res;
    Md5.final(Md5Res);
    Hashes["md5"] = Md5Res.digest().str().lower();
    Hashes["sha256"] = inputSha256(*S);
    Hashes["crc32"] = llvm::utohexstr(Crc, /*LowerCase=*/true, /*Width=*/8);
  }
  Root["hashes"] = std::move(Hashes);

  llvm::json::Object Counts;
  Counts["functions"] = static_cast<int64_t>(S->Functions.size());
  Counts["imports"] = static_cast<int64_t>(S->Img.Imports.size());
  Counts["exports"] = static_cast<int64_t>(S->Img.Exports.size());
  int64_t StringCount = 0;
  for (const auto &Seg : S->Img.Segments) {
    if (Seg.isExecutable())
      continue;
    const uint8_t *Data = Seg.Data.data();
    size_t Len = Seg.Data.size();
    size_t RunLen = 0;
    for (size_t I = 0; I <= Len; ++I) {
      uint8_t B = (I < Len) ? Data[I] : 0;
      bool IsPrintable = (B >= 0x20 && B < 0x7F) || B == '\t' || B == '\n';
      if (IsPrintable) {
        ++RunLen;
      } else {
        if (RunLen >= 4 && B == 0)
          ++StringCount;
        RunLen = 0;
      }
    }
  }
  Counts["strings"] = StringCount;
  Counts["symbols"] = static_cast<int64_t>(S->Img.Symbols.size());
  Counts["segments"] = static_cast<int64_t>(S->Img.Segments.size());
  Counts["sections"] = static_cast<int64_t>(S->Img.Sections.size());
  Counts["relocations"] = static_cast<int64_t>(S->Img.Relocations.size());
  Counts["entrypoints"] = static_cast<int64_t>(
      (hasMainEntryPoint(S->Img) ? 1 : 0) + S->Img.DynInfo.PreinitArray.size() +
      S->Img.DynInfo.InitArray.size() + S->Img.DynInfo.FiniArray.size() +
      (S->Img.DynInfo.InitAddr ? 1 : 0) + (S->Img.DynInfo.FiniAddr ? 1 : 0));
  Root["counts"] = std::move(Counts);

  llvm::json::Array Libs;
  for (const auto &Lib : S->Img.DynInfo.NeededLibs)
    Libs.push_back(Lib);
  Root["libraries"] = std::move(Libs);

  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

// ===--------------------------------------------------------------------===//
// Address resolution
// ===--------------------------------------------------------------------===//

const char *neverd_resolve_addr(neverd_session_t Sess, neverd_va_t Addr) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return nullptr;
  (void)S->synchronizeFunctions();

  llvm::json::Object Obj;

  Obj = S->functionIdentity(Addr);
  if (!Obj.empty()) {
    Obj["type"] = "function";
    return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
  }

  if (const Import *Imp = S->Img.findImportAt(Addr)) {
    Obj["type"] = "import";
    Obj["name"] = jsonSafeText(Imp->Name);
    Obj["module"] = jsonSafeText(Imp->Module);
    Obj["addr"] = vaHex(Addr);
    return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
  }

  for (const auto &Exp : S->Img.Exports) {
    if (Exp.Addr == Addr) {
      Obj["type"] = "export";
      Obj["name"] = jsonSafeText(Exp.Name);
      Obj["addr"] = vaHex(Addr);
      return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
    }
  }

  for (const auto &Seg : S->Img.Segments) {
    if (!Seg.contains(Addr))
      continue;
    if (Seg.isExecutable())
      continue;
    uint64_t Off64 = Addr - Seg.VA;
    if (Off64 < Seg.Data.size()) {
      size_t Off = static_cast<size_t>(Off64);
      const uint8_t *P = Seg.Data.data() + Off;
      size_t Remain = Seg.Data.size() - Off;
      size_t Len = 0;
      while (Len < Remain && Len < 256 && P[Len] >= 0x20 && P[Len] < 0x7f)
        ++Len;
      if (Len >= 4 && (Len >= Remain || P[Len] == 0)) {
        Obj["type"] = "string";
        Obj["value"] = std::string(reinterpret_cast<const char *>(P), Len);
        Obj["addr"] = vaHex(Addr);
        return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
      }
    }
  }

  return nullptr;
}
