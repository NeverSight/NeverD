//===- MetadataJSONTests.cpp - public metadata JSON regressions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/sdk/NeverDCAPISafety.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace llvm;
using namespace llvm::COFF;
using namespace llvm::object;

constexpr size_t kDOSOffset = 0;
constexpr size_t kPEOffset = 0x80;
constexpr size_t kTextFileOffset = 0x200;
constexpr size_t kDataFileOffset = 0x400;
constexpr uint32_t kTextRVA = 0x1000;
constexpr uint32_t kDataRVA = 0x2000;

template <typename T>
void writeObject(std::vector<uint8_t> &Bytes, size_t Offset, const T &Object) {
  ASSERT_LE(Offset, Bytes.size());
  ASSERT_LE(sizeof(T), Bytes.size() - Offset);
  std::memcpy(Bytes.data() + Offset, &Object, sizeof(T));
}

void writeBytes(std::vector<uint8_t> &Bytes, size_t Offset,
                std::initializer_list<uint8_t> Values) {
  ASSERT_LE(Offset, Bytes.size());
  ASSERT_LE(Values.size(), Bytes.size() - Offset);
  std::copy(Values.begin(), Values.end(), Bytes.begin() + Offset);
}

std::vector<uint8_t> makeMalformedMetadataPE() {
  constexpr size_t kDirectoryCount = 16;
  constexpr size_t kFileHeaderOffset = kPEOffset + 4;
  constexpr size_t kOptionalHeaderOffset =
      kFileHeaderOffset + sizeof(coff_file_header);
  constexpr size_t kDirectoriesOffset =
      kOptionalHeaderOffset + sizeof(pe32plus_header);
  constexpr size_t kSectionHeadersOffset =
      kDirectoriesOffset + kDirectoryCount * sizeof(data_directory);

  std::vector<uint8_t> Bytes(0x600, 0);

  dos_header DOS{};
  DOS.Magic[0] = 'M';
  DOS.Magic[1] = 'Z';
  DOS.AddressOfNewExeHeader = kPEOffset;
  writeObject(Bytes, kDOSOffset, DOS);
  writeBytes(Bytes, kPEOffset, {'P', 'E', 0, 0});

  coff_file_header File{};
  File.Machine = IMAGE_FILE_MACHINE_AMD64;
  File.NumberOfSections = 2;
  File.SizeOfOptionalHeader =
      sizeof(pe32plus_header) + kDirectoryCount * sizeof(data_directory);
  File.Characteristics =
      IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
  writeObject(Bytes, kFileHeaderOffset, File);

  pe32plus_header Optional{};
  Optional.Magic = PE32Header::PE32_PLUS;
  Optional.SizeOfCode = 0x200;
  Optional.SizeOfInitializedData = 0x200;
  Optional.AddressOfEntryPoint = kTextRVA;
  Optional.BaseOfCode = kTextRVA;
  Optional.ImageBase = 0x140000000ULL;
  Optional.SectionAlignment = 0x1000;
  Optional.FileAlignment = 0x200;
  Optional.MajorOperatingSystemVersion = 6;
  Optional.MajorSubsystemVersion = 6;
  Optional.SizeOfImage = 0x3000;
  Optional.SizeOfHeaders = 0x200;
  Optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
  Optional.SizeOfStackReserve = 0x100000;
  Optional.SizeOfStackCommit = 0x1000;
  Optional.SizeOfHeapReserve = 0x100000;
  Optional.SizeOfHeapCommit = 0x1000;
  Optional.NumberOfRvaAndSize = kDirectoryCount;
  writeObject(Bytes, kOptionalHeaderOffset, Optional);

  std::array<data_directory, kDirectoryCount> Directories{};
  Directories[EXPORT_TABLE].RelativeVirtualAddress = kDataRVA + 0x90;
  Directories[EXPORT_TABLE].Size = 0x70;
  Directories[IMPORT_TABLE].RelativeVirtualAddress = kDataRVA;
  Directories[IMPORT_TABLE].Size =
      2 * sizeof(coff_import_directory_table_entry);
  for (size_t I = 0; I < Directories.size(); ++I)
    writeObject(Bytes, kDirectoriesOffset + I * sizeof(data_directory),
                Directories[I]);

  coff_section Text{};
  std::memcpy(Text.Name, ".text", 5);
  Text.VirtualSize = 1;
  Text.VirtualAddress = kTextRVA;
  Text.SizeOfRawData = 0x200;
  Text.PointerToRawData = kTextFileOffset;
  Text.Characteristics =
      IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
  writeObject(Bytes, kSectionHeadersOffset, Text);

  coff_section Data{};
  Data.Name[0] = '.';
  Data.Name[1] = 'r';
  Data.Name[2] = static_cast<char>(0xff);
  Data.Name[3] = 'd';
  Data.VirtualSize = 0x200;
  Data.VirtualAddress = kDataRVA;
  Data.SizeOfRawData = 0x200;
  Data.PointerToRawData = kDataFileOffset;
  Data.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  writeObject(Bytes, kSectionHeadersOffset + sizeof(coff_section), Data);

  Bytes[kTextFileOffset] = 0xc3; // ret

  coff_import_directory_table_entry Import{};
  Import.ImportLookupTableRVA = kDataRVA + 0x40;
  Import.NameRVA = kDataRVA + 0x60;
  Import.ImportAddressTableRVA = kDataRVA + 0x50;
  writeObject(Bytes, kDataFileOffset, Import);
  const uint64_t HintNameRVA = kDataRVA + 0x70;
  writeObject(Bytes, kDataFileOffset + 0x40, HintNameRVA);
  writeObject(Bytes, kDataFileOffset + 0x50, HintNameRVA);
  writeBytes(Bytes, kDataFileOffset + 0x60, {'m', 0xff, '.', 'd', 'l', 'l', 0});
  writeBytes(Bytes, kDataFileOffset + 0x70, {0, 0, 'f', 0xc3, '(', 0});

  export_directory_table_entry ExportDirectory{};
  ExportDirectory.NameRVA = kDataRVA + 0xe0;
  ExportDirectory.OrdinalBase = 1;
  ExportDirectory.AddressTableEntries = 1;
  ExportDirectory.NumberOfNamePointers = 1;
  ExportDirectory.ExportAddressTableRVA = kDataRVA + 0xc0;
  ExportDirectory.NamePointerRVA = kDataRVA + 0xc4;
  ExportDirectory.OrdinalTableRVA = kDataRVA + 0xc8;
  writeObject(Bytes, kDataFileOffset + 0x90, ExportDirectory);
  const uint32_t ExportRVA = kTextRVA;
  const uint32_t ExportNameRVA = kDataRVA + 0xd0;
  const uint16_t ExportOrdinal = 0;
  writeObject(Bytes, kDataFileOffset + 0xc0, ExportRVA);
  writeObject(Bytes, kDataFileOffset + 0xc4, ExportNameRVA);
  writeObject(Bytes, kDataFileOffset + 0xc8, ExportOrdinal);
  writeBytes(Bytes, kDataFileOffset + 0xd0, {'e', 0x80, 0});
  writeBytes(Bytes, kDataFileOffset + 0xe0,
             {'f', 'i', 'x', 't', 'u', 'r', 'e', 0});
  return Bytes;
}

