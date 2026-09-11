//===- SessionCAPITests.cpp - public session failure contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/ProjectWriteLock.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Text(Value);
  neverd_free_string(Value);
  return Text;
}

// A sectionless executable with one RX segment and a two-instruction body.
// Keeping both ISAs in the fixture makes decoder state observable on reload.
std::string makeNativeELF(bool AArch64, uint64_t Base = 0x400000,
                          std::string_view TrailingCode = {}) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Code = AArch64
                         ? std::string("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
                         : std::string("\xb8\x07\x00\x00\x00\xc3", 6);
  Code += TrailingCode;
  const size_t CodeOffset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  std::string Bytes(CodeOffset + Code.size(), '\0');
  ELF::Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = AArch64 ? EM_AARCH64 : EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Base + CodeOffset;
  Header.e_phoff = sizeof(ELF::Ehdr);
  Header.e_ehsize = sizeof(ELF::Ehdr);
  Header.e_phentsize = sizeof(ELF::Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(ELF::Shdr);
  ELF::Phdr Segment{};
  Segment.p_type = PT_LOAD;
  Segment.p_flags = PF_R | PF_X;
  Segment.p_vaddr = Base;
  Segment.p_paddr = Base;
  Segment.p_filesz = Bytes.size();
  Segment.p_memsz = Bytes.size();
  Segment.p_align = 0x1000;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), &Segment, sizeof(Segment));
  Bytes.replace(CodeOffset, Code.size(), Code);
  return Bytes;
}

// Give the native function a loader-provided name without passing it through
// another JSON API before the IR page boundary under test.
std::string makeNamedNativeELF(const std::string &Name,
                               uint64_t Base = 0x400000) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Bytes = makeNativeELF(false, Base);
  ELF::Ehdr Header{};
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  std::array<ELF::Shdr, 5> Sections{};
  Sections[1].sh_name = 1;
  Sections[1].sh_type = SHT_PROGBITS;
  Sections[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[1].sh_addr = Header.e_entry;
  Sections[1].sh_offset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  Sections[1].sh_size = 6;
  Sections[1].sh_addralign = 1;

  Bytes.resize((Bytes.size() + 7) & ~size_t(7), '\0');
  std::array<ELF::Sym, 2> Symbols{};
  Symbols[1].st_name = 1;
  Symbols[1].setBindingAndType(STB_GLOBAL, STT_FUNC);
  Symbols[1].st_shndx = 1;
  Symbols[1].st_value = Header.e_entry;
  Symbols[1].st_size = 6;
  Sections[2].sh_name = 7;
  Sections[2].sh_type = SHT_SYMTAB;
  Sections[2].sh_offset = Bytes.size();
  Sections[2].sh_size = sizeof(Symbols);
  Sections[2].sh_link = 3;
  Sections[2].sh_info = 1;
  Sections[2].sh_addralign = 8;
  Sections[2].sh_entsize = sizeof(ELF::Sym);
  Bytes.append(reinterpret_cast<const char *>(Symbols.data()), sizeof(Symbols));

  const std::string Names = std::string(1, '\0') + Name + '\0';
  Sections[3].sh_name = 15;
  Sections[3].sh_type = SHT_STRTAB;
  Sections[3].sh_offset = Bytes.size();
  Sections[3].sh_size = Names.size();
  Sections[3].sh_addralign = 1;
  Bytes += Names;
  constexpr char SectionNames[] = "\0.text\0.symtab\0.strtab\0.shstrtab";
  Sections[4].sh_name = 23;
  Sections[4].sh_type = SHT_STRTAB;
  Sections[4].sh_offset = Bytes.size();
  Sections[4].sh_size = sizeof(SectionNames);
  Sections[4].sh_addralign = 1;
  Bytes.append(SectionNames, sizeof(SectionNames));
  Bytes.resize((Bytes.size() + 7) & ~size_t(7), '\0');
  Header.e_shoff = Bytes.size();
  Header.e_shnum = Sections.size();
  Header.e_shstrndx = 4;
  Bytes.append(reinterpret_cast<const char *>(Sections.data()),
               sizeof(Sections));
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  return Bytes;
}

