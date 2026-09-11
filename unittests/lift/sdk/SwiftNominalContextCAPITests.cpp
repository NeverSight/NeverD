//===- SwiftNominalContextCAPITests.cpp - public Swift source contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kBase = 0x100000000ULL;
constexpr uint32_t kAccessor = 0x400;
constexpr uint32_t kGlobal = 0x420;
constexpr uint32_t kDescriptor = 0x1100;
constexpr uint32_t kMetadata = 0x1400;
constexpr uint32_t kValueWitnesses = 0x1600;
constexpr char kAccessorEntry[] = "0x100000400";
constexpr char kGlobalEntry[] = "0x100000420";
constexpr char kAccessorSymbol[] = "_$s4Demo5EmptyVMa";
constexpr char kGlobalSymbol[] = "_$s4Demo5EmptySiyF";
constexpr char kMemberSymbol[] = "_$s4Demo5EmptyV5valueSiyFZ";
constexpr char kSource[] = "struct `Empty` {\n}\n";

enum class Damage {
  None,
  MissingAccessor,
  OtherAccessor,
  OtherMetadataReturn,
  IncompleteState,
  MissingMetadata,
  MismatchedFieldCount,
  MissingValueWitnesses,
  TruncatedValueWitnesses,
  NonemptyStorage,
  ZeroStride,
  OveralignedStorage,
  NontrivialValues,
  NoncopyableValues,
  ExtraInhabitants,
  UnknownValueFlags
};

template <typename T>
void writeObject(std::vector<uint8_t> &Bytes, size_t Offset, const T &Object) {
  ASSERT_LE(Offset, Bytes.size());
  ASSERT_LE(sizeof(T), Bytes.size() - Offset);
  std::memcpy(Bytes.data() + Offset, &Object, sizeof(Object));
}

void setName(char (&Destination)[16], llvm::StringRef Name) {
  ASSERT_LT(Name.size(), sizeof(Destination));
  std::memcpy(Destination, Name.data(), Name.size());
}