std::vector<uint8_t> makeInternalCallPE() {
  constexpr size_t kDirectoryCount = 16;
  constexpr size_t kFileHeaderOffset = kPEOffset + 4;
  constexpr size_t kOptionalHeaderOffset =
      kFileHeaderOffset + sizeof(coff_file_header);
  constexpr size_t kDirectoriesOffset =
      kOptionalHeaderOffset + sizeof(pe32plus_header);
  constexpr size_t kSectionHeadersOffset =
      kDirectoriesOffset + kDirectoryCount * sizeof(data_directory);

  std::vector<uint8_t> Bytes = makeMalformedMetadataPE();
  coff_section Text{};
  std::memcpy(&Text, Bytes.data() + kSectionHeadersOffset, sizeof(Text));
  Text.VirtualSize = 0x10;
  writeObject(Bytes, kSectionHeadersOffset, Text);

  // entry: call helper; ret
  // helper: mov eax, 42; ret
  writeBytes(Bytes, kTextFileOffset,
             {0xe8, 0x03, 0x00, 0x00, 0x00, 0xc3, 0x90, 0x90, 0xb8, 0x2a, 0x00,
              0x00, 0x00, 0xc3});
  return Bytes;
}

std::vector<uint8_t> makePartiallyTruncatedInternalCallPE() {
  constexpr size_t kDirectoryCount = 16;
  constexpr size_t kFileHeaderOffset = kPEOffset + 4;
  constexpr size_t kOptionalHeaderOffset =
      kFileHeaderOffset + sizeof(coff_file_header);
  constexpr size_t kDirectoriesOffset =
      kOptionalHeaderOffset + sizeof(pe32plus_header);
  constexpr size_t kSectionHeadersOffset =
      kDirectoriesOffset + kDirectoryCount * sizeof(data_directory);

  std::vector<uint8_t> Bytes = makeInternalCallPE();
  coff_section Text{};
  std::memcpy(&Text, Bytes.data() + kSectionHeadersOffset, sizeof(Text));
  Text.VirtualSize = 9;
  writeObject(Bytes, kSectionHeadersOffset, Text);
  Bytes[kTextFileOffset + 8] = 0x0f;
  return Bytes;
}