class SessionCAPITest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> Sequence{0};
    const auto Stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path TemporaryRoot =
        std::filesystem::temp_directory_path();
    for (unsigned Attempt = 0; Attempt < 100; ++Attempt) {
      auto Candidate =
          TemporaryRoot / ("neverd-session-" + std::to_string(Stamp) + "-" +
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

  std::string write(std::string_view Name, std::string_view Text) {
    const std::string Path = (Directory / Name).string();
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
    Output.close();
    EXPECT_TRUE(Output) << "cannot write fixture " << Path;
    return Path;
  }

  void expectSignatureJSONName(const std::string &Name) {
    const auto Input =
        write("signature-input.elf", makeNamedNativeELF("entry"));
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
        << takeString(neverd_last_error(Session));
    const neverd_va_t Entry = neverd_session_entry_addr(Session);
    ASSERT_EQ(neverd_func_count(Session), 1);
    const auto Pattern =
        write("signature-library.pat",
              "B807000000C3 00 0000 0006 :0000 " + Name + "\n");
    ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1)
        << takeString(neverd_last_error(Session));
    EXPECT_EQ(neverd_sig_match_count(Session), 1);
    const std::string Text = takeString(neverd_sig_matches_json(Session));
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    const auto *Matches = Parsed->getAsArray();
    ASSERT_NE(Matches, nullptr);
    ASSERT_EQ(Matches->size(), 1U);
    const auto *Match = (*Matches)[0].getAsObject();
    ASSERT_NE(Match, nullptr);
    EXPECT_EQ(Match->getString("name"), Name);
    EXPECT_EQ(Match->getString("library"), "signature-library");
    EXPECT_EQ(Match->getInteger("func_len"), 6);
    const auto Address = Match->getString("addr");
    ASSERT_TRUE(Address.has_value());
    uint64_t ParsedAddress = 0;
    ASSERT_FALSE(Address->getAsInteger(0, ParsedAddress));
    EXPECT_EQ(ParsedAddress, Entry);
    EXPECT_EQ(neverd_sig_match_count(Session), 1);
  }

  void expectQueryJSONFileSpelling(const std::string &Input) {
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
        << takeString(neverd_last_error(Session));
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Input);

    auto Headers = llvm::json::parse(takeString(neverd_headers_json(Session)));
    ASSERT_TRUE(static_cast<bool>(Headers))
        << llvm::toString(Headers.takeError());
    const auto *HeaderObject = Headers->getAsObject();
    ASSERT_NE(HeaderObject, nullptr);
    EXPECT_EQ(HeaderObject->getString("file_path"), Input);

    auto Dashboard =
        llvm::json::parse(takeString(neverd_dashboard_json(Session)));
    ASSERT_TRUE(static_cast<bool>(Dashboard))
        << llvm::toString(Dashboard.takeError());
    const auto *DashboardObject = Dashboard->getAsObject();
    ASSERT_NE(DashboardObject, nullptr);
    const auto *File = DashboardObject->getObject("file");
    ASSERT_NE(File, nullptr);
    EXPECT_EQ(File->getString("path"), Input);
    EXPECT_EQ(File->getString("name"),
              std::filesystem::path(Input).filename().string());
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Input);
  }

  std::string readSidecar(std::string_view Name) {
    std::ifstream Input(Directory / Name, std::ios::binary);
    EXPECT_TRUE(Input.is_open());
    return std::string(std::istreambuf_iterator<char>(Input),
                       std::istreambuf_iterator<char>());
  }

  void expectPersistedRows(const std::string &Text, llvm::StringRef ValueField,
                           const std::map<uint64_t, std::string> &Expected) {
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    const auto *Rows = Parsed->getAsArray();
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), Expected.size());
    std::map<uint64_t, std::string> Actual;
    for (const auto &Row : *Rows) {
      const auto *Object = Row.getAsObject();
      ASSERT_NE(Object, nullptr);
      const auto Address = Object->getString("addr");
      const auto Value = Object->getString(ValueField);
      ASSERT_TRUE(Address.has_value());
      ASSERT_TRUE(Value.has_value());
      ASSERT_TRUE(Address->starts_with("0x"));
      uint64_t Key = 0;
      ASSERT_FALSE(Address->drop_front(2).getAsInteger(16, Key));
      ASSERT_TRUE(Actual.emplace(Key, Value->str()).second);
    }
    EXPECT_EQ(Actual, Expected);
  }

  std::filesystem::path Directory;
  neverd_session_t Session = nullptr;
};

TEST_F(SessionCAPITest, QueryJSONPreservesASCIIFileSpelling) {
  const auto Input = write("query sample.elf", makeNamedNativeELF("entry"));
  expectQueryJSONFileSpelling(Input);
}

#ifndef _WIN32
TEST_F(SessionCAPITest, QueryJSONPreservesUTF8FileSpelling) {
  // Valid UTF-8 bytes exercise the POSIX path spelling contract.
  const auto Input = write("r\xc3\xa9sum\xc3\xa9-\xe5\x85\xa5\xe5\x8f\xa3.elf",
                           makeNamedNativeELF("entry"));
  expectQueryJSONFileSpelling(Input);
}
#endif