// Owned linked Mach-O bytes, not captured Swift compiler output. There are
// no internal BinaryImage, Session, LowIR or runtime-proof test doubles.
std::vector<uint8_t> makeMachO(bool Arm64, Damage Fault, bool WithGlobal,
                               bool WithMember) {
  using namespace llvm::MachO;
  constexpr uint32_t TextCommandSize =
      sizeof(segment_command_64) + sizeof(section_64);
  constexpr uint32_t DataCommandSize =
      sizeof(segment_command_64) + 2 * sizeof(section_64);
  constexpr uint32_t CommandSize =
      TextCommandSize + DataCommandSize + sizeof(segment_command_64) +
      sizeof(symtab_command) + sizeof(entry_point_command);
  static_assert(sizeof(mach_header_64) + CommandSize < kAccessor);
  std::vector<uint8_t> Bytes(0x2200, 0);
  auto U32 = [&](uint32_t Offset, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Value);
  };
  auto U64 = [&](uint32_t Offset, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
  auto Relative = [&](uint32_t Offset, uint32_t Target) {
    U32(Offset, Target - Offset);
  };
  auto Text = [&](uint32_t Offset, llvm::StringRef Value) {
    std::memcpy(Bytes.data() + Offset, Value.data(), Value.size());
  };
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = Arm64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
  Header.cpusubtype = Arm64 ? CPU_SUBTYPE_ARM64_ALL : CPU_SUBTYPE_X86_64_ALL;
  Header.filetype = MH_EXECUTE;
  Header.ncmds = 5;
  Header.sizeofcmds = CommandSize;
  Header.flags = MH_NOUNDEFS;
  writeObject(Bytes, 0, Header);

  size_t Command = sizeof(Header);
  segment_command_64 Segment{};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = TextCommandSize;
  setName(Segment.segname, "__TEXT");
  Segment.vmaddr = kBase;
  Segment.vmsize = Segment.filesize = 0x1000;
  Segment.maxprot = Segment.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment.nsects = 1;
  writeObject(Bytes, Command, Segment);
  section_64 Section{};
  setName(Section.sectname, "__text");
  setName(Section.segname, "__TEXT");
  Section.addr = kBase + kAccessor;
  Section.size = WithGlobal ? 0x28 : 0x10;
  Section.offset = kAccessor;
  Section.align = 2;
  Section.flags = static_cast<uint32_t>(S_REGULAR) |
                  static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS);
  writeObject(Bytes, Command + sizeof(Segment), Section);
  Command += TextCommandSize;

  Segment = {};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = DataCommandSize;
  setName(Segment.segname, "__DATA");
  Segment.vmaddr = kBase + 0x1000;
  Segment.fileoff = 0x1000;
  Segment.vmsize = Segment.filesize = 0x1000;
  Segment.maxprot = Segment.initprot = VM_PROT_READ | VM_PROT_WRITE;
  Segment.nsects = 2;
  writeObject(Bytes, Command, Segment);
  Section = {};
  setName(Section.sectname, "__swift5_types");
  setName(Section.segname, "__DATA");
  Section.addr = kBase + 0x1000;
  Section.size = 4;
  Section.offset = 0x1000;
  Section.align = 2;
  writeObject(Bytes, Command + sizeof(Segment), Section);
  Section = {};
  setName(Section.sectname, "__data");
  setName(Section.segname, "__DATA");
  Section.addr = kBase + 0x1004;
  Section.size = 0xffc;
  Section.offset = 0x1004;
  Section.align = 2;
  writeObject(Bytes, Command + sizeof(Segment) + sizeof(Section), Section);
  Command += DataCommandSize;

  Segment = {};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = sizeof(Segment);
  setName(Segment.segname, "__LINKEDIT");
  Segment.vmaddr = kBase + 0x2000;
  Segment.fileoff = 0x2000;
  Segment.vmsize = Segment.filesize = 0x200;
  Segment.maxprot = Segment.initprot = VM_PROT_READ;
  writeObject(Bytes, Command, Segment);
  Command += sizeof(Segment);

  std::string Strings(1, '\0');
  auto Symbol = [&](unsigned Index, llvm::StringRef Name, uint8_t Ordinal,
                    uint32_t Offset) {
    nlist_64 Entry{};
    Entry.n_strx = static_cast<uint32_t>(Strings.size());
    Strings.append(Name.data(), Name.size());
    Strings.push_back('\0');
    Entry.n_type = static_cast<uint8_t>(N_SECT) | static_cast<uint8_t>(N_EXT);
    Entry.n_sect = Ordinal;
    Entry.n_value = kBase + Offset;
    writeObject(Bytes, 0x2000 + Index * sizeof(Entry), Entry);
  };
  Symbol(0, kAccessorSymbol, 1, kAccessor);
  Symbol(1, "_$s4Demo5EmptyVN", 3, kMetadata);
  if (WithGlobal)
    Symbol(2, WithMember ? kMemberSymbol : kGlobalSymbol, 1, kGlobal);
  symtab_command Symbols{};
  Symbols.cmd = LC_SYMTAB;
  Symbols.cmdsize = sizeof(Symbols);
  Symbols.symoff = 0x2000;
  Symbols.nsyms = WithGlobal ? 3 : 2;
  Symbols.stroff = 0x2080;
  Symbols.strsize = static_cast<uint32_t>(Strings.size());
  writeObject(Bytes, Command, Symbols);
  Text(Symbols.stroff, Strings);
  Command += sizeof(Symbols);
  entry_point_command Main{};
  Main.cmd = LC_MAIN;
  Main.cmdsize = sizeof(Main);
  Main.entryoff = kAccessor;
  writeObject(Bytes, Command, Main);

  Relative(0x1000, kDescriptor);
  U32(kDescriptor, 0x51); // Non-generic struct in a module context.
  Relative(kDescriptor + 4, 0x1200);
  Relative(kDescriptor + 8, 0x1180);
  if (Fault != Damage::MissingAccessor)
    Relative(kDescriptor + 12,
             kAccessor + (Fault == Damage::OtherAccessor ? 4 : 0));
  Relative(kDescriptor + 16, 0x1300);
  Text(0x1180, "Empty");
  Relative(0x1208, 0x1210);
  Text(0x1210, "Demo");
  U32(0x1308, 12u << 16); // Struct field descriptor; zero stored fields.
  U32(0x130c, Fault == Damage::MismatchedFieldCount ? 1 : 0);
  U64(kMetadata, Fault == Damage::MissingMetadata ? 0 : 0x200);
  U64(kMetadata + 8, kBase + kDescriptor);

  // The value-witness layout is an independent, file-backed ABI declaration.
  // Witness-function slots are not executed or used as native-body evidence.
  uint64_t WitnessAddress = kBase + kValueWitnesses;
  uint64_t ValueSize = 0;
  uint64_t ValueStride = 1;
  uint32_t ValueFlags = 0;
  uint32_t ExtraInhabitants = 0;
  switch (Fault) {
  case Damage::MissingValueWitnesses:
    WitnessAddress = 0;
    break;
  case Damage::TruncatedValueWitnesses:
    WitnessAddress = kBase + 0x1fc0;
    break;
  case Damage::NonemptyStorage:
    ValueSize = 1;
    break;
  case Damage::ZeroStride:
    ValueStride = 0;
    break;
  case Damage::OveralignedStorage:
    ValueStride = 16;
    ValueFlags = 15;
    break;
  case Damage::NontrivialValues:
    ValueFlags = 0x00010000;
    break;
  case Damage::NoncopyableValues:
    ValueFlags = 0x00800000;
    break;
  case Damage::ExtraInhabitants:
    ExtraInhabitants = 1;
    break;
  case Damage::UnknownValueFlags:
    ValueFlags = 0x80000000;
    break;
  default:
    break;
  }
  U64(kMetadata - 8, WitnessAddress);
  U64(kValueWitnesses + 64, ValueSize);
  U64(kValueWitnesses + 72, ValueStride);
  U32(kValueWitnesses + 80, ValueFlags);
  U32(kValueWitnesses + 84, ExtraInhabitants);

  const uint32_t ReturnOffset =
      kMetadata + (Fault == Damage::OtherMetadataReturn ? 8 : 0);
  const uint32_t State = Fault == Damage::IncompleteState ? 1 : 0;
  if (Arm64) {
    // adrp x0, metadata-page; add x0, x0, page-offset; movz x1, state; ret.
    U32(kAccessor, 0xb0000000);
    U32(kAccessor + 4, 0x91000000 | ((ReturnOffset & 0xfff) << 10));
    U32(kAccessor + 8, 0xd2800001 | (State << 5));
    U32(kAccessor + 12, 0xd65f03c0);
    if (WithGlobal) {
      U32(kGlobal, 0xd2800540); // movz x0, #42
      U32(kGlobal + 4, 0xd65f03c0);
    }
  } else {
    // lea rax, [rip + metadata-delta]; mov edx, state; ret.
    Bytes[kAccessor] = 0x48;
    Bytes[kAccessor + 1] = 0x8d;
    Bytes[kAccessor + 2] = 0x05;
    U32(kAccessor + 3, ReturnOffset - (kAccessor + 7));
    Bytes[kAccessor + 7] = 0xba;
    U32(kAccessor + 8, State);
    Bytes[kAccessor + 12] = 0xc3;
    if (WithGlobal) {
      Bytes[kGlobal] = 0xb8; // mov eax, 42
      U32(kGlobal + 1, 42);
      Bytes[kGlobal + 5] = 0xc3;
    }
  }
  return Bytes;
}