class TemporaryPE {
public:
  explicit TemporaryPE(const std::vector<uint8_t> &Bytes) {
    Error = sys::fs::createTemporaryFile("neverd-metadata-json", "exe", Path);
    if (Error)
      return;
    std::ofstream Output(Path.str().str(), std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
    if (!Output)
      Error = std::make_error_code(std::errc::io_error);
  }

  ~TemporaryPE() {
    if (!Path.empty())
      sys::fs::remove(Path);
  }

  std::string path() const { return Path.str().str(); }
  const std::error_code &error() const { return Error; }

private:
  SmallString<128> Path;
  std::error_code Error;
};

class SessionGuard {
public:
  SessionGuard() : Session(neverd_session_create()) {}
  ~SessionGuard() { neverd_session_destroy(Session); }
  operator neverd_session_t() const { return Session; }

private:
  neverd_session_t Session;
};

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Result(Value);
  neverd_free_string(Value);
  return Result;
}

void expectParseableJSON(const char *Name, const char *Value) {
  ASSERT_NE(Value, nullptr) << Name;
  std::string JSON = takeString(Value);
  auto Parsed = json::parse(JSON);
  ASSERT_TRUE(static_cast<bool>(Parsed))
      << Name << ": " << toString(Parsed.takeError()) << "\n"
      << JSON;
}

json::Object runBench(const std::vector<uint8_t> &Bytes, int MaxFunctions = 0) {
  TemporaryPE Input(Bytes);
  EXPECT_FALSE(Input.error()) << Input.error().message();

  SessionGuard Session;
  EXPECT_TRUE(neverd_session_load(Session, Input.path().c_str()))
      << takeString(neverd_last_error(Session));

  const char *Value =
      neverd_bench_run(Session, Input.path().c_str(), MaxFunctions);
  EXPECT_NE(Value, nullptr) << takeString(neverd_last_error(Session));
  if (!Value)
    return {};
  std::string JSON = takeString(Value);
  auto Parsed = json::parse(JSON);
  if (!Parsed) {
    ADD_FAILURE() << toString(Parsed.takeError()) << "\n" << JSON;
    return {};
  }
  if (!Parsed->getAsObject()) {
    ADD_FAILURE() << "benchmark JSON root is not an object\n" << JSON;
    return {};
  }
  return std::move(*Parsed->getAsObject());
}

} // namespace

TEST(NeverDMetadataJSON, MalformedPENamesRemainParseable) {
  TemporaryPE Input(makeMalformedMetadataPE());
  ASSERT_FALSE(Input.error()) << Input.error().message();

  SessionGuard Session;
  ASSERT_TRUE(neverd_session_load(Session, Input.path().c_str()))
      << takeString(neverd_last_error(Session));

  expectParseableJSON("headers", neverd_headers_json(Session));
  expectParseableJSON("imports", neverd_imports_json(Session));
  expectParseableJSON("exports", neverd_exports_json(Session));
  expectParseableJSON("segments", neverd_segments_json(Session));
  expectParseableJSON("sections", neverd_sections_json(Session));
  expectParseableJSON("symbols", neverd_symbols_json(Session));
  expectParseableJSON("relocations", neverd_relocs_json(Session));
}

TEST(NeverDMetadataJSON, CoverageReportReconcilesEveryPipelineStage) {
  json::Object Root = runBench(makeMalformedMetadataPE());
  const json::Object *Audit = Root.getObject("audit");
  ASSERT_NE(Audit, nullptr);
  EXPECT_EQ(Audit->getBoolean("complete"), true);
  EXPECT_EQ(Audit->getInteger("candidate_functions"), 1);
  EXPECT_EQ(Audit->getInteger("accepted_functions"), 1);
  EXPECT_EQ(Audit->getInteger("rejected_functions"), 0);
  EXPECT_EQ(Audit->getInteger("decoded_instructions"), 1);
  EXPECT_EQ(Audit->getInteger("lifted_instructions"), 1);
  EXPECT_EQ(Audit->getInteger("decode_failures"), 0);
  EXPECT_EQ(Audit->getInteger("unsupported_instructions"), 0);
  EXPECT_EQ(Audit->getInteger("truncated_paths"), 0);
  EXPECT_EQ(Audit->getInteger("med_failures"), 0);
  EXPECT_EQ(Audit->getInteger("med_ir_verifier_failures"), 0);
  EXPECT_EQ(Audit->getInteger("backend_unhandled_value_intrinsics"), 0);
  EXPECT_EQ(Audit->getInteger("unresolved_internal_calls"), 0);
  EXPECT_EQ(Audit->getBoolean("llvm_verifier_failed"), false);

  const json::Array *Functions = Root.getArray("audit_functions");
  const json::Array *Definitions = Root.getArray("llvm_definitions");
  ASSERT_NE(Functions, nullptr);
  ASSERT_NE(Definitions, nullptr);
  ASSERT_EQ(Functions->size(), 1u);
  ASSERT_EQ(Definitions->size(), 1u);
  const json::Object *Function = (*Functions)[0].getAsObject();
  ASSERT_NE(Function, nullptr);
  EXPECT_EQ(Function->getString("disposition"), "accepted");
  EXPECT_EQ(Function->getBoolean("low_ir"), true);
  EXPECT_EQ(Function->getBoolean("med_ir"), true);
  EXPECT_EQ(Function->getBoolean("med_ir_verified"), true);
  EXPECT_EQ(Function->getBoolean("llvm_definition"), true);
}