TEST_F(SessionCAPITest, EntryPointsIncludeEVMZeroAddress) {
  const std::string Input = write("entry.evm", "00");
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  auto Parsed = llvm::json::parse(takeString(neverd_entrypoints_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Entries = Parsed->getAsArray();
  ASSERT_NE(Entries, nullptr);
  ASSERT_EQ(Entries->size(), 1u);
  const auto *Entry = (*Entries)[0].getAsObject();
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->getString("type"), "entry");
  EXPECT_EQ(Entry->getString("addr"), "0x0");
  EXPECT_EQ(Entry->getString("name"), "evm_entry");

  auto Dashboard =
      llvm::json::parse(takeString(neverd_dashboard_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Dashboard))
      << llvm::toString(Dashboard.takeError());
  const auto *Object = Dashboard->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *Counts = Object->getObject("counts");
  ASSERT_NE(Counts, nullptr);
  EXPECT_EQ(Counts->getInteger("entrypoints"), 1);
  EXPECT_EQ(Counts->getInteger("entrypoints"),
            static_cast<int64_t>(Entries->size()));
}

TEST_F(SessionCAPITest, EntryPointsPreserveNativeAddress) {
  const std::string Input = write("entry.elf", makeNamedNativeELF("entry"));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  auto Parsed = llvm::json::parse(takeString(neverd_entrypoints_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Entries = Parsed->getAsArray();
  ASSERT_NE(Entries, nullptr);
  ASSERT_EQ(Entries->size(), 1u);
  const auto *Entry = (*Entries)[0].getAsObject();
  ASSERT_NE(Entry, nullptr);
  EXPECT_EQ(Entry->getString("type"), "entry");
  EXPECT_EQ(Entry->getString("addr"), "0x400078");
  EXPECT_EQ(Entry->getString("name"), "entry");

  auto Dashboard =
      llvm::json::parse(takeString(neverd_dashboard_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Dashboard))
      << llvm::toString(Dashboard.takeError());
  const auto *Object = Dashboard->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *Counts = Object->getObject("counts");
  ASSERT_NE(Counts, nullptr);
  EXPECT_EQ(Counts->getInteger("entrypoints"), 1);
  EXPECT_EQ(Counts->getInteger("entrypoints"),
            static_cast<int64_t>(Entries->size()));
}

TEST_F(SessionCAPITest, EntryPointsOmitAbsentNativeAddress) {
  std::string Bytes = makeNativeELF(false);
  llvm::object::ELF64LE::Ehdr Header{};
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Header.e_entry = 0;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  const std::string Input = write("no-entry.elf", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  auto Parsed = llvm::json::parse(takeString(neverd_entrypoints_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Entries = Parsed->getAsArray();
  ASSERT_NE(Entries, nullptr);
  EXPECT_TRUE(Entries->empty());

  auto Dashboard =
      llvm::json::parse(takeString(neverd_dashboard_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Dashboard))
      << llvm::toString(Dashboard.takeError());
  const auto *Object = Dashboard->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *Counts = Object->getObject("counts");
  ASSERT_NE(Counts, nullptr);
  EXPECT_EQ(Counts->getInteger("entrypoints"), 0);
  EXPECT_EQ(Counts->getInteger("entrypoints"),
            static_cast<int64_t>(Entries->size()));
}

TEST_F(SessionCAPITest, SidecarWritesRespectWorkerOwnership) {
  const std::string Input = write("owned.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  ASSERT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
  neverd_annotation_set(Session, 0, "staged note");
  {
    neverd::ProjectWriteLock Writer(Input);
    ASSERT_TRUE(static_cast<bool>(Writer)) << Writer.error();
    EXPECT_NE(neverd_annotations_save(Session), 0);
    EXPECT_FALSE(std::filesystem::exists(Input + ".neverd-annotations.json"));
    EXPECT_NE(neverd_rename_func(Session, "evm_entry", "blocked_name"), 0);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
    EXPECT_EQ(takeString(neverd_renames_json(Session)), "[]");
    EXPECT_FALSE(std::filesystem::exists(Input + ".neverd-renames.json"));
    // Read-only views remain usable while another writer owns the input.
    EXPECT_EQ(takeString(neverd_annotation_get(Session, 0)), "staged note");
  }
  ASSERT_EQ(neverd_annotations_save(Session), 0);
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "saved_name"), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "saved_name");
  EXPECT_TRUE(std::filesystem::exists(Input + ".neverd-annotations.json"));
  EXPECT_TRUE(std::filesystem::exists(Input + ".neverd-renames.json"));
}

TEST_F(SessionCAPITest, ReloadingRenamesRestoresNamesRemovedFromTheSidecar) {
  const std::string Original = write("original.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1);
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "reviewed_entry"), 0);
  ASSERT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");

  write("original.evm.neverd-renames.json", "invalid JSON");
  EXPECT_NE(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");

  write("original.evm.neverd-renames.json", "[]");
  ASSERT_EQ(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_renames_json(Session)), "[]");
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
}

TEST_F(SessionCAPITest,
       ReloadingAbsentSidecarsDiscardsStagedNotesAndDeletedRenames) {
  const std::string Path = write("reload.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  neverd_annotation_set(Session, 0, "discard this unsaved note");
  ASSERT_FALSE(std::filesystem::exists(Path + ".neverd-annotations.json"));
  ASSERT_EQ(neverd_annotations_load(Session), 0);
  EXPECT_TRUE(takeString(neverd_annotation_get(Session, 0)).empty());

  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "reviewed_entry"), 0);
  ASSERT_TRUE(std::filesystem::remove(Path + ".neverd-renames.json"));
  ASSERT_EQ(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
  EXPECT_EQ(takeString(neverd_renames_json(Session)), "[]");
}

#ifndef _WIN32
TEST_F(SessionCAPITest,
       FailedSidecarWritesReturnErrorsAndPreservePreviousFiles) {
  const std::string Path = write("write-failure.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_annotations_save(Session), 0);
  ASSERT_EQ(neverd_renames_save(Session), 0);
  ASSERT_EQ(neverd_func_count(Session), 1);
  ASSERT_EXIT(
      {
        struct rlimit Limit;
        if (getrlimit(RLIMIT_FSIZE, &Limit) != 0)
          _exit(2);
        Limit.rlim_cur = 1;
        if (setrlimit(RLIMIT_FSIZE, &Limit) != 0)
          _exit(3);
        std::signal(SIGXFSZ, SIG_IGN);
        neverd_annotation_set(Session, 0, "staged note");
        if (neverd_annotations_save(Session) == 0)
          _exit(4);
        if (takeString(neverd_last_error(Session)).empty())
          _exit(5);
        if (neverd_rename_func(Session, "evm_entry", "failed_name") == 0)
          _exit(6);
        if (takeString(neverd_func_name(Session, 0)) != "evm_entry")
          _exit(7);
        for (const char *Suffix :
             {".neverd-annotations.json", ".neverd-renames.json"}) {
          std::ifstream Input(Path + Suffix);
          std::string Text((std::istreambuf_iterator<char>(Input)),
                           std::istreambuf_iterator<char>());
          if (Text != "[]")
            _exit(8);
        }
        _exit(0);
      },
      ::testing::ExitedWithCode(0), ".*");
}
#endif

TEST_F(SessionCAPITest, FailedDebugReloadPreservesLoadedImageAnalysisAndEdits) {
  const std::string Original = write("original.evm", "6001600055");
  const std::string Replacement = write("replacement.evm", "6002600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const std::string LowIR = takeString(neverd_ir_low(Session, 0));
  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, 0));
  const std::string Source = takeString(neverd_decompile(Session, 0));
  ASSERT_FALSE(LowIR.empty());
  ASSERT_FALSE(LLVMIR.empty());
  ASSERT_FALSE(Source.empty());
  neverd_annotation_set(Session, 0, "retain this comment");
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "reviewed_entry"), 0)
      << takeString(neverd_last_error(Session));
  const std::string Renames = takeString(neverd_renames_json(Session));

  for (bool PDB : {false, true}) {
    SCOPED_TRACE(PDB ? "missing PDB" : "missing MAP");
    const std::string Missing = (Directory / "absent.debug").string();
    neverd_session_set_map_path(Session, PDB ? nullptr : Missing.c_str());
    neverd_session_set_pdb_path(Session, PDB ? Missing.c_str() : nullptr);
    ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 0);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("file not found"),
              std::string::npos);
    EXPECT_EQ(neverd_session_is_loaded(Session), 1);
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Original);
    EXPECT_EQ(neverd_func_count(Session), 1);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");
    EXPECT_EQ(takeString(neverd_annotation_get(Session, 0)),
              "retain this comment");
    EXPECT_EQ(takeString(neverd_renames_json(Session)), Renames);
    std::array<unsigned char, 2> Bytes{};
    EXPECT_EQ(neverd_read_bytes(Session, 0, Bytes.data(), Bytes.size()), 2);
    EXPECT_EQ(Bytes[1], 1);
    EXPECT_EQ(neverd_session_analyze(Session), 1);
    EXPECT_EQ(takeString(neverd_ir_low(Session, 0)), LowIR);
    EXPECT_EQ(takeString(neverd_ir_llvm(Session, 0)), LLVMIR);
    EXPECT_EQ(takeString(neverd_decompile(Session, 0)), Source);
  }
}