llvm::json::Object request(bool Global = false) {
  llvm::json::Object Result{
      {"entry", Global ? kGlobalEntry : kAccessorEntry},
      {"mangled_symbol", Global ? kGlobalSymbol : kAccessorSymbol},
      {"module", "Demo"},
      {"context_kind", Global ? "global" : "struct"},
      {"context_name", Global ? "" : "Empty"},
      {"name", Global ? "Empty" : "typeMetadata"},
      {"declaration_kind", Global ? "function" : "runtime"},
      {"node_kind", Global ? "Function" : "TypeMetadataAccessFunction"},
      {"is_static", false},
      {"is_mutating", false},
      {"is_mutating_known", true},
      {"parameters", llvm::json::Array{}},
      {"labels", llvm::json::Array{}}};
  if (Global) {
    Result["status"] = "supported";
    Result["return_type"] = llvm::json::Object{
        {"kind", "integer"}, {"name", "Int"}, {"bits", 64}, {"signed", true}};
  } else {
    Result["requires_runtime_source_proof"] = true;
    Result["runtime_source_kind"] = "type_metadata_accessor";
    Result["return_type"] =
        llvm::json::Object{{"kind", "void"}, {"name", "Void"}};
  }
  return Result;
}

llvm::json::Object memberRequest() {
  auto Result = request(true);
  Result["mangled_symbol"] = kMemberSymbol;
  Result["context_kind"] = "struct";
  Result["context_name"] = "Empty";
  Result["name"] = "value";
  Result["is_static"] = true;
  return Result;
}

