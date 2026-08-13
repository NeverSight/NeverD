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

#include "SessionImpl.h"

#include "neverd/evm/bytecode/EVMBytecode.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/SHA256.h"

#include <fstream>

using namespace neverd;
using namespace neverd::sdk;

namespace {

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

// ===--------------------------------------------------------------------===//
// Info panels (JSON)
// ===--------------------------------------------------------------------===//

const char *neverd_imports_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Imp : S->Img.Imports) {
    llvm::json::Object Obj;
    Obj["module"] = Imp.Module;
    Obj["name"] = Imp.Name;
    Obj["ordinal"] = static_cast<int64_t>(Imp.Ordinal);
    Obj["iat_addr"] = vaHex(Imp.IATAddr);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_exports_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr(std::string("[]"));

  llvm::json::Array Arr;
  for (const auto &Exp : S->Img.Exports) {
    llvm::json::Object Obj;
    Obj["name"] = Exp.Name;
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
    Obj["name"] = Seg.Name;
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
  llvm::json::Array Arr;

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
        if (RunLen >= MinLen && B == 0) {
          va_t StrAddr = Seg.VA + RunStart;
          llvm::json::Object Obj;
          Obj["addr"] = vaHex(StrAddr);
          Obj["value"] = std::string(
              reinterpret_cast<const char *>(Data + RunStart), RunLen);
          Obj["length"] = static_cast<int64_t>(RunLen);
          Arr.push_back(std::move(Obj));
        }
        RunLen = 0;
      }
    }
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
}

const char *neverd_sections_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Sec : S->Img.Sections) {
    llvm::json::Object Obj;
    Obj["name"] = Sec.Name;
    Obj["segment"] = Sec.SegmentName;
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

const char *neverd_symbols_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("[]");

  llvm::json::Array Arr;
  for (const auto &Sym : S->Img.Symbols) {
    llvm::json::Object Obj;
    Obj["name"] = Sym.Name;
    Obj["addr"] = vaHex(Sym.Addr);
    Obj["size"] = static_cast<int64_t>(Sym.Size);
    Arr.push_back(std::move(Obj));
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Arr))));
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
    Obj["symbol"] = Rel.SymbolName;
    Obj["section"] = Rel.SectionName;
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
  Root["format"] = S->Img.getFormatName();
  Root["bits"] = S->Img.is64Bit() ? 64 : 32;
  Root["file_path"] = S->FilePath.string();

  std::error_code EC;
  auto FileSz = std::filesystem::file_size(S->FilePath, EC);
  Root["file_size"] = EC ? 0 : static_cast<int64_t>(FileSz);

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
                      const std::string &Name) {
    if (!Addr)
      return;
    llvm::json::Object O;
    O["type"] = Type;
    O["addr"] = vaHex(Addr);
    O["name"] = Name;
    Arr.push_back(std::move(O));
  };

  addEntry("entry", Img.Entry, Img.getFunctionNameAt(Img.Entry));
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

const char *neverd_dashboard_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->Loaded)
    return dupStr("{}");
  (void)S->synchronizeFunctions();

  llvm::json::Object Root;

  llvm::json::Object File;
  File["path"] = S->FilePath.string();
  File["name"] = S->FilePath.filename().string();
  File["format"] = S->Img.getFormatName();
  File["arch"] = getArchName(S->Img.Arch);
  File["bits"] = S->Img.is64Bit() ? 64 : 32;
  File["entry"] = vaHex(S->Img.Entry);
  File["base"] = vaHex(S->Img.Base);
  File["endian"] = "LE";
  std::error_code EC;
  auto FileSz = std::filesystem::file_size(S->FilePath, EC);
  File["size"] = EC ? 0 : static_cast<int64_t>(FileSz);
  Root["file"] = std::move(File);

  llvm::json::Object Hashes;
  std::ifstream Ifs(S->FilePath, std::ios::binary);
  if (Ifs.is_open()) {
    llvm::MD5 Md5;
    llvm::SHA256 Sha;
    char Buf[8192];
    while (Ifs.read(Buf, sizeof(Buf)) || Ifs.gcount() > 0) {
      auto Count = static_cast<size_t>(Ifs.gcount());
      Md5.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Buf),
                                         Count));
      Sha.update(llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Buf),
                                         Count));
    }
    llvm::MD5::MD5Result Md5Res;
    Md5.final(Md5Res);
    Hashes["md5"] = Md5Res.digest().str().lower();
    auto Sha256Res = Sha.final();
    std::string Sha256Hex;
    const char Digits[] = "0123456789abcdef";
    for (auto B : Sha256Res) {
      Sha256Hex += Digits[(B >> 4) & 0xF];
      Sha256Hex += Digits[B & 0xF];
    }
    Hashes["sha256"] = Sha256Hex;
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
      (S->Img.Entry ? 1 : 0) + S->Img.DynInfo.PreinitArray.size() +
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

  for (const auto &F : S->Functions) {
    if (F.Entry == Addr) {
      Obj["type"] = "function";
      Obj["name"] = F.Name;
      Obj["addr"] = vaHex(F.Entry);
      return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
    }
  }

  if (const Import *Imp = S->Img.findImportAt(Addr)) {
    Obj["type"] = "import";
    Obj["name"] = Imp->Name;
    Obj["module"] = Imp->Module;
    Obj["addr"] = vaHex(Addr);
    return dupStr(jsonToString(llvm::json::Value(std::move(Obj))));
  }

  for (const auto &Exp : S->Img.Exports) {
    if (Exp.Addr == Addr) {
      Obj["type"] = "export";
      Obj["name"] = Exp.Name;
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