TEST_F(SessionCAPITest, CachedAnalysisFailuresRetainTheirDiagnostic) {
  const std::string Path = write("requires-shanghai.evm", "5f00");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_evm_set_hardfork(Session, "london"), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 0);
  const std::string Diagnostic = takeString(neverd_last_error(Session));
  ASSERT_NE(Diagnostic.find("inactive opcode"), std::string::npos);

  using TextQuery = const char *(*)(neverd_session_t, neverd_va_t);
  const std::array<TextQuery, 5> Queries{neverd_decompile, neverd_ir_low,
                                         neverd_ir_med, neverd_ir_high,
                                         neverd_ir_llvm};
  for (size_t Index = 0; Index < Queries.size(); ++Index) {
    SCOPED_TRACE(Index);
    EXPECT_TRUE(takeString(Queries[Index](Session, 0)).empty());
    EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);
    EXPECT_EQ(neverd_session_analyze(Session), 0);
    EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);
  }

  ASSERT_EQ(neverd_evm_set_hardfork(Session, "shanghai"), 1);
  EXPECT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_FALSE(takeString(neverd_ir_low(Session, 0)).empty());
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, FailedFirstLoadLeavesSessionUnloadedAndRetryable) {
  const std::string Path = write("input.evm", "6001600055");
  const std::string Missing = (Directory / "absent.map").string();
  neverd_session_set_map_path(Session, Missing.c_str());
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 0);
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  EXPECT_EQ(neverd_func_count(Session), 0);
  EXPECT_TRUE(takeString(neverd_session_file_path(Session)).empty());
  EXPECT_TRUE(takeString(neverd_ir_low(Session, 0)).empty());
  EXPECT_EQ(takeString(neverd_last_error(Session)), "no binary loaded");

  neverd_session_set_map_path(Session, nullptr);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_FALSE(takeString(neverd_ir_low(Session, 0)).empty());
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, SuccessfulReloadReplacesAnalysisAndPerImageEdits) {
  const std::string Original = write("original.evm", "6001600055");
  const std::string Replacement = write("replacement.evm", "6002600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1);
  const std::string OriginalIR = takeString(neverd_ir_llvm(Session, 0));
  ASSERT_FALSE(OriginalIR.empty());
  neverd_annotation_set(Session, 0, "old comment");
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "old_name"), 0);

  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_file_path(Session)), Replacement);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");
  EXPECT_EQ(takeString(neverd_annotations_json(Session)), "[]");
  EXPECT_EQ(takeString(neverd_renames_json(Session)), "[]");
  std::array<unsigned char, 2> Bytes{};
  EXPECT_EQ(neverd_read_bytes(Session, 0, Bytes.data(), Bytes.size()), 2);
  EXPECT_EQ(Bytes[1], 2);
  const std::string ReplacementIR = takeString(neverd_ir_llvm(Session, 0));
  ASSERT_FALSE(ReplacementIR.empty());
  EXPECT_NE(ReplacementIR, OriginalIR);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, FailedNativeReloadPreservesDecoderAndDebugSelection) {
  const std::string Original = write("x86_64.elf", makeNativeELF(false));
  const std::string Replacement = write("aarch64.elf", makeNativeELF(true));
  const std::string Map =
      write("selected.map", "VMA LMA Size Align Out In Symbol\n"
                            "00400078 00400078 00000006 1 .text\n"
                            "00400078 00400078 00000006 1 reviewed_function\n");
  neverd_session_set_map_path(Session, Map.c_str());
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  const std::string Disassembly =
      takeString(neverd_disasm_json(Session, Entry, 2));
  ASSERT_NE(Disassembly.find("eax"), std::string::npos) << Disassembly;
  const std::string DebugKind =
      takeString(neverd_session_debug_info_kind(Session));
  const std::string DebugPath =
      takeString(neverd_session_debug_info_path(Session));
  ASSERT_EQ(DebugKind, "map");
  ASSERT_EQ(DebugPath, Map);
  const std::string Missing = (Directory / "absent.map").string();
  neverd_session_set_map_path(Session, Missing.c_str());
  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 0);
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "x86_64");
  EXPECT_EQ(takeString(neverd_session_debug_info_kind(Session)), DebugKind);
  EXPECT_EQ(takeString(neverd_session_debug_info_path(Session)), DebugPath);
  EXPECT_EQ(takeString(neverd_disasm_json(Session, Entry, 2)), Disassembly);

  neverd_session_set_map_path(Session, nullptr);
  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "aarch64");
  const std::string NewDisassembly =
      takeString(neverd_disasm_json(Session, Entry, 2));
  EXPECT_NE(NewDisassembly.find("w0"), std::string::npos) << NewDisassembly;
  EXPECT_NE(NewDisassembly, Disassembly);
}

TEST_F(SessionCAPITest, NativeAnalysisPublishesRecoveredFunctions) {
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64 ? "aarch64" : "x86_64");
    const std::string Path =
        write(AArch64 ? "recovered-arm.elf" : "recovered-x86.elf",
              makeNativeELF(AArch64));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
    const neverd_va_t Entry = neverd_session_entry_addr(Session);
    // Listing loader symbols must keep the inexpensive, unanalyzed path.
    EXPECT_EQ(neverd_func_count(Session), 0);
    ASSERT_EQ(neverd_session_analyze(Session), 1)
        << takeString(neverd_last_error(Session));
    ASSERT_EQ(neverd_func_count(Session), 1);
    const int Index = neverd_func_find_by_addr(Session, Entry);
    ASSERT_GE(Index, 0);
    EXPECT_EQ(neverd_func_entry(Session, Index), Entry);
    EXPECT_EQ(neverd_func_size(Session, Index), AArch64 ? 8 : 6);
    const std::string Name = takeString(neverd_func_name(Session, Index));
    EXPECT_FALSE(Name.empty());
    EXPECT_EQ(neverd_func_find_by_name(Session, Name.c_str()), Index);
    const std::string Resolved =
        takeString(neverd_resolve_addr(Session, Entry));
    auto Parsed = llvm::json::parse(Resolved);
    ASSERT_TRUE(static_cast<bool>(Parsed)) << Resolved;
    ASSERT_NE(Parsed->getAsObject(), nullptr);
    EXPECT_EQ(Parsed->getAsObject()->getString("type"), "function");
    ASSERT_EQ(neverd_session_analyze(Session), 1);
    EXPECT_EQ(neverd_func_count(Session), 1);
  }
}