std::string takeString(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

class SwiftNominalContextCAPI : public ::testing::Test {
protected:
  std::filesystem::path Directory;
  neverd_session_t Session = nullptr;

  void SetUp() override {
    static std::atomic<unsigned> Sequence{0};
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned Attempt = 0; Attempt < 100; ++Attempt) {
      auto Candidate = std::filesystem::temp_directory_path() /
                       ("neverd-swift-nominal-" + std::to_string(Stamp) + "-" +
                        std::to_string(Sequence.fetch_add(1)));
      std::error_code Error;
      if (std::filesystem::create_directory(Candidate, Error)) {
        Directory = std::move(Candidate);
        break;
      }
      ASSERT_FALSE(Error) << Error.message();
    }
    ASSERT_FALSE(Directory.empty());
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Error;
      std::filesystem::remove_all(Directory, Error);
      EXPECT_FALSE(Error) << Error.message();
    }
  }

  void load(bool Arm64, Damage Fault = Damage::None, bool WithGlobal = false,
            bool WithMember = false) {
    const auto Bytes = makeMachO(Arm64, Fault, WithGlobal, WithMember);
    const auto Path = (Directory / "owned.macho").string();
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
    Output.close();
    ASSERT_TRUE(Output);
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
        << takeString(neverd_last_error(Session));
    EXPECT_EQ(neverd_session_entry_addr(Session), kBase + kAccessor);
    ASSERT_EQ(neverd_func_count(Session), WithGlobal ? 2 : 1);
    const int Index = neverd_func_find_by_addr(Session, kBase + kAccessor);
    ASSERT_GE(Index, 0);
    EXPECT_EQ(takeString(neverd_func_name(Session, Index)), kAccessorSymbol);
    if (WithGlobal) {
      const int Global = neverd_func_find_by_addr(Session, kBase + kGlobal);
      ASSERT_GE(Global, 0);
      EXPECT_EQ(takeString(neverd_func_name(Session, Global)),
                WithMember ? kMemberSymbol : kGlobalSymbol);
    }
    std::array<unsigned char, 16> Loaded{};
    ASSERT_EQ(neverd_read_bytes(Session, kBase + kAccessor, Loaded.data(),
                                static_cast<int>(Loaded.size())),
              static_cast<int>(Loaded.size()));
    EXPECT_TRUE(
        std::equal(Loaded.begin(), Loaded.end(), Bytes.begin() + kAccessor));
  }

  llvm::json::Value report(llvm::json::Array Methods) {
    std::string Input;
    llvm::raw_string_ostream Stream(Input);
    Stream << llvm::json::Value(llvm::json::Object{
        {"schema_version", 1}, {"methods", std::move(Methods)}});
    const char *Raw = neverd_swift_methods_json(Session, Input.c_str(), 0);
    if (!Raw) {
      ADD_FAILURE() << takeString(neverd_last_error(Session));
      return llvm::json::Object{};
    }
    auto Parsed = llvm::json::parse(takeString(Raw));
    if (!Parsed) {
      ADD_FAILURE() << llvm::toString(Parsed.takeError());
      return llvm::json::Object{};
    }
    return std::move(*Parsed);
  }

  void counts(const llvm::json::Object &Report, int Total, int Bodies,
              int Projections) {
    EXPECT_EQ(Report.getInteger("schema_version"), 1);
    EXPECT_EQ(Report.getString("status"), "success");
    EXPECT_EQ(Report.getInteger("method_count"), Total);
    EXPECT_EQ(Report.getInteger("recovered_method_count"),
              Bodies + Projections);
    EXPECT_EQ(Report.getInteger("source_body_method_count"), Bodies);
    EXPECT_EQ(Report.getInteger("compiler_projection_method_count"),
              Projections);
    EXPECT_EQ(Report.getInteger("unrecovered_method_count"),
              Total - Bodies - Projections);
    EXPECT_EQ(Report.getString("coverage_status"),
              Bodies + Projections == Total ? "recovered" : "unrecovered");
  }

  void identity(const llvm::json::Object &Row, bool Global = false) {
    EXPECT_EQ(Row.getString("entry"), Global ? kGlobalEntry : kAccessorEntry);
    EXPECT_EQ(Row.getString("mangled_symbol"),
              Global ? kGlobalSymbol : kAccessorSymbol);
  }

  void rejected(const llvm::json::Value &Value, llvm::StringRef Reason,
                bool TwoRows = false, bool GlobalFirst = false) {
    const auto *Report = Value.getAsObject();
    ASSERT_NE(Report, nullptr);
    counts(*Report, TwoRows ? 2 : 1, 0, 0);
    EXPECT_EQ(Report->getString("source"), "");
    const auto *Units = Report->getArray("source_units");
    ASSERT_NE(Units, nullptr);
    EXPECT_TRUE(Units->empty());
    const auto *Rows = Report->getArray("methods");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), TwoRows ? 2u : 1u);
    for (size_t I = 0; I < Rows->size(); ++I) {
      const auto *Row = (*Rows)[I].getAsObject();
      ASSERT_NE(Row, nullptr);
      identity(*Row, TwoRows && ((I == 0) == GlobalFirst));
      EXPECT_EQ(Row->getString("status"), "unrecovered");
      EXPECT_EQ(Row->getString("reason"), Reason);
      EXPECT_FALSE(Row->get("source"));
      EXPECT_FALSE(Row->get("source_representation"));
      EXPECT_FALSE(Row->get("compiler_projection_evidence"));
    }
  }
};