TEST(NeverDMetadataJSON, CoverageReportExposesReachableDecodeFailure) {
  auto Bytes = makeMalformedMetadataPE();
  Bytes[kTextFileOffset] = 0x0f; // truncated two-byte opcode

  json::Object Root = runBench(Bytes);
  const json::Object *Audit = Root.getObject("audit");
  ASSERT_NE(Audit, nullptr);
  EXPECT_EQ(Audit->getBoolean("complete"), false);
  EXPECT_EQ(Audit->getInteger("decode_failures"), 1);
  EXPECT_EQ(Audit->getInteger("rejected_functions"), 1);
}

TEST(NeverDMetadataJSON, CoverageReportRejectsMissingInternalCallee) {
  json::Object LimitedRoot = runBench(makeInternalCallPE(), 1);
  const json::Object *LimitedAudit = LimitedRoot.getObject("audit");
  ASSERT_NE(LimitedAudit, nullptr);
  EXPECT_EQ(LimitedAudit->getBoolean("complete"), false);
  EXPECT_EQ(LimitedAudit->getInteger("detected_functions"), 2);
  EXPECT_EQ(LimitedAudit->getInteger("skipped_functions"), 1);
  EXPECT_EQ(LimitedAudit->getInteger("unresolved_internal_calls"), 1);

  json::Object FullRoot = runBench(makeInternalCallPE());
  const json::Object *FullAudit = FullRoot.getObject("audit");
  ASSERT_NE(FullAudit, nullptr);
  EXPECT_EQ(FullAudit->getBoolean("complete"), true);
  EXPECT_EQ(FullAudit->getInteger("accepted_functions"), 2);
  EXPECT_EQ(FullAudit->getInteger("unresolved_internal_calls"), 0);
}

TEST(NeverDMetadataJSON, SafetyRejectsPartiallyLiftedFunctionInventory) {
  const std::vector<uint8_t> Bytes = makePartiallyTruncatedInternalCallPE();
  json::Object Coverage = runBench(Bytes);
  const json::Object *Audit = Coverage.getObject("audit");
  ASSERT_NE(Audit, nullptr);
  EXPECT_EQ(Audit->getInteger("detected_functions"), 1);
  EXPECT_EQ(Audit->getInteger("accepted_functions"), 1);
  EXPECT_EQ(Audit->getInteger("rejected_functions"), 0);
  EXPECT_EQ(Audit->getInteger("unresolved_internal_calls"), 1);
  EXPECT_EQ(Audit->getBoolean("complete"), false);

  TemporaryPE Input(Bytes);
  ASSERT_FALSE(Input.error()) << Input.error().message();

  SessionGuard Session;
  ASSERT_TRUE(neverd_session_load(Session, Input.path().c_str()))
      << takeString(neverd_last_error(Session));
  neverd_safety_options Options{};
  Options.struct_size = sizeof(Options);
  for (auto Analyze : {neverd_session_hunt_json, neverd_session_audit_json}) {
    const std::string JSON = takeString(Analyze(Session, &Options));
    auto Parsed = json::parse(JSON);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << toString(Parsed.takeError()) << "\n"
        << JSON;
    const json::Object *Root = Parsed->getAsObject();
    ASSERT_NE(Root, nullptr);
    EXPECT_FALSE(Root->getBoolean("ok").value_or(true));
    EXPECT_EQ(Root->getString("verdict"), "UNKNOWN");
    EXPECT_EQ(Root->getString("confidence"), "LOW");
    ASSERT_TRUE(Root->getString("error").has_value());
    EXPECT_NE(Root->getString("error")->find("incomplete safety lift"),
              StringRef::npos);
  }
}