TEST_F(SessionCAPITest, LazyNativeAnalysisRetainsRecoveredNamesAcrossReload) {
  const std::string Path = write("recovered.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  ASSERT_FALSE(takeString(neverd_ir_low(Session, Entry)).empty());
  const int Index = neverd_func_find_by_addr(Session, Entry);
  ASSERT_GE(Index, 0);
  const std::string Original = takeString(neverd_func_name(Session, Index));
  ASSERT_EQ(neverd_rename_func(Session, Original.c_str(), "reviewed_entry"), 0)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(neverd_func_find_by_name(Session, "reviewed_entry"), Index);

  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  EXPECT_EQ(neverd_func_count(Session), 0);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  EXPECT_EQ(neverd_func_count(Session), 1);
  const int Reloaded = neverd_func_find_by_addr(Session, Entry);
  ASSERT_GE(Reloaded, 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, Reloaded)), "reviewed_entry");
  EXPECT_NE(takeString(neverd_renames_json(Session)).find(Original),
            std::string::npos);

  const std::string Replacement =
      write("replacement.elf", makeNativeELF(true, 0x800000));
  ASSERT_EQ(neverd_session_load(Session, Replacement.c_str()), 1);
  EXPECT_EQ(neverd_func_count(Session), 0);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  EXPECT_EQ(neverd_func_count(Session), 1);
  EXPECT_EQ(neverd_func_find_by_addr(Session, Entry), -1);
  EXPECT_EQ(neverd_func_find_by_name(Session, "reviewed_entry"), -1);
}

TEST_F(SessionCAPITest, NativeDisassemblyUsesRecoveredExtentAfterLazyAnalysis) {
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64 ? "aarch64" : "x86_64");
    // Two executable NOPs follow the return, outside the recovered body. The
    // segment boundary alone must not make an unclipped decoder look correct.
    const std::string Tail =
        AArch64 ? std::string("\x1f\x20\x03\xd5\x1f\x20\x03\xd5", 8)
                : std::string("\x90\x90", 2);
    const std::string Path =
        write(AArch64 ? "extent-arm.elf" : "extent-x86.elf",
              makeNativeELF(AArch64, 0x400000, Tail));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
    const neverd_va_t Entry = neverd_session_entry_addr(Session);
    const uint64_t FunctionSize = AArch64 ? 8 : 6;
    const uint64_t ReturnOffset = AArch64 ? 4 : 5;
    const std::string BeforeAnalysis =
        takeString(neverd_disasm_json(Session, Entry, 16));
    auto Before = llvm::json::parse(BeforeAnalysis);
    ASSERT_TRUE(static_cast<bool>(Before)) << BeforeAnalysis;
    ASSERT_NE(Before->getAsArray(), nullptr);
    EXPECT_EQ(Before->getAsArray()->size(), 4U);
    // A quick native decode must not start the full pipeline and publish the
    // entry. Function listing remains an inexpensive loader-only operation.
    EXPECT_EQ(neverd_func_count(Session), 0);

    std::array<std::string, 3> Disassemblies;
    for (size_t Order = 0; Order < Disassemblies.size(); ++Order) {
      SCOPED_TRACE(Order);
      ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
      if (Order == 2) {
        ASSERT_EQ(neverd_session_analyze(Session), 1)
            << takeString(neverd_last_error(Session));
      } else {
        ASSERT_FALSE(takeString(neverd_ir_low(Session, Entry)).empty())
            << takeString(neverd_last_error(Session));
        if (Order == 1)
          EXPECT_GT(neverd_func_count(Session), 0);
      }
      // Order 0 deliberately performs no synchronizing list/metadata query
      // between lazy IR production and disassembly.
      Disassemblies[Order] = takeString(neverd_disasm_json(Session, Entry, 16));
      auto Parsed = llvm::json::parse(Disassemblies[Order]);
      ASSERT_TRUE(static_cast<bool>(Parsed)) << Disassemblies[Order];
      const auto *Rows = Parsed->getAsArray();
      ASSERT_NE(Rows, nullptr);
      ASSERT_EQ(Rows->size(), 2U) << Disassemblies[Order];
      for (size_t I = 0; I < Rows->size(); ++I) {
        const auto *Row = (*Rows)[I].getAsObject();
        ASSERT_NE(Row, nullptr);
        const auto Address = Row->getString("addr");
        ASSERT_TRUE(Address);
        const uint64_t VA = std::stoull(Address->str(), nullptr, 16);
        EXPECT_EQ(VA, Entry + (I == 0 ? 0 : ReturnOffset));
        EXPECT_LT(VA, Entry + FunctionSize);
      }
      const int Index = neverd_func_find_by_addr(Session, Entry);
      ASSERT_GE(Index, 0);
      EXPECT_EQ(neverd_func_size(Session, Index), FunctionSize);
    }
    EXPECT_EQ(Disassemblies[0], Disassemblies[1]);
    EXPECT_EQ(Disassemblies[0], Disassemblies[2]);
  }
}

TEST_F(SessionCAPITest, NativeAnalysisPreservesLoaderNamesAndSavedRenames) {
  const std::string Path =
      write("named.elf", makeNamedNativeELF("named_entry"));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_func_count(Session), 1);
  const neverd_va_t Entry = neverd_func_entry(Session, 0);
  ASSERT_EQ(neverd_rename_func(Session, "named_entry", "reviewed_entry"), 0);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  EXPECT_EQ(neverd_func_count(Session), 1);
  EXPECT_EQ(neverd_func_entry(Session, 0), Entry);
  EXPECT_EQ(neverd_func_size(Session, 0), 6);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "reviewed_entry");
  ASSERT_TRUE(std::filesystem::remove(Path + ".neverd-renames.json"));
  ASSERT_EQ(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "named_entry");
  EXPECT_EQ(neverd_func_count(Session), 1);
}