TEST_F(SwiftNominalContextCAPI, OwnedAccessorPublishesTypeWithoutOrdinaryBody) {
  for (bool Arm64 : {true, false}) {
    SCOPED_TRACE(Arm64 ? "AArch64" : "x86-64");
    ASSERT_NO_FATAL_FAILURE(load(Arm64));
    const auto Value = report({request()});
    const auto *Report = Value.getAsObject();
    ASSERT_NE(Report, nullptr);
    counts(*Report, 1, 0, 1);
    EXPECT_EQ(Report->getString("source"), std::string(kSource) + "\n");
    const auto *Types = Report->getArray("types");
    ASSERT_NE(Types, nullptr);
    ASSERT_EQ(Types->size(), 1u);
    const auto *Type = (*Types)[0].getAsObject();
    ASSERT_NE(Type, nullptr);
    EXPECT_EQ(Type->getString("module"), "Demo");
    EXPECT_EQ(Type->getString("name"), "Empty");
    EXPECT_EQ(Type->getString("kind"), "struct");
    EXPECT_EQ(Type->getString("status"), "recovered");
    EXPECT_EQ(Type->getString("reason"), "");
    EXPECT_EQ(Type->getInteger("size"), 0);
    EXPECT_EQ(Type->getInteger("alignment"), 1);
    ASSERT_NE(Type->getArray("fields"), nullptr);
    EXPECT_TRUE(Type->getArray("fields")->empty());
    const auto *Rows = Report->getArray("methods");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = (*Rows)[0].getAsObject();
    ASSERT_NE(Row, nullptr);
    identity(*Row);
    EXPECT_EQ(Row->getString("status"), "recovered");
    EXPECT_EQ(Row->getString("source"), kSource);
    EXPECT_EQ(Row->getString("source_representation"),
              "compiler-generated-from-type");
    EXPECT_EQ(Row->getString("compiler_projection_kind"),
              "type_metadata_accessor");
    const auto *Evidence = Row->getArray("compiler_projection_evidence");
    ASSERT_NE(Evidence, nullptr);
    ASSERT_FALSE(Evidence->empty());
    for (const auto &Item : *Evidence) {
      ASSERT_TRUE(Item.getAsString());
      EXPECT_FALSE(Item.getAsString()->empty());
    }
    const auto *Units = Report->getArray("source_units");
    ASSERT_NE(Units, nullptr);
    ASSERT_EQ(Units->size(), 1u);
    const auto *Unit = (*Units)[0].getAsObject();
    ASSERT_NE(Unit, nullptr);
    EXPECT_EQ(Unit->getString("kind"), "type");
    EXPECT_EQ(Unit->getString("module"), "Demo");
    EXPECT_EQ(Unit->getString("name"), "Empty");
    EXPECT_EQ(Unit->getString("source"), kSource);
    const auto *Entries = Unit->getArray("method_entries");
    ASSERT_NE(Entries, nullptr);
    ASSERT_EQ(Entries->size(), 1u);
    EXPECT_EQ((*Entries)[0].getAsString(), kAccessorEntry);
    const auto *IDs = Unit->getArray("method_identities");
    ASSERT_NE(IDs, nullptr);
    ASSERT_EQ(IDs->size(), 1u);
    ASSERT_NE((*IDs)[0].getAsObject(), nullptr);
    identity(*(*IDs)[0].getAsObject());
    EXPECT_EQ(neverd_func_count(Session), 1);
  }
}