TEST_F(SessionCAPITest,
       NativeLLVMQueryUsesOriginalAddressForDebugNamedFunction) {
  const std::string Path = write("named.elf", makeNativeELF(false));
  const std::string Map =
      write("named.map", "VMA LMA Size Align Out In Symbol\n"
                         "00400078 00400078 00000006 1 .text\n"
                         "00400078 00400078 00000006 1 meaningful_name\n");
  neverd_session_set_map_path(Session, Map.c_str());
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  ASSERT_EQ(takeString(neverd_func_name(Session, 0)), "meaningful_name");

  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, Entry));
  ASSERT_FALSE(LLVMIR.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(LLVMIR.find("define "), std::string::npos) << LLVMIR;
  EXPECT_NE(LLVMIR.find("@meaningful_name("), std::string::npos) << LLVMIR;
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(takeString(neverd_ir_llvm(Session, Entry)), LLVMIR);
}

TEST_F(SessionCAPITest, NativeLLVMQueryRejectsAnAddressThatOnlyMatchesAPrefix) {
  const std::string Path = write("anonymous.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, Entry));
  ASSERT_FALSE(LLVMIR.empty()) << takeString(neverd_last_error(Session));

  EXPECT_TRUE(takeString(neverd_ir_llvm(Session, Entry >> 4)).empty());
  EXPECT_NE(takeString(neverd_last_error(Session)).find("not found"),
            std::string::npos);
  EXPECT_EQ(takeString(neverd_ir_llvm(Session, Entry)), LLVMIR);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, TargetOptionChangesInvalidatePreviouslyCachedLLVM) {
  const std::string Path = write("requires-shanghai.evm", "5f00");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_evm_set_hardfork(Session, "shanghai"), 1);
  const std::string LLVMIR = takeString(neverd_ir_llvm(Session, 0));
  ASSERT_FALSE(LLVMIR.empty()) << takeString(neverd_last_error(Session));

  ASSERT_EQ(neverd_evm_set_hardfork(Session, "london"), 1);
  EXPECT_TRUE(takeString(neverd_ir_llvm(Session, 0)).empty());
  EXPECT_NE(takeString(neverd_last_error(Session)).find("inactive opcode"),
            std::string::npos);

  ASSERT_EQ(neverd_evm_set_hardfork(Session, "shanghai"), 1);
  EXPECT_EQ(takeString(neverd_ir_llvm(Session, 0)), LLVMIR);
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

llvm::json::Object takeView(const char *Text) {
  EXPECT_NE(Text, nullptr);
  const auto Owned = takeString(Text);
  auto Parsed = llvm::json::parse(Owned);
  EXPECT_TRUE(static_cast<bool>(Parsed)) << Owned;
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return {};
  }
  auto *Object = Parsed->getAsObject();
  EXPECT_NE(Object, nullptr);
  return Object ? std::move(*Object) : llvm::json::Object{};
}

TEST_F(SessionCAPITest,
       NativeIRViewPagesPreserveTextAndExactInstructionAnchors) {
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64 ? "AArch64 high VA" : "x86_64");
    const uint64_t Base = AArch64 ? 0xffff800000400000ULL : 0x400000;
    const std::string Path = write("mapped.elf", makeNativeELF(AArch64, Base));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
        << takeString(neverd_last_error(Session));
    const auto Entry = neverd_session_entry_addr(Session);
    const uint64_t ReturnAddress = Entry + (AArch64 ? 4 : 5);
    for (const char *Stage : {"low", "med"}) {
      SCOPED_TRACE(Stage);
      const std::string Legacy = takeString(
          std::strcmp(Stage, "low") == 0 ? neverd_ir_low(Session, Entry)
                                         : neverd_ir_med(Session, Entry));
      ASSERT_FALSE(Legacy.empty()) << takeString(neverd_last_error(Session));
      size_t Offset = 0, Anchors = 0;
      std::string Reassembled;
      std::set<std::string> ObjectIds;
      for (;;) {
        auto Page =
            takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 2));
        ASSERT_EQ(Page.getString("mapping_status"), "instruction_anchors");
        EXPECT_EQ(Page.getBoolean("provenance_complete"), false);
        ASSERT_TRUE(Page.getString("text"));
        Reassembled += Page.getString("text")->str();
        const auto *Rows = Page.getArray("rows");
        ASSERT_NE(Rows, nullptr);
        ASSERT_LE(Rows->size(), 2U);
        for (size_t I = 0; I < Rows->size(); ++I) {
          const auto *Row = (*Rows)[I].getAsObject();
          ASSERT_NE(Row, nullptr);
          EXPECT_EQ(Row->getInteger("line"), Offset + I);
          ASSERT_TRUE(Row->getString("object_id"));
          EXPECT_TRUE(
              ObjectIds.insert(Row->getString("object_id")->str()).second);
          const auto *Addresses = Row->getArray("addresses");
          ASSERT_NE(Addresses, nullptr);
          if (Row->getString("mapping_status") == "instruction_anchor") {
            ASSERT_EQ(Addresses->size(), 1U);
            ASSERT_TRUE((*Addresses)[0].getAsString());
            const auto Address =
                std::stoull((*Addresses)[0].getAsString()->str(), nullptr, 16);
            EXPECT_TRUE(Address == Entry || Address == ReturnAddress);
            EXPECT_EQ(Row->getString("kind"), "operation");
            EXPECT_GE(Row->getInteger("origin_seq").value_or(-1), 0);
            ++Anchors;
          } else
            EXPECT_TRUE(Addresses->empty());
        }
        if (Page.getBoolean("complete").value_or(false)) {
          EXPECT_EQ(Page.getInteger("total_lines"), Offset + Rows->size());
          break;
        }
        ASSERT_TRUE(Page.getInteger("next_offset"));
        const auto Next = *Page.getInteger("next_offset");
        ASSERT_GT(Next, static_cast<int64_t>(Offset));
        Offset = static_cast<size_t>(Next);
        ASSERT_LT(Offset, 1000U);
      }
      EXPECT_GT(Anchors, 0U);
      EXPECT_EQ(Reassembled, Legacy);
      auto Beyond =
          takeView(neverd_ir_view_json(Session, Entry, Stage, 10000, 2));
      EXPECT_EQ(Beyond.getString("text"), "");
      EXPECT_TRUE(Beyond.getBoolean("complete").value_or(false));
      EXPECT_TRUE(Beyond.getArray("rows")->empty());
    }
  }
}

TEST_F(SessionCAPITest,
       IRViewRejectsInvalidPagesAndReportsUnsupportedMappings) {
  EXPECT_EQ(neverd_ir_view_json(Session, 0, "low", 0, 0), nullptr);
  EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(neverd_ir_view_json(Session, 0, nullptr, 0, 1), nullptr);
  EXPECT_EQ(neverd_ir_view_json(Session, 0, "med", 0, 2049), nullptr);
  const std::string Path = write("mapped.evm", "600160020100");
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  auto VM = takeView(neverd_ir_view_json(Session, 0, "low", 0, 2));
  EXPECT_EQ(VM.getString("mapping_status"), "unsupported_architecture");
  EXPECT_TRUE(VM.getArray("rows")->empty());
  EXPECT_FALSE(VM.getString("text"));
  for (const char *Stage : {"c", "high", "llvm"}) {
    auto Unsupported = takeView(neverd_ir_view_json(Session, 0, Stage, 0, 2));
    EXPECT_EQ(Unsupported.getString("mapping_status"),
              "unsupported_representation");
    EXPECT_TRUE(Unsupported.getArray("rows")->empty());
  }
}

TEST_F(SessionCAPITest, IRViewRejectsInvalidUTF8Representation) {
  EXPECT_EQ(neverd_ir_view_json(Session, 0, "\xff", 0, 1), nullptr);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("UTF-8"),
            std::string::npos);
  auto Unsupported = takeView(neverd_ir_view_json(Session, 0, "high", 0, 1));
  EXPECT_EQ(Unsupported.getString("mapping_status"),
            "unsupported_representation");
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(SessionCAPITest, IRViewPreservesEscapedLoaderNames) {
  const std::string Name = std::string("entry_") + '\xff';
  const std::string Expected = "entry_\\xFF";
  const auto Path = write("named.elf", makeNamedNativeELF(Name));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Entry = neverd_session_entry_addr(Session);
  int FunctionIndex = -1;
  const int FunctionCount = neverd_func_count(Session);
  for (int I = 0; I < FunctionCount; ++I)
    if (neverd_func_entry(Session, I) == Entry) {
      FunctionIndex = I;
      break;
    }
  ASSERT_GE(FunctionIndex, 0) << takeString(neverd_last_error(Session));
  ASSERT_EQ(takeString(neverd_func_name(Session, FunctionIndex)), Expected);
  for (const char *Stage : {"low", "med"}) {
    SCOPED_TRACE(Stage);
    const std::string Legacy = takeString(std::strcmp(Stage, "low") == 0
                                              ? neverd_ir_low(Session, Entry)
                                              : neverd_ir_med(Session, Entry));
    const std::string LegacyError = takeString(neverd_last_error(Session));
    ASSERT_FALSE(Legacy.empty()) << ::testing::PrintToString(LegacyError);
    ASSERT_NE(Legacy.find(Expected), std::string::npos)
        << "IR: " << ::testing::PrintToString(Legacy)
        << " error: " << ::testing::PrintToString(LegacyError);
    ASSERT_NE(Legacy.find('\n'), std::string::npos);
    auto First = takeView(neverd_ir_view_json(Session, Entry, Stage, 0, 1));
    ASSERT_TRUE(First.getString("text"));
    EXPECT_EQ(First.getString("text"), Legacy.substr(0, Legacy.find('\n') + 1));
    EXPECT_TRUE(llvm::json::isUTF8(*First.getString("text")));
    EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
    auto Next = takeView(neverd_ir_view_json(Session, Entry, Stage, 1, 1));
    ASSERT_TRUE(Next.getString("text"));
    EXPECT_FALSE(Next.getString("text")->empty());
    EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
  }
}

TEST_F(SessionCAPITest, IRViewPreservesMultibyteUTF8AcrossPhysicalPages) {
  const std::string Name = "entry_\xc3\xa9\n\xe4\xb8\xad_\xf0\x9f\x98\x80";
  const auto Path = write("unicode.elf", makeNamedNativeELF(Name));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Entry = neverd_session_entry_addr(Session);
  for (const char *Stage : {"low", "med"}) {
    SCOPED_TRACE(Stage);
    const std::string Legacy = takeString(std::strcmp(Stage, "low") == 0
                                              ? neverd_ir_low(Session, Entry)
                                              : neverd_ir_med(Session, Entry));
    ASSERT_NE(Legacy.find(Name), std::string::npos);
    size_t Offset = 0;
    std::string Reassembled;
    for (;;) {
      auto Page =
          takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 1));
      ASSERT_TRUE(Page.getString("text"));
      EXPECT_TRUE(llvm::json::isUTF8(*Page.getString("text")));
      Reassembled += Page.getString("text")->str();
      const auto *Rows = Page.getArray("rows");
      ASSERT_NE(Rows, nullptr);
      ASSERT_EQ(Rows->size(), 1U);
      ASSERT_NE((*Rows)[0].getAsObject(), nullptr);
      EXPECT_EQ((*Rows)[0].getAsObject()->getInteger("line"), Offset);
      if (Page.getBoolean("complete").value_or(false)) {
        EXPECT_EQ(Page.getInteger("total_lines"), Offset + 1);
        break;
      }
      ASSERT_EQ(Page.getInteger("next_offset"), Offset + 1);
      ASSERT_LT(++Offset, 1000U);
    }
    EXPECT_EQ(Reassembled, Legacy);
    EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
  }
}

TEST_F(SessionCAPITest, SignatureJSONPreservesASCIINameAndMatchFields) {
  expectSignatureJSONName("ascii_function");
}

TEST_F(SessionCAPITest, SignatureJSONPreservesUnicodeNameAndMatchFields) {
  expectSignatureJSONName("\xe5\x87\xbd\xe6\x95\xb0_caf\xc3\xa9");
}