TEST_F(SwiftNominalContextCAPI, NativeOwnershipAndCompletePairRemainRequired) {
  for (bool Arm64 : {true, false}) {
    SCOPED_TRACE(Arm64 ? "AArch64" : "x86-64");
    for (const auto &[Fault, Reason] :
         {std::pair{Damage::MissingAccessor,
                    "native descriptor has no metadata accessor reference"},
          std::pair{Damage::OtherAccessor,
                    "native type descriptor does not own this metadata "
                    "accessor"},
          std::pair{Damage::OtherMetadataReturn,
                    "metadata accessor does not return its exact context "
                    "metadata and complete state"},
          std::pair{Damage::IncompleteState,
                    "metadata accessor does not return its exact context "
                    "metadata and complete state"}}) {
      SCOPED_TRACE(static_cast<int>(Fault));
      ASSERT_NO_FATAL_FAILURE(load(Arm64, Fault));
      rejected(report({request()}), Reason);
      EXPECT_EQ(neverd_func_count(Session), 1);
    }
  }
}

TEST_F(SwiftNominalContextCAPI, IncompleteMetadataCannotSeedNominalSource) {
  for (bool Arm64 : {true, false}) {
    SCOPED_TRACE(Arm64 ? "AArch64" : "x86-64");
    for (const auto &[Fault, Reason] :
         {std::pair{Damage::MissingMetadata,
                    "Swift type metadata address is absent or ambiguous"},
          std::pair{Damage::MismatchedFieldCount,
                    "Swift stored-property records disagree with the type "
                    "descriptor"}}) {
      SCOPED_TRACE(static_cast<int>(Fault));
      ASSERT_NO_FATAL_FAILURE(load(Arm64, Fault));
      const auto Value = report({request()});
      rejected(Value, "runtime source context has ambiguous or unsupported "
                      "native metadata");
      const auto *Report = Value.getAsObject();
      ASSERT_NE(Report, nullptr);
      const auto *Types = Report->getArray("types");
      ASSERT_NE(Types, nullptr);
      ASSERT_EQ(Types->size(), 1u);
      const auto *Type = (*Types)[0].getAsObject();
      ASSERT_NE(Type, nullptr);
      EXPECT_EQ(Type->getString("module"), "Demo");
      EXPECT_EQ(Type->getString("name"), "Empty");
      EXPECT_EQ(Type->getString("kind"), "struct");
      EXPECT_EQ(Type->getString("status"), "unrecovered");
      EXPECT_EQ(Type->getString("reason"), Reason);
    }
  }
}

TEST_F(SwiftNominalContextCAPI, EmptyFieldsDoNotReplaceValueWitnessLayout) {
  for (bool Arm64 : {true, false}) {
    SCOPED_TRACE(Arm64 ? "AArch64" : "x86-64");
    for (Damage Fault :
         {Damage::MissingValueWitnesses, Damage::TruncatedValueWitnesses,
          Damage::NonemptyStorage, Damage::ZeroStride,
          Damage::OveralignedStorage, Damage::NontrivialValues,
          Damage::NoncopyableValues, Damage::ExtraInhabitants,
          Damage::UnknownValueFlags}) {
      SCOPED_TRACE(static_cast<int>(Fault));
      ASSERT_NO_FATAL_FAILURE(load(Arm64, Fault));
      const auto Value = report({request()});
      rejected(Value, "runtime source context has ambiguous or unsupported "
                      "native metadata");
      const auto *Report = Value.getAsObject();
      ASSERT_NE(Report, nullptr);
      const auto *Types = Report->getArray("types");
      ASSERT_NE(Types, nullptr);
      ASSERT_EQ(Types->size(), 1u);
      const auto *Type = (*Types)[0].getAsObject();
      ASSERT_NE(Type, nullptr);
      EXPECT_EQ(Type->getString("status"), "unrecovered");
      ASSERT_TRUE(Type->getString("reason"));
      EXPECT_FALSE(Type->getString("reason")->empty());
      ASSERT_NE(Type->getArray("fields"), nullptr);
      EXPECT_TRUE(Type->getArray("fields")->empty());
      EXPECT_EQ(neverd_func_count(Session), 1);
    }
  }
}