TEST_F(SessionCAPITest, AnnotationsPreserveExactNumericAddressKeys) {
  const auto Input = write("integer-notes.elf", makeNamedNativeELF("entry"));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const std::map<uint64_t, std::string> Expected = {
      {37, "small"},
      {uint64_t(1) << 53 | 1, "above_binary64_exact_range"},
      {static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
       "signed_maximum"},
      {uint64_t(1) << 63 | 1, "unsigned_low_bit"},
      {std::numeric_limits<uint64_t>::max(), "unsigned_maximum"}};
  std::string Rows = "[";
  for (const auto &[Address, Text] : Expected) {
    if (Rows.size() > 1)
      Rows += ',';
    Rows +=
        "{\"addr\":" + std::to_string(Address) + ",\"text\":\"" + Text + "\"}";
  }
  Rows += ']';
  write("integer-notes.elf.neverd-annotations.json", Rows);
  ASSERT_EQ(neverd_annotations_load(Session), 0);
  for (const auto &[Address, Text] : Expected)
    EXPECT_EQ(takeString(neverd_annotation_get(Session, Address)), Text);
  for (uint64_t Other :
       {uint64_t(1) << 53, uint64_t(1) << 63, (uint64_t(1) << 63) + 2,
        std::numeric_limits<uint64_t>::max() - 1})
    EXPECT_TRUE(takeString(neverd_annotation_get(Session, Other)).empty());
  expectPersistedRows(takeString(neverd_annotations_json(Session)), "text",
                      Expected);
  ASSERT_EQ(neverd_annotations_save(Session), 0);
  expectPersistedRows(readSidecar("integer-notes.elf.neverd-annotations.json"),
                      "text", Expected);
  for (const auto &Item : Expected)
    neverd_annotation_remove(Session, Item.first);
  ASSERT_EQ(neverd_annotations_load(Session), 0);
  expectPersistedRows(takeString(neverd_annotations_json(Session)), "text",
                      Expected);
  for (const auto &[Address, Text] : Expected)
    EXPECT_EQ(takeString(neverd_annotation_get(Session, Address)), Text);
}

TEST_F(SessionCAPITest, NumericRenameReachesExactHighAddressFunction) {
  constexpr uint64_t Base = 0xffff800000000000ULL;
  const auto Input =
      write("integer-rename.elf", makeNamedNativeELF("entry", Base));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  ASSERT_EQ(neverd_func_count(Session), 1);
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  ASSERT_EQ(Entry, Base + sizeof(llvm::object::ELF64LE::Ehdr) +
                       sizeof(llvm::object::ELF64LE::Phdr));
  ASSERT_EQ(neverd_func_entry(Session, 0), Entry);
  ASSERT_EQ(takeString(neverd_func_name(Session, 0)), "entry");
  write("integer-rename.elf.neverd-renames.json",
        "[{\"addr\":" + std::to_string(Entry) +
            ",\"renamed\":\"high_renamed\"}]");
  ASSERT_EQ(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "high_renamed");
  EXPECT_EQ(neverd_func_find_by_name(Session, "high_renamed"), 0);
  EXPECT_EQ(neverd_func_find_by_addr(Session, Entry), 0);
  const std::map<uint64_t, std::string> Expected = {{Entry, "high_renamed"}};
  expectPersistedRows(takeString(neverd_renames_json(Session)), "renamed",
                      Expected);
  ASSERT_EQ(neverd_renames_save(Session), 0);
  expectPersistedRows(readSidecar("integer-rename.elf.neverd-renames.json"),
                      "renamed", Expected);
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  ASSERT_EQ(neverd_func_count(Session), 1);
  EXPECT_EQ(neverd_func_entry(Session, 0), Entry);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "high_renamed");
  EXPECT_EQ(neverd_func_find_by_name(Session, "high_renamed"), 0);
  expectPersistedRows(takeString(neverd_renames_json(Session)), "renamed",
                      Expected);
}

TEST_F(SessionCAPITest, PersistedAddressEncodingsKeepCanonicalSaveRoundTrips) {
  const auto Input = write("encodings.elf", makeNamedNativeELF("entry"));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  ASSERT_EQ(neverd_func_count(Session), 1);
  constexpr uint64_t Entry = 0x400078;
  ASSERT_EQ(neverd_session_entry_addr(Session), Entry);
  ASSERT_EQ(neverd_func_entry(Session, 0), Entry);
  struct Encoding {
    const char *Annotation;
    const char *Rename;
  };
  const Encoding Encodings[] = {{"37", "4194424"},
                                {"37.0", "4194424.0"},
                                {"\"0x25\"", "\"0x400078\""},
                                {"\"25\"", "\"400078\""},
                                {"\"0X25\"", "\"0X400078\""}};
  unsigned Index = 0;
  for (const auto &Value : Encodings) {
    SCOPED_TRACE(Value.Annotation);
    const std::string Text = "note_" + std::to_string(Index);
    const std::string Name = "renamed_" + std::to_string(Index++);
    write("encodings.elf.neverd-annotations.json",
          "[{\"addr\":" + std::string(Value.Annotation) + ",\"text\":\"" +
              Text + "\"}]");
    write("encodings.elf.neverd-renames.json",
          "[{\"addr\":" + std::string(Value.Rename) + ",\"renamed\":\"" + Name +
              "\"}]");
    ASSERT_EQ(neverd_annotations_load(Session), 0);
    ASSERT_EQ(neverd_renames_load(Session), 0);
    EXPECT_EQ(takeString(neverd_annotation_get(Session, 37)), Text);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
    EXPECT_EQ(neverd_func_find_by_name(Session, Name.c_str()), 0);
    const std::map<uint64_t, std::string> Notes = {{37, Text}};
    const std::map<uint64_t, std::string> Renames = {{Entry, Name}};
    expectPersistedRows(takeString(neverd_annotations_json(Session)), "text",
                        Notes);
    expectPersistedRows(takeString(neverd_renames_json(Session)), "renamed",
                        Renames);
    ASSERT_EQ(neverd_annotations_save(Session), 0);
    ASSERT_EQ(neverd_renames_save(Session), 0);
    expectPersistedRows(readSidecar("encodings.elf.neverd-annotations.json"),
                        "text", Notes);
    expectPersistedRows(readSidecar("encodings.elf.neverd-renames.json"),
                        "renamed", Renames);
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
    ASSERT_EQ(neverd_func_count(Session), 1);
    EXPECT_EQ(neverd_func_entry(Session, 0), Entry);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
    EXPECT_EQ(neverd_func_find_by_name(Session, Name.c_str()), 0);
    EXPECT_EQ(takeString(neverd_annotation_get(Session, 37)), Text);
    expectPersistedRows(takeString(neverd_annotations_json(Session)), "text",
                        Notes);
    expectPersistedRows(takeString(neverd_renames_json(Session)), "renamed",
                        Renames);
  }
}

} // namespace