TEST_F(SwiftNominalContextCAPI, OrdinaryMemberCannotBypassEmptyLayoutCheck) {
  for (bool Arm64 : {true, false}) {
    SCOPED_TRACE(Arm64 ? "AArch64" : "x86-64");
    for (Damage Fault :
         {Damage::None, Damage::MissingValueWitnesses,
          Damage::OveralignedStorage, Damage::NoncopyableValues}) {
      SCOPED_TRACE(static_cast<int>(Fault));
      ASSERT_NO_FATAL_FAILURE(load(Arm64, Fault, true, true));
      const auto Value = report({memberRequest()});
      const auto *Report = Value.getAsObject();
      ASSERT_NE(Report, nullptr);
      const bool Recoverable = Fault == Damage::None;
      counts(*Report, 1, Recoverable ? 1 : 0, 0);
      const auto *Rows = Report->getArray("methods");
      ASSERT_NE(Rows, nullptr);
      ASSERT_EQ(Rows->size(), 1u);
      const auto *Row = (*Rows)[0].getAsObject();
      ASSERT_NE(Row, nullptr);
      EXPECT_EQ(Row->getString("entry"), kGlobalEntry);
      EXPECT_EQ(Row->getString("mangled_symbol"), kMemberSymbol);
      EXPECT_EQ(Row->getString("context_name"), "Empty");
      const auto *Units = Report->getArray("source_units");
      ASSERT_NE(Units, nullptr);
      if (Recoverable) {
        EXPECT_EQ(Row->getString("status"), "recovered");
        EXPECT_EQ(Row->getString("source_representation"),
                  "native-method-body");
        ASSERT_TRUE(Row->getString("source"));
        EXPECT_NE(Row->getString("source")->find("42"), llvm::StringRef::npos);
        ASSERT_EQ(Units->size(), 1u);
        ASSERT_NE((*Units)[0].getAsObject(), nullptr);
        EXPECT_EQ((*Units)[0].getAsObject()->getString("kind"), "type");
        EXPECT_EQ((*Units)[0].getAsObject()->getString("name"), "Empty");
      } else {
        EXPECT_EQ(Row->getString("status"), "unrecovered");
        EXPECT_EQ(Row->getString("reason"),
                  "Swift declaration context has no complete native storage "
                  "layout");
        EXPECT_FALSE(Row->get("source"));
        EXPECT_FALSE(Row->get("source_representation"));
        EXPECT_EQ(Report->getString("source"), "");
        EXPECT_TRUE(Units->empty());
      }
      EXPECT_EQ(neverd_func_count(Session), 2);
    }
  }
}

TEST_F(SwiftNominalContextCAPI, GlobalCollisionRejectsBothNativeIdentities) {
  for (bool Arm64 : {true, false}) {
    SCOPED_TRACE(Arm64 ? "AArch64" : "x86-64");
    ASSERT_NO_FATAL_FAILURE(load(Arm64, Damage::None, true));
    // Prove the ordinary row has a real recoverable native body, rather than
    // obtaining two failures from an already unsupported signature.
    const auto Ordinary = report({request(true)});
    const auto *Report = Ordinary.getAsObject();
    ASSERT_NE(Report, nullptr);
    counts(*Report, 1, 1, 0);
    const auto *Rows = Report->getArray("methods");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = (*Rows)[0].getAsObject();
    ASSERT_NE(Row, nullptr);
    identity(*Row, true);
    EXPECT_EQ(Row->getString("status"), "recovered");
    EXPECT_EQ(Row->getString("source_representation"), "native-method-body");
    ASSERT_TRUE(Row->getString("source"));
    EXPECT_NE(Row->getString("source")->find("42"), llvm::StringRef::npos);
    const auto *Units = Report->getArray("source_units");
    ASSERT_NE(Units, nullptr);
    ASSERT_EQ(Units->size(), 1u);
    ASSERT_NE((*Units)[0].getAsObject(), nullptr);
    EXPECT_EQ((*Units)[0].getAsObject()->getString("kind"), "function");
    for (bool GlobalFirst : {true, false}) {
      SCOPED_TRACE(GlobalFirst ? "global first" : "accessor first");
      llvm::json::Array Methods;
      Methods.push_back(request(GlobalFirst));
      Methods.push_back(request(!GlobalFirst));
      rejected(report(std::move(Methods)),
               "Swift declarations collide in the emitted source namespace",
               true, GlobalFirst);
    }
    EXPECT_EQ(neverd_func_count(Session), 2);
  }
}

} // namespace
