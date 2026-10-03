//===- SessionCAPITests.cpp - public session failure contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NativeMobileSession.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/ProjectWriteLock.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

TEST(SessionInterpreterRecovery, RejectsTruncatedOptionsBeforeReadingFields) {
  neverd_session_t Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  // Only the size field is accessible through this versioned options prefix.
  size_t SizeOnly = sizeof(size_t);
  const char *Report = nullptr;
  const char *Source = neverd_devirtualize_source_v1(
      Session, 0,
      reinterpret_cast<const neverd_devirtualize_options_v1 *>(&SizeOnly),
      &Report);
  EXPECT_EQ(Source, nullptr);
  ASSERT_NE(Report, nullptr);
  auto Parsed = llvm::json::parse(Report);
  ASSERT_TRUE(static_cast<bool>(Parsed));
  ASSERT_NE(Parsed->getAsObject(), nullptr);
  EXPECT_EQ(Parsed->getAsObject()->getBoolean("complete"), false);
  EXPECT_TRUE(Parsed->getAsObject()->getString("error").has_value());
  neverd_free_string(Report);
  neverd_session_destroy(Session);
}

TEST(SessionInterpreterRecovery, RejectsUnknownFlagsWithoutPublishingSource) {
  neverd_session_t Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  neverd_devirtualize_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.reserved = 1;
  const char *Report = nullptr;
  EXPECT_EQ(neverd_devirtualize_source_v1(Session, 0, &Options, &Report),
            nullptr);
  ASSERT_NE(Report, nullptr);
  EXPECT_NE(std::string(Report).find("invalid devirtualize flags"),
            std::string::npos);
  neverd_free_string(Report);
  neverd_session_destroy(Session);
}

TEST(SessionInterpreterRecovery, NullSessionClearsTheReportDestination) {
  const char *Report = "previous";
  EXPECT_EQ(neverd_devirtualize_source_v1(nullptr, 0, nullptr, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
}

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Text(Value);
  neverd_free_string(Value);
  return Text;
}

TEST(SessionARMFunctionMode, RejectsInvalidModeAndAcceptsObjectAddressZero) {
  EXPECT_EQ(neverd_session_set_arm_function_mode(
                nullptr, 0, NEVERD_ARM_FUNCTION_MODE_THUMB),
            0);
  neverd_session_t Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  EXPECT_EQ(neverd_session_set_arm_function_mode(Session, 0, 99), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("ARM function mode"),
            std::string::npos);
  EXPECT_EQ(neverd_session_set_arm_function_mode(
                Session, 0, NEVERD_ARM_FUNCTION_MODE_THUMB),
            1);
  EXPECT_EQ(neverd_session_set_arm_function_mode(
                Session, 0, NEVERD_ARM_FUNCTION_MODE_CLEAR),
            1);
  neverd_session_destroy(Session);
}

// Owned linked Mach-O bytes, not a Swift compiler fixture. The symbol's raw
// bytes make the pre-normalization observer boundary directly observable.
std::string makeObservedMachO(bool AArch64, const std::string &Name) {
  using namespace llvm::MachO;
  constexpr uint64_t Base = 0x100000000ULL;
  constexpr uint32_t CodeOffset = 0x400;
  constexpr uint32_t SymbolsOffset = 0x1000;
  constexpr uint32_t StringsOffset = 0x1010;
  constexpr uint32_t TextCommandSize =
      sizeof(segment_command_64) + sizeof(section_64);
  constexpr uint32_t CommandsSize =
      TextCommandSize + sizeof(segment_command_64) + sizeof(symtab_command) +
      sizeof(entry_point_command);
  static_assert(sizeof(mach_header_64) + CommandsSize < CodeOffset);
  if (Name.empty() || Name.size() > 127 || Name.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid owned Mach-O fixture symbol");
  const std::string Code =
      AArch64 ? std::string("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
              : std::string("\xb8\x07\x00\x00\x00\xc3", 6);
  std::string Bytes(0x1200, '\0');
  const auto Put = [&Bytes](size_t Offset, const auto &Object) {
    std::memcpy(Bytes.data() + Offset, &Object, sizeof(Object));
  };
  mach_header_64 Header{};
  Header.magic = MH_MAGIC_64;
  Header.cputype = AArch64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
  Header.cpusubtype = AArch64 ? uint32_t(CPU_SUBTYPE_ARM64_ALL)
                              : uint32_t(CPU_SUBTYPE_X86_64_ALL);
  Header.filetype = MH_EXECUTE;
  Header.ncmds = 4;
  Header.sizeofcmds = CommandsSize;
  Header.flags = MH_NOUNDEFS;
  Put(0, Header);
  size_t Command = sizeof(Header);
  segment_command_64 Segment{};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = TextCommandSize;
  std::memcpy(Segment.segname, "__TEXT", 6);
  Segment.vmaddr = Base;
  Segment.vmsize = Segment.filesize = 0x1000;
  Segment.maxprot = Segment.initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Segment.nsects = 1;
  Put(Command, Segment);
  section_64 Section{};
  std::memcpy(Section.sectname, "__text", 6);
  std::memcpy(Section.segname, "__TEXT", 6);
  Section.addr = Base + CodeOffset;
  Section.size = Code.size();
  Section.offset = CodeOffset;
  Section.align = AArch64 ? 2 : 0;
  Section.flags = static_cast<uint32_t>(S_REGULAR) |
                  static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS);
  Put(Command + sizeof(Segment), Section);
  Command += TextCommandSize;
  Segment = {};
  Segment.cmd = LC_SEGMENT_64;
  Segment.cmdsize = sizeof(Segment);
  std::memcpy(Segment.segname, "__LINKEDIT", 10);
  Segment.vmaddr = Base + SymbolsOffset;
  Segment.fileoff = SymbolsOffset;
  Segment.vmsize = Segment.filesize = 0x200;
  Segment.maxprot = Segment.initprot = VM_PROT_READ;
  Put(Command, Segment);
  Command += sizeof(Segment);
  symtab_command Symbols{};
  Symbols.cmd = LC_SYMTAB;
  Symbols.cmdsize = sizeof(Symbols);
  Symbols.symoff = SymbolsOffset;
  Symbols.nsyms = 1;
  Symbols.stroff = StringsOffset;
  Symbols.strsize = static_cast<uint32_t>(Name.size() + 2);
  Put(Command, Symbols);
  Command += sizeof(Symbols);
  entry_point_command Main{};
  Main.cmd = LC_MAIN;
  Main.cmdsize = sizeof(Main);
  Main.entryoff = CodeOffset;
  Put(Command, Main);
  nlist_64 Symbol{};
  Symbol.n_strx = 1;
  Symbol.n_type = static_cast<uint8_t>(N_SECT) | static_cast<uint8_t>(N_EXT);
  Symbol.n_sect = 1;
  Symbol.n_value = Base + CodeOffset;
  Put(SymbolsOffset, Symbol);
  Bytes.replace(CodeOffset, Code.size(), Code);
  Bytes.replace(StringsOffset + 1, Name.size(), Name);
  return Bytes;
}

struct ObservedMobileImage {
  unsigned Calls = 0;
  neverd::BinaryFormat Format = neverd::BinaryFormat::Unknown;
  neverd::Arch Architecture = neverd::Arch::Unknown;
  std::string Raw;
  std::vector<std::string> Names;

  static void observe(const neverd::BinaryImage &Image, void *Context) {
    auto &Snapshot = *static_cast<ObservedMobileImage *>(Context);
    ++Snapshot.Calls;
    Snapshot.Format = Image.Format;
    Snapshot.Architecture = Image.Arch;
    Snapshot.Raw.assign(reinterpret_cast<const char *>(Image.Raw.data()),
                        Image.Raw.size());
    Snapshot.Names.clear();
    for (const auto &Symbol : Image.Symbols)
      Snapshot.Names.push_back(Symbol.Name);
  }
};

class MobileObserverFailure : public std::runtime_error {
public:
  MobileObserverFailure()
      : std::runtime_error("owned mobile metadata failure") {}
};

class ScopedNativePhaseEnvironment {
public:
  ScopedNativePhaseEnvironment() {
    if (const char *Value = std::getenv("NEVERD_NATIVE_PHASES")) {
      HadValue = true;
      Original = Value;
    }
  }

  ~ScopedNativePhaseEnvironment() {
    EXPECT_EQ(set(HadValue ? Original.c_str() : nullptr), 0);
  }

  int set(const char *Value) const {
#ifdef _WIN32
    return _putenv_s("NEVERD_NATIVE_PHASES", Value ? Value : "");
#else
    return Value ? setenv("NEVERD_NATIVE_PHASES", Value, 1)
                 : unsetenv("NEVERD_NATIVE_PHASES");
#endif
  }

private:
  bool HadValue = false;
  std::string Original;
};

struct CapturedPhaseLoad {
  int Status;
  std::string Diagnostic;
  std::string Error;
};

CapturedPhaseLoad capturePhaseLoad(neverd_session_t Session, const char *Path) {
  testing::internal::CaptureStderr();
  const int Status = neverd_session_load(Session, Path);
  std::string Diagnostic = testing::internal::GetCapturedStderr();
  return {Status, std::move(Diagnostic),
          takeString(neverd_last_error(Session))};
}

void expectSessionPhasePair(const std::string &Diagnostic, const char *Event) {
  llvm::StringRef Remaining(Diagnostic);
  ASSERT_TRUE(Remaining.consume_front(
      "[neverd-child-phase] phase=session_load event=begin iteration=0 "
      "elapsed_ms=0\n"))
      << Diagnostic;
  const std::string End =
      std::string("[neverd-child-phase] phase=session_load event=") + Event +
      " iteration=0 elapsed_ms=";
  ASSERT_TRUE(Remaining.consume_front(End)) << Diagnostic;
  ASSERT_TRUE(Remaining.consume_back("\n")) << Diagnostic;
  ASSERT_FALSE(Remaining.empty());
  EXPECT_TRUE(std::all_of(Remaining.begin(), Remaining.end(), [](char C) {
    return C >= '0' && C <= '9';
  })) << Diagnostic;
  uint64_t Elapsed = 0;
  EXPECT_FALSE(Remaining.getAsInteger(10, Elapsed)) << Diagnostic;
  // The elapsed value has no timing threshold. Exact consumption forbids
  // extra records, path/error disclosure, or a completed/aborted failure.
}

#ifndef _WIN32
int exerciseFailedPhaseSink(neverd_session_t Session, const std::string &Input,
                            const std::string &Missing, bool Full,
                            bool Enabled) {
  if (unsetenv("NEVERD_NATIVE_PHASES") != 0)
    return 10;
  if (neverd_session_load(Session, Input.c_str()) != 1)
    return 11;
  const std::string BeforeError = takeString(neverd_last_error(Session));
  const std::string BeforeHeaders = takeString(neverd_headers_json(Session));
  if (!BeforeError.empty() || BeforeHeaders.empty() || llvm::errs().has_error())
    return 12;
  if (setenv("NEVERD_NATIVE_PHASES", Enabled ? "1" : "0", 1) != 0)
    return 13;
  const int SavedStderr = dup(STDERR_FILENO);
  if (SavedStderr < 0)
    return 14;
  if (Full) {
    const int Sink = open("/dev/full", O_WRONLY);
    if (Sink < 0 || dup2(Sink, STDERR_FILENO) < 0)
      return 15;
    close(Sink);
  } else if (close(STDERR_FILENO) != 0) {
    return 16;
  }
  errno = 0;
  const bool SinkFailed =
      ::write(STDERR_FILENO, "x", 1) == -1 && errno == (Full ? ENOSPC : EBADF);
  const int Loaded = neverd_session_load(Session, Input.c_str());
  const std::string LoadedError = takeString(neverd_last_error(Session));
  const std::string Headers = takeString(neverd_headers_json(Session));
  const int Failed = neverd_session_load(Session, Missing.c_str());
  const std::string Failure = takeString(neverd_last_error(Session));
  const bool PreservedImage =
      neverd_session_is_loaded(Session) == 1 &&
      takeString(neverd_session_file_path(Session)) == Input;
  const int Restored = dup2(SavedStderr, STDERR_FILENO);
  close(SavedStderr);
  if (Restored < 0)
    return 17;
  if (!SinkFailed || Loaded != 1 || LoadedError != BeforeError ||
      Headers != BeforeHeaders || Failed != 0 ||
      Failure != "file not found: " + Missing || !PreservedImage)
    return 18;
  // Do not clear global stream errors: that would hide the v1 regression.
  if (llvm::errs().has_error())
    return 19;
  llvm::errs() << "phase-sink-restored\n";
  llvm::errs().flush();
  return llvm::errs().has_error() ? 20 : 0;
}
#endif

// A sectionless executable with one RX segment and a two-instruction body.
// Keeping both ISAs in the fixture makes decoder state observable on reload.
std::string makeNativeELF(bool AArch64, uint64_t Base = 0x400000,
                          std::string_view TrailingCode = {},
                          std::string_view Body = {}) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Code = !Body.empty() ? std::string(Body)
                     : AArch64
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
                               uint64_t Base = 0x400000, bool AArch64 = false) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Bytes = makeNativeELF(AArch64, Base);
  const uint64_t CodeSize = AArch64 ? 8 : 6;
  ELF::Ehdr Header{};
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  std::array<ELF::Shdr, 5> Sections{};
  Sections[1].sh_name = 1;
  Sections[1].sh_type = SHT_PROGBITS;
  Sections[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[1].sh_addr = Header.e_entry;
  Sections[1].sh_offset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  Sections[1].sh_size = CodeSize;
  Sections[1].sh_addralign = AArch64 ? 4 : 1;

  Bytes.resize((Bytes.size() + 7) & ~size_t(7), '\0');
  std::array<ELF::Sym, 2> Symbols{};
  Symbols[1].st_name = 1;
  Symbols[1].setBindingAndType(STB_GLOBAL, STT_FUNC);
  Symbols[1].st_shndx = 1;
  Symbols[1].st_value = Header.e_entry;
  Symbols[1].st_size = CodeSize;
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

  void expectSignatureJSONName(const std::string &Name,
                               const std::string &Aliases = "",
                               const std::vector<std::string> &Listed = {}) {
    const auto Input =
        write("signature-input.elf", makeNamedNativeELF("entry"));
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
        << takeString(neverd_last_error(Session));
    const neverd_va_t Entry = neverd_session_entry_addr(Session);
    ASSERT_EQ(neverd_func_count(Session), 1);
    const auto Pattern =
        write("signature-library.pat",
              "B807000000C3 00 0000 0006 :0000 " + Name + Aliases + "\n");
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
    EXPECT_EQ(Match->getBoolean("confirmed"), false);
    const auto *AliasArray = Match->getArray("aliases");
    if (Listed.empty()) {
      EXPECT_EQ(AliasArray, nullptr);
    } else {
      ASSERT_NE(AliasArray, nullptr);
      std::vector<std::string> Seen;
      for (const llvm::json::Value &Alias : *AliasArray)
        Seen.push_back(Alias.getAsString().value_or("").str());
      EXPECT_EQ(Seen, Listed);
    }
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

  void expectDecompileDiff(const std::string &OtherCode, bool Identical) {
    const auto Input = write("diff-original.evm", "6001600055");
    const auto OtherInput = write("diff-other.evm", OtherCode);
    std::unique_ptr<void, decltype(&neverd_session_destroy)> Other(
        neverd_session_create(), &neverd_session_destroy);
    ASSERT_NE(Other.get(), nullptr);
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
        << takeString(neverd_last_error(Session));
    ASSERT_EQ(neverd_session_load(Other.get(), OtherInput.c_str()), 1)
        << takeString(neverd_last_error(Other.get()));
    ASSERT_EQ(neverd_session_analyze(Session), 1)
        << takeString(neverd_last_error(Session));
    ASSERT_EQ(neverd_session_analyze(Other.get()), 1)
        << takeString(neverd_last_error(Other.get()));
    const std::string ExpectedA = takeString(neverd_decompile(Session, 0));
    const std::string ExpectedB = takeString(neverd_decompile(Other.get(), 0));
    ASSERT_FALSE(ExpectedA.empty());
    ASSERT_FALSE(ExpectedB.empty());
    ASSERT_EQ(ExpectedA == ExpectedB, Identical);

    auto Parsed = llvm::json::parse(
        takeString(neverd_diff_decompile(Session, 0, Other.get(), 0)));
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    const auto *Object = Parsed->getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getString("code_a"), ExpectedA);
    EXPECT_EQ(Object->getString("code_b"), ExpectedB);
    EXPECT_EQ(Object->getBoolean("identical"), Identical);
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

TEST_F(SessionCAPITest, InterpreterRecoveryV2RejectsTruncationBeforeTailReads) {
  for (auto Recover :
       {neverd_devirtualize_source_v2, neverd_devirtualize_machine_source_v2}) {
    // The v2 entry point must not read beyond a caller's accessible prefix.
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v1 V1Only{};
    V1Only.struct_size = sizeof(V1Only);
    neverd_devirtualize_options_v2 Partial{};
    Partial.base.struct_size = sizeof(Partial) - 1;
    for (const auto *Options :
         {reinterpret_cast<const neverd_devirtualize_options_v2 *>(&SizeOnly),
          reinterpret_cast<const neverd_devirtualize_options_v2 *>(&V1Only),
          static_cast<const neverd_devirtualize_options_v2 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, 0, Options, &Report), nullptr);
      const std::string Text = takeString(Report);
      EXPECT_NE(Text.find("complete v2 structure"), std::string::npos);
    }
  }
}

TEST_F(SessionCAPITest, InterpreterRecoveryV2RejectsBothReservedFields) {
  for (auto Recover :
       {neverd_devirtualize_source_v2, neverd_devirtualize_machine_source_v2}) {
    for (bool Extension : {false, true}) {
      neverd_devirtualize_options_v2 Options{};
      Options.base.struct_size = sizeof(Options);
      if (Extension)
        Options.reserved = 1;
      else
        Options.base.reserved = 1;
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, 0, &Options, &Report), nullptr);
      auto Parsed = llvm::json::parse(takeString(Report));
      ASSERT_TRUE(static_cast<bool>(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getString("error"),
                Extension ? "invalid devirtualize v2 flags"
                          : "invalid devirtualize flags");
    }
    const char *Report = "previous";
    EXPECT_EQ(Recover(nullptr, 0, nullptr, &Report), nullptr);
    EXPECT_EQ(Report, nullptr);
  }
}

TEST_F(SessionCAPITest,
       InterpreterRecoveryVersionsPreserveDefaultsAndFutureTails) {
  const auto Input = write("recovery-options.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  // The v1 layout, including its tail padding, is frozen on the supported
  // 64-bit ABI. v2 adds fields after that complete base, not inside padding.
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v2, base), 0u);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v2, max_control_refinements),
            sizeof(neverd_devirtualize_options_v1));
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v3, base), 0u);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v3, max_control_fields),
            sizeof(neverd_devirtualize_options_v2));
  if (sizeof(void *) == 8) {
    EXPECT_EQ(sizeof(neverd_devirtualize_options_v1), 72u);
    EXPECT_EQ(sizeof(neverd_devirtualize_options_v2), 80u);
    EXPECT_EQ(sizeof(neverd_devirtualize_options_v3), 96u);
  }
  for (bool Machine : {false, true}) {
    SCOPED_TRACE(Machine);
    const auto V1 = Machine ? neverd_devirtualize_machine_source_v1
                            : neverd_devirtualize_source_v1;
    const auto V2 = Machine ? neverd_devirtualize_machine_source_v2
                            : neverd_devirtualize_source_v2;
    const auto V3 = Machine ? neverd_devirtualize_machine_source_v3
                            : neverd_devirtualize_source_v3;
    const auto Check = [&](auto Recover, const auto *Options,
                           uint32_t ExpectedBudget, uint32_t Fields = 16,
                           uint32_t Queries = 4096) {
      const char *Report = nullptr;
      const std::string Source =
          takeString(Recover(Session, Entry, Options, &Report));
      const std::string Evidence = takeString(Report);
      ASSERT_FALSE(Source.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::json::parse(Evidence);
      ASSERT_TRUE(static_cast<bool>(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), true);
      EXPECT_EQ(Object->getInteger("maxControlRefinements"), ExpectedBudget);
      EXPECT_EQ(Object->getInteger("maxControlFields"), Fields);
      EXPECT_EQ(Object->getInteger("maxSolverQueries"), Queries);
      EXPECT_EQ(Object->getString("sourceABI"),
                Machine ? "x64-machine-state-v1" : "ordinary-source");
    };
    Check(V1, static_cast<const neverd_devirtualize_options_v1 *>(nullptr), 16);
    Check(V2, static_cast<const neverd_devirtualize_options_v2 *>(nullptr), 16);
    Check(V3, static_cast<const neverd_devirtualize_options_v3 *>(nullptr), 16);
    neverd_devirtualize_options_v2 Options{};
    Options.base.struct_size = sizeof(Options);
    Check(V2, &Options, 16);
    for (uint32_t Limit : {1u, 37u}) {
      Options.max_control_refinements = Limit;
      Check(V2, &Options, Limit);
    }
    // An older caller/library treats every future byte as opaque, even when
    // it happens to resemble a newer version's budget or reserved field.
    Options.reserved = ~uint32_t{0};
    Check(V1, &Options.base, 16);
    struct FutureOptions {
      neverd_devirtualize_options_v2 Known;
      uint64_t Opaque;
    } Future{};
    Future.Known.base.struct_size = sizeof(Future);
    Future.Known.max_control_refinements = 23;
    Future.Opaque = ~uint64_t{0};
    Check(V2, &Future.Known, 23);

    neverd_devirtualize_options_v3 Budgets{};
    Budgets.base.base.struct_size = sizeof(Budgets);
    Check(V3, &Budgets, 16);
    Budgets.base.max_control_refinements = 29;
    Budgets.max_control_fields = 53;
    Budgets.max_solver_queries = 17000;
    Check(V3, &Budgets, 29, 53, 17000);
    Budgets.reserved = ~uint32_t{0};
    Check(V2, &Budgets.base, 29);
    Check(V1, &Budgets.base.base, 16);
    struct FutureBudgets {
      neverd_devirtualize_options_v3 Known;
      uint64_t Opaque;
    } FutureBudget{};
    FutureBudget.Known.base.base.struct_size = sizeof(FutureBudget);
    FutureBudget.Known.max_control_fields = 31;
    FutureBudget.Known.max_solver_queries = 5000;
    FutureBudget.Opaque = ~uint64_t{0};
    Check(V3, &FutureBudget.Known, 16, 31, 5000);
  }
}

TEST_F(SessionCAPITest,
       InterpreterRecoveryV3RejectsTruncationAndReservedFields) {
  for (auto Recover :
       {neverd_devirtualize_source_v3, neverd_devirtualize_machine_source_v3}) {
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v2 V2Only{};
    V2Only.base.struct_size = sizeof(V2Only);
    neverd_devirtualize_options_v3 Partial{};
    Partial.base.base.struct_size = sizeof(Partial) - 1;
    for (const auto *Options :
         {reinterpret_cast<const neverd_devirtualize_options_v3 *>(&SizeOnly),
          reinterpret_cast<const neverd_devirtualize_options_v3 *>(&V2Only),
          static_cast<const neverd_devirtualize_options_v3 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, 0, Options, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v3 structure"),
                std::string::npos);
    }
    for (unsigned Version = 1; Version <= 3; ++Version) {
      neverd_devirtualize_options_v3 Options{};
      Options.base.base.struct_size = sizeof(Options);
      if (Version == 1)
        Options.base.base.reserved = 1;
      else if (Version == 2)
        Options.base.reserved = 1;
      else
        Options.reserved = 1;
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, 0, &Options, &Report), nullptr);
      auto Parsed = llvm::json::parse(takeString(Report));
      ASSERT_TRUE(static_cast<bool>(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getString("error"),
                Version == 1   ? "invalid devirtualize flags"
                : Version == 2 ? "invalid devirtualize v2 flags"
                               : "invalid devirtualize v3 flags");
    }
    const char *Report = "previous";
    EXPECT_EQ(Recover(nullptr, 0, nullptr, &Report), nullptr);
    EXPECT_EQ(Report, nullptr);
  }
}

TEST_F(SessionCAPITest,
       InterpreterRecoveryV4RejectsTruncatedAndInvalidOptions) {
  for (auto Recover :
       {neverd_devirtualize_source_v4, neverd_devirtualize_machine_source_v4}) {
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v3 V3Only{};
    V3Only.base.base.struct_size = sizeof(V3Only);
    neverd_devirtualize_options_v4 Partial{};
    Partial.base.base.base.struct_size = sizeof(Partial) - 1;
    for (const auto *Options :
         {reinterpret_cast<const neverd_devirtualize_options_v4 *>(&SizeOnly),
          reinterpret_cast<const neverd_devirtualize_options_v4 *>(&V3Only),
          static_cast<const neverd_devirtualize_options_v4 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, 0, Options, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v4 structure"),
                std::string::npos);
    }
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      neverd_devirtualize_options_v4 Options{};
      Options.base.base.base.struct_size = sizeof(Options);
      const char *Expected = nullptr;
      switch (Mutation) {
      case 0:
        Options.base.base.base.reserved = 1;
        Expected = "invalid devirtualize flags";
        break;
      case 1:
        Options.base.base.reserved = 1;
        Expected = "invalid devirtualize v2 flags";
        break;
      case 2:
        Options.base.reserved = 1;
        Expected = "invalid devirtualize v3 flags";
        break;
      case 3:
        Options.flags = 4;
        Expected = "invalid devirtualize v4 flags";
        break;
      case 4:
        Options.entry_frame_begin = -16;
        Expected = "entry frame endpoints require the bounds flag";
        break;
      }
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, 0, &Options, &Report), nullptr);
      auto Parsed = llvm::json::parse(takeString(Report));
      ASSERT_TRUE(static_cast<bool>(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getString("error"), Expected);
    }
    const char *Report = "previous";
    EXPECT_EQ(Recover(nullptr, 0, nullptr, &Report), nullptr);
    EXPECT_EQ(Report, nullptr);
  }
}

TEST_F(SessionCAPITest, InterpreterRecoveryV4PreservesOldAndFutureLayouts) {
  const auto Input = write("recovery-v4-layout.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v4, base), 0u);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v4, max_chained_transfers),
            sizeof(neverd_devirtualize_options_v3));
  if (sizeof(void *) == 8)
    EXPECT_EQ(sizeof(neverd_devirtualize_options_v4), 120u);
  for (bool Machine : {false, true}) {
    const auto V1 = Machine ? neverd_devirtualize_machine_source_v1
                            : neverd_devirtualize_source_v1;
    const auto V2 = Machine ? neverd_devirtualize_machine_source_v2
                            : neverd_devirtualize_source_v2;
    const auto V3 = Machine ? neverd_devirtualize_machine_source_v3
                            : neverd_devirtualize_source_v3;
    const auto V4 = Machine ? neverd_devirtualize_machine_source_v4
                            : neverd_devirtualize_source_v4;
    const auto Check = [&](auto Recover, const auto *Options, uint32_t Chain,
                           bool Discovery) {
      const char *Report = nullptr;
      const auto Source = takeString(Recover(Session, Entry, Options, &Report));
      const auto Evidence = takeString(Report);
      ASSERT_FALSE(Source.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::json::parse(Evidence);
      ASSERT_TRUE(static_cast<bool>(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), true);
      EXPECT_EQ(Object->getInteger("maxChainedTransfers"), Chain);
      EXPECT_EQ(Object->getBoolean("discoverControlState"), Discovery);
      const auto *Bounds = Object->get("entryFrameBounds");
      ASSERT_NE(Bounds, nullptr);
      EXPECT_TRUE(Bounds->getAsNull().has_value());
      EXPECT_EQ(Source.find("Numeric entry-RSP precondition"),
                std::string::npos);
    };
    Check(V4, static_cast<const neverd_devirtualize_options_v4 *>(nullptr), 0,
          true);
    neverd_devirtualize_options_v4 Options{};
    Options.base.base.base.struct_size = sizeof(Options);
    Check(V4, &Options, 0, true);
    Options.max_chained_transfers = 7;
    Options.flags = NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY;
    Check(V4, &Options, 7, false);
    Options.flags = ~uint32_t{0};
    Options.entry_frame_begin = INT64_MIN;
    Options.entry_frame_end = INT64_MAX;
    Check(V1, &Options.base.base.base, 0, true);
    Check(V2, &Options.base.base, 0, true);
    Check(V3, &Options.base, 0, true);
    struct FutureOptions {
      neverd_devirtualize_options_v4 Known;
      uint64_t Opaque;
    } Future{};
    Future.Known.base.base.base.struct_size = sizeof(Future);
    Future.Known.max_chained_transfers = 5;
    Future.Opaque = UINT64_MAX;
    Check(V4, &Future.Known, 5, true);
  }
}

TEST_F(SessionCAPITest,
       InterpreterRecoveryV7ChecksWorkBudgetsAndPreservesLayouts) {
  const auto Input = write("recovery-work-budgets.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v7, max_node_evaluations),
            sizeof(neverd_devirtualize_options_v6));
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v7, max_discovery_visits),
            sizeof(neverd_devirtualize_options_v6) + sizeof(uint32_t));
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v7, flags),
            sizeof(neverd_devirtualize_options_v6) + 2 * sizeof(uint32_t));
  for (auto Recover :
       {neverd_devirtualize_source_v7, neverd_devirtualize_machine_source_v7}) {
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v7 Partial{};
    Partial.base.base.base.base.base.base.struct_size = sizeof(Partial) - 1;
    for (const auto *O :
         {reinterpret_cast<const neverd_devirtualize_options_v7 *>(&SizeOnly),
          static_cast<const neverd_devirtualize_options_v7 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, Entry, O, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v7 structure"),
                std::string::npos);
    }
    const char *NullReport = "previous";
    EXPECT_EQ(Recover(nullptr, Entry, nullptr, &NullReport), nullptr);
    EXPECT_EQ(NullReport, nullptr);
    const char *DefaultReport = nullptr;
    EXPECT_FALSE(
        takeString(Recover(Session, Entry, nullptr, &DefaultReport)).empty());
    auto Default = llvm::json::parse(takeString(DefaultReport));
    ASSERT_TRUE(bool(Default));
    EXPECT_EQ(Default->getAsObject()->getInteger("maxNodeEvaluations"), 16384);
    EXPECT_EQ(Default->getAsObject()->getInteger("maxDiscoveryVisits"), 65536);
    EXPECT_EQ(
        Default->getAsObject()->getBoolean("stopChainingAtRepeatedDestination"),
        false);
    for (bool LLVM : {false, true})
      for (unsigned Case = 0; Case < 3; ++Case) {
        struct Future {
          neverd_devirtualize_options_v7 Options;
          uint64_t Opaque;
        } F{};
        auto &O = F.Options;
        auto &Base = O.base.base.base.base.base.base;
        Base.struct_size = sizeof(F);
        Base.use_llvm = LLVM;
        F.Opaque = UINT64_MAX;
        O.max_node_evaluations = Case == 0 ? 0 : Case == 1 ? 19 : UINT32_MAX;
        O.max_discovery_visits = Case == 0 ? 0 : Case == 1 ? 37 : UINT32_MAX;
        O.flags = Case == 0 ? 0 : NEVERD_DEVIRTUALIZE_V7_STOP_CHAIN_AT_REPEAT;
        O.base.base.base.max_chained_transfers = Case == 2 ? 8 : 0;
        const char *Report = nullptr;
        ASSERT_FALSE(takeString(Recover(Session, Entry, &O, &Report)).empty())
            << takeString(neverd_last_error(Session));
        auto Parsed = llvm::json::parse(takeString(Report));
        ASSERT_TRUE(bool(Parsed));
        EXPECT_EQ(
            Parsed->getAsObject()->getInteger("maxNodeEvaluations"),
            int64_t(O.max_node_evaluations ? O.max_node_evaluations : 16384));
        EXPECT_EQ(
            Parsed->getAsObject()->getInteger("maxDiscoveryVisits"),
            int64_t(O.max_discovery_visits ? O.max_discovery_visits : 65536));
        EXPECT_EQ(Parsed->getAsObject()->getBoolean(
                      "stopChainingAtRepeatedDestination"),
                  Case != 0);
      }
    for (unsigned Bad = 0; Bad < 6; ++Bad) {
      neverd_devirtualize_options_v7 O{};
      O.base.base.base.base.base.base.struct_size = sizeof(O);
      if (Bad == 0)
        O.base.base.base.base.base.base.reserved = 1;
      else if (Bad == 1)
        O.base.base.base.base.base.reserved = 1;
      else if (Bad == 2)
        O.base.base.base.base.reserved = 1;
      else if (Bad == 3)
        O.base.base.base.flags = UINT32_MAX;
      else if (Bad == 4)
        O.base.flags = UINT32_MAX;
      else
        O.flags = UINT32_MAX;
      EXPECT_EQ(Recover(Session, Entry, &O, nullptr), nullptr);
    }
  }
  neverd_devirtualize_options_v7 Future{};
  Future.base.base.base.base.base.base.struct_size = sizeof(Future);
  Future.max_node_evaluations = Future.max_discovery_visits = 1;
  Future.flags = UINT32_MAX;
  const auto CheckOld = [&](auto Recover, const auto *Options) {
    const char *Report = nullptr;
    ASSERT_FALSE(takeString(Recover(Session, Entry, Options, &Report)).empty());
    auto Parsed = llvm::json::parse(takeString(Report));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("maxNodeEvaluations"), 16384);
    EXPECT_EQ(Parsed->getAsObject()->getInteger("maxDiscoveryVisits"), 65536);
    EXPECT_EQ(
        Parsed->getAsObject()->getBoolean("stopChainingAtRepeatedDestination"),
        false);
  };
  CheckOld(neverd_devirtualize_source_v1,
           &Future.base.base.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v1,
           &Future.base.base.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v2, &Future.base.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v2,
           &Future.base.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v3, &Future.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v3, &Future.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v4, &Future.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v4, &Future.base.base.base);
  CheckOld(neverd_devirtualize_source_v5, &Future.base.base);
  CheckOld(neverd_devirtualize_machine_source_v5, &Future.base.base);
  CheckOld(neverd_devirtualize_source_v6, &Future.base);
  CheckOld(neverd_devirtualize_machine_source_v6, &Future.base);
}

TEST_F(SessionCAPITest,
       InterpreterRecoveryV8KeepsDestinationCapsAndOlderTailsSeparate) {
  const auto Input = write("recovery-chain-visits.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v8,
                     max_chained_visits_per_destination),
            sizeof(neverd_devirtualize_options_v7));
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v8, reserved),
            sizeof(neverd_devirtualize_options_v7) + sizeof(uint32_t));
  for (auto Recover :
       {neverd_devirtualize_source_v8, neverd_devirtualize_machine_source_v8}) {
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v8 Partial{};
    Partial.base.base.base.base.base.base.base.struct_size =
        sizeof(Partial) - 1;
    for (const auto *O :
         {reinterpret_cast<const neverd_devirtualize_options_v8 *>(&SizeOnly),
          static_cast<const neverd_devirtualize_options_v8 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, Entry, O, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v8 structure"),
                std::string::npos);
    }
    const char *NullReport = "previous";
    EXPECT_EQ(Recover(nullptr, Entry, nullptr, &NullReport), nullptr);
    EXPECT_EQ(NullReport, nullptr);
    const char *DefaultReport = nullptr;
    EXPECT_FALSE(
        takeString(Recover(Session, Entry, nullptr, &DefaultReport)).empty());
    auto Default = llvm::json::parse(takeString(DefaultReport));
    ASSERT_TRUE(bool(Default));
    EXPECT_EQ(
        Default->getAsObject()->getInteger("maxChainedVisitsPerDestination"),
        0);
    EXPECT_EQ(Default->getAsObject()->getInteger(
                  "effectiveChainedVisitsPerDestination"),
              0);
    for (bool LLVM : {false, true})
      for (bool Chain : {false, true})
        for (bool Legacy : {false, true})
          for (uint32_t Visits : {0u, 1u, 2u, UINT32_MAX}) {
            struct Future {
              neverd_devirtualize_options_v8 Options;
              uint64_t Opaque;
            } F{};
            auto &O = F.Options;
            auto &Base = O.base.base.base.base.base.base.base;
            Base.struct_size = sizeof(F);
            Base.use_llvm = LLVM;
            F.Opaque = UINT64_MAX;
            O.max_chained_visits_per_destination = Visits;
            O.base.flags =
                Legacy ? NEVERD_DEVIRTUALIZE_V7_STOP_CHAIN_AT_REPEAT : 0;
            O.base.base.base.base.max_chained_transfers = Chain ? 8 : 0;
            const char *Report = nullptr;
            ASSERT_FALSE(
                takeString(Recover(Session, Entry, &O, &Report)).empty())
                << takeString(neverd_last_error(Session));
            auto Parsed = llvm::json::parse(takeString(Report));
            ASSERT_TRUE(bool(Parsed));
            EXPECT_EQ(Parsed->getAsObject()->getInteger(
                          "maxChainedVisitsPerDestination"),
                      Visits);
            EXPECT_EQ(Parsed->getAsObject()->getInteger(
                          "effectiveChainedVisitsPerDestination"),
                      Chain ? Legacy ? 1 : Visits : 0);
            EXPECT_EQ(Parsed->getAsObject()->getBoolean(
                          "stopChainingAtRepeatedDestination"),
                      Legacy);
          }
    for (unsigned Bad = 0; Bad != 8; ++Bad) {
      neverd_devirtualize_options_v8 O{};
      O.base.base.base.base.base.base.base.struct_size = sizeof(O);
      if (Bad == 0)
        O.reserved = 1;
      else if (Bad == 1)
        O.base.flags = UINT32_MAX;
      else if (Bad == 2)
        O.base.base.flags = UINT32_MAX;
      else if (Bad == 3)
        O.base.base.base.entry_frame_residue = 1;
      else if (Bad == 4)
        O.base.base.base.base.flags = UINT32_MAX;
      else if (Bad == 5)
        O.base.base.base.base.base.reserved = 1;
      else if (Bad == 6)
        O.base.base.base.base.base.base.reserved = 1;
      else
        O.base.base.base.base.base.base.base.reserved = 1;
      EXPECT_EQ(Recover(Session, Entry, &O, nullptr), nullptr);
    }
  }
  neverd_devirtualize_options_v8 Future{};
  Future.base.base.base.base.base.base.base.struct_size = sizeof(Future);
  Future.max_chained_visits_per_destination = UINT32_MAX;
  Future.reserved = UINT32_MAX;
  const auto CheckOld = [&](auto Recover, const auto *Options) {
    const char *Report = nullptr;
    ASSERT_FALSE(takeString(Recover(Session, Entry, Options, &Report)).empty());
    auto Parsed = llvm::json::parse(takeString(Report));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(
        Parsed->getAsObject()->getInteger("maxChainedVisitsPerDestination"), 0);
    EXPECT_EQ(Parsed->getAsObject()->getInteger(
                  "effectiveChainedVisitsPerDestination"),
              0);
  };
  CheckOld(neverd_devirtualize_source_v1,
           &Future.base.base.base.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v1,
           &Future.base.base.base.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v2,
           &Future.base.base.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v2,
           &Future.base.base.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v3, &Future.base.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v3,
           &Future.base.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v4, &Future.base.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v4, &Future.base.base.base.base);
  CheckOld(neverd_devirtualize_source_v5, &Future.base.base.base);
  CheckOld(neverd_devirtualize_machine_source_v5, &Future.base.base.base);
  CheckOld(neverd_devirtualize_source_v6, &Future.base.base);
  CheckOld(neverd_devirtualize_machine_source_v6, &Future.base.base);
  CheckOld(neverd_devirtualize_source_v7, &Future.base);
  CheckOld(neverd_devirtualize_machine_source_v7, &Future.base);
}

TEST_F(SessionCAPITest,
       InterpreterRecoveryV6ChecksSeparationAndPreservesLayouts) {
  const auto Input = write("recovery-separation.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v6, flags),
            sizeof(neverd_devirtualize_options_v5));
  for (auto Recover :
       {neverd_devirtualize_source_v6, neverd_devirtualize_machine_source_v6}) {
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v6 Partial{};
    Partial.base.base.base.base.base.struct_size = sizeof(Partial) - 1;
    for (const auto *O :
         {reinterpret_cast<const neverd_devirtualize_options_v6 *>(&SizeOnly),
          static_cast<const neverd_devirtualize_options_v6 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, Entry, O, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v6 structure"),
                std::string::npos);
    }
    EXPECT_FALSE(takeString(Recover(Session, Entry, nullptr, nullptr)).empty());
    const char *Report = "previous";
    EXPECT_EQ(Recover(nullptr, Entry, nullptr, &Report), nullptr);
    EXPECT_EQ(Report, nullptr);
  }
  for (bool LLVM : {false, true}) {
    struct Future {
      neverd_devirtualize_options_v6 Options;
      uint64_t Opaque;
    } F{};
    auto &O = F.Options;
    O.base.base.base.base.base.struct_size = sizeof(F);
    O.base.base.base.base.base.use_llvm = LLVM;
    F.Opaque = UINT64_MAX;
    O.flags = NEVERD_DEVIRTUALIZE_V6_EXTERNAL_STORES_DISJOINT_ENTRY_FRAME;
    EXPECT_EQ(
        neverd_devirtualize_machine_source_v6(Session, Entry, &O, nullptr),
        nullptr);
    O.base.base.flags = NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
    O.base.base.entry_frame_begin = -32;
    O.base.base.entry_frame_end = 8;
    const char *Report = nullptr;
    const auto Source = takeString(
        neverd_devirtualize_machine_source_v6(Session, Entry, &O, &Report));
    ASSERT_FALSE(Source.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::json::parse(takeString(Report));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(
        Parsed->getAsObject()->getBoolean("externalStoresDisjointEntryFrame"),
        true);
    EXPECT_NE(Source.find("Unchecked external-STORE precondition"),
              std::string::npos);
    EXPECT_EQ(neverd_devirtualize_source_v6(Session, Entry, &O, nullptr),
              nullptr);
    O.flags = 2;
    EXPECT_EQ(
        neverd_devirtualize_machine_source_v6(Session, Entry, &O, nullptr),
        nullptr);
    O.base.base.flags = 0;
    O.base.base.entry_frame_begin = O.base.base.entry_frame_end = 0;
    const auto CheckOld = [&](auto Recover, const auto *Options) {
      const char *OldReport = nullptr;
      EXPECT_FALSE(
          takeString(Recover(Session, Entry, Options, &OldReport)).empty());
      auto Old = llvm::json::parse(takeString(OldReport));
      ASSERT_TRUE(bool(Old));
      EXPECT_EQ(
          Old->getAsObject()->getBoolean("externalStoresDisjointEntryFrame"),
          false);
    };
    CheckOld(neverd_devirtualize_machine_source_v1,
             &O.base.base.base.base.base);
    CheckOld(neverd_devirtualize_machine_source_v2, &O.base.base.base.base);
    CheckOld(neverd_devirtualize_machine_source_v3, &O.base.base.base);
    CheckOld(neverd_devirtualize_machine_source_v4, &O.base.base);
    CheckOld(neverd_devirtualize_machine_source_v5, &O.base);
  }
}

TEST_F(SessionCAPITest, InterpreterRecoveryV5ChecksDomainAndPreservesLayouts) {
  const auto Input = write("recovery-congruence.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  EXPECT_EQ(offsetof(neverd_devirtualize_options_v5, entry_frame_alignment),
            sizeof(neverd_devirtualize_options_v4));
  for (auto Recover :
       {neverd_devirtualize_source_v5, neverd_devirtualize_machine_source_v5}) {
    size_t SizeOnly = sizeof(size_t);
    neverd_devirtualize_options_v5 Partial{};
    Partial.base.base.base.base.struct_size = sizeof(Partial) - 1;
    for (const auto *O :
         {reinterpret_cast<const neverd_devirtualize_options_v5 *>(&SizeOnly),
          static_cast<const neverd_devirtualize_options_v5 *>(&Partial)}) {
      const char *Report = nullptr;
      EXPECT_EQ(Recover(Session, Entry, O, &Report), nullptr);
      EXPECT_NE(takeString(Report).find("complete v5 structure"),
                std::string::npos);
    }
    EXPECT_FALSE(takeString(Recover(Session, Entry, nullptr, nullptr)).empty());
    const char *Report = "previous";
    EXPECT_EQ(Recover(nullptr, Entry, nullptr, &Report), nullptr);
    EXPECT_EQ(Report, nullptr);
  }
  for (bool LLVM : {false, true}) {
    struct Future {
      neverd_devirtualize_options_v5 Options;
      uint64_t Opaque;
    } F{};
    auto &O = F.Options;
    O.base.base.base.base.struct_size = sizeof(F);
    O.base.base.base.base.use_llvm = LLVM;
    F.Opaque = UINT64_MAX;
    for (auto [Alignment, Residue] :
         {std::pair{16u, 3u}, {1u, 0u}, {0x80000000u, 0x7fffffffu}}) {
      O.entry_frame_alignment = Alignment;
      O.entry_frame_residue = Residue;
      const char *Report = nullptr;
      const auto Source = takeString(
          neverd_devirtualize_machine_source_v5(Session, Entry, &O, &Report));
      ASSERT_FALSE(Source.empty()) << takeString(neverd_last_error(Session));
      auto Parsed = llvm::json::parse(takeString(Report));
      ASSERT_TRUE(bool(Parsed));
      const auto *Domain =
          Parsed->getAsObject()->getObject("entryFrameAlignment");
      ASSERT_NE(Domain, nullptr);
      EXPECT_EQ(Domain->getInteger("alignment"), Alignment);
      EXPECT_EQ(Domain->getInteger("residue"), Residue);
      EXPECT_NE(Source.find("Checked entry RSP modulo"), std::string::npos);
      EXPECT_EQ(neverd_devirtualize_source_v5(Session, Entry, &O, nullptr),
                nullptr);
    }
    for (auto [Alignment, Residue] :
         {std::pair{0u, 1u}, {3u, 0u}, {8u, 8u}, {8u, 9u}}) {
      O.entry_frame_alignment = Alignment;
      O.entry_frame_residue = Residue;
      EXPECT_EQ(
          neverd_devirtualize_machine_source_v5(Session, Entry, &O, nullptr),
          nullptr);
    }
    // Old entry points ignore even an invalid v5 tail.
    const auto CheckOld = [&](auto Recover, const auto *Options) {
      const char *Report = nullptr;
      ASSERT_FALSE(
          takeString(Recover(Session, Entry, Options, &Report)).empty());
      auto Parsed = llvm::json::parse(takeString(Report));
      ASSERT_TRUE(bool(Parsed));
      EXPECT_TRUE(Parsed->getAsObject()
                      ->get("entryFrameAlignment")
                      ->getAsNull()
                      .has_value());
    };
    CheckOld(neverd_devirtualize_machine_source_v1, &O.base.base.base.base);
    CheckOld(neverd_devirtualize_machine_source_v2, &O.base.base.base);
    CheckOld(neverd_devirtualize_machine_source_v3, &O.base.base);
    CheckOld(neverd_devirtualize_machine_source_v4, &O.base);
    O.entry_frame_alignment = O.entry_frame_residue = 0;
    O.max_symbolic_nodes = 1;
    const char *Report = nullptr;
    EXPECT_EQ(
        neverd_devirtualize_machine_source_v5(Session, Entry, &O, &Report),
        nullptr);
    auto Parsed = llvm::json::parse(takeString(Report));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("maxSymbolicNodes"), 1);
    EXPECT_EQ(Parsed->getAsObject()->getString("status"), "budget-exceeded");
    CheckOld(neverd_devirtualize_machine_source_v4, &O.base);
  }
}

TEST_F(SessionCAPITest, InterpreterRecoveryV4PublishesUncheckedBoundsInSource) {
  const auto Input = write("recovery-v4-domain.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  for (bool LLVM : {false, true}) {
    neverd_devirtualize_options_v4 Options{};
    Options.base.base.base.struct_size = sizeof(Options);
    Options.base.base.base.use_llvm = LLVM;
    Options.flags = NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
    for (int64_t Begin : {int64_t{-32}, INT64_MIN}) {
      Options.entry_frame_begin = Begin;
      Options.entry_frame_end = 8;
      const char *Report = nullptr;
      const auto Source = takeString(neverd_devirtualize_machine_source_v4(
          Session, Entry, &Options, &Report));
      const auto Evidence = takeString(Report);
      ASSERT_FALSE(Source.empty()) << takeString(neverd_last_error(Session));
      EXPECT_NE(Source.find("[" + std::to_string(Begin) + ",8)"),
                std::string::npos);
      EXPECT_NE(Source.find("not checked at runtime"), std::string::npos);
      auto Parsed = llvm::json::parse(Evidence);
      ASSERT_TRUE(static_cast<bool>(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      const auto *Bounds = Object->getObject("entryFrameBounds");
      ASSERT_NE(Bounds, nullptr);
      EXPECT_EQ(Bounds->getInteger("begin"), Begin);
      EXPECT_EQ(Bounds->getInteger("end"), 8);
      EXPECT_EQ(takeString(neverd_devirtualize_machine_source_v4(
                    Session, Entry, &Options, nullptr)),
                Source);
      EXPECT_EQ(
          neverd_devirtualize_source_v4(Session, Entry, &Options, nullptr),
          nullptr);
      EXPECT_NE(
          takeString(neverd_last_error(Session)).find("machine-state API"),
          std::string::npos);
    }
    for (int64_t End : {int64_t{8}, int64_t{7}}) {
      Options.entry_frame_begin = 8;
      Options.entry_frame_end = End;
      EXPECT_EQ(neverd_devirtualize_machine_source_v4(Session, Entry, &Options,
                                                      nullptr),
                nullptr);
      EXPECT_NE(takeString(neverd_last_error(Session)).find("frame"),
                std::string::npos);
    }
  }
}

TEST_F(SessionCAPITest,
       NativeMobileObserverSeesRawImageBeforeNormalizedPublication) {
  ScopedNativePhaseEnvironment Environment;
  ASSERT_EQ(Environment.set("1"), 0);
  const std::string RawName = "_owned_\xff";
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64);
    const auto Bytes = makeObservedMachO(AArch64, RawName);
    const auto Input =
        write(AArch64 ? "observed-arm64.macho" : "observed-x64.macho", Bytes);
    ObservedMobileImage Observed;
    testing::internal::CaptureStderr();
    const int Status = neverd::sdk::loadNativeMobileSession(
        Session, Input.c_str(), ObservedMobileImage::observe, &Observed);
    const auto Trace = testing::internal::GetCapturedStderr();
    ASSERT_EQ(Status, 1) << takeString(neverd_last_error(Session));
    expectSessionPhasePair(Trace, "completed");
    ASSERT_EQ(Observed.Calls, 1U);
    EXPECT_EQ(Observed.Format, neverd::BinaryFormat::MachO);
    EXPECT_EQ(Observed.Architecture,
              AArch64 ? neverd::Arch::AArch64 : neverd::Arch::X64);
    EXPECT_EQ(Observed.Raw, Bytes);
    EXPECT_EQ(Observed.Names, std::vector<std::string>{RawName});
    ASSERT_EQ(neverd_func_count(Session), 1);
    EXPECT_EQ(neverd_func_entry(Session, 0), 0x100000400ULL);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "_owned_\\xFF");
    EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());

    std::unique_ptr<void, decltype(&neverd_session_destroy)> Public(
        neverd_session_create(), &neverd_session_destroy);
    ASSERT_NE(Public.get(), nullptr);
    ASSERT_EQ(neverd_session_load(Public.get(), Input.c_str()), 1)
        << takeString(neverd_last_error(Public.get()));
    EXPECT_EQ(takeString(neverd_symbols_json(Session)),
              takeString(neverd_symbols_json(Public.get())));
    EXPECT_EQ(takeString(neverd_headers_json(Session)),
              takeString(neverd_headers_json(Public.get())));
    EXPECT_EQ(takeString(neverd_disasm_json(Session, 0x100000400ULL, 2)),
              takeString(neverd_disasm_json(Public.get(), 0x100000400ULL, 2)));
  }
}

TEST_F(SessionCAPITest, NativeMobileLoadRejectsBeforeCallingTheObserver) {
  ScopedNativePhaseEnvironment Environment;
  ASSERT_EQ(Environment.set("1"), 0);
  const auto Valid =
      write("valid-mobile.macho", makeObservedMachO(false, "_owned"));
  const auto Invalid = write("invalid-mobile.macho", "not a Mach-O file");
  const auto ELF = write("other-format.elf", makeNativeELF(false));
  const auto Missing = (Directory / "missing-mobile.macho").string();
  ObservedMobileImage Observed;
  testing::internal::CaptureStderr();
  const int NullStatus = neverd::sdk::loadNativeMobileSession(
      nullptr, Valid.c_str(), ObservedMobileImage::observe, &Observed);
  const auto NullTrace = testing::internal::GetCapturedStderr();
  EXPECT_EQ(NullStatus, 0);
  EXPECT_TRUE(NullTrace.empty());
  EXPECT_EQ(Observed.Calls, 0U);
  struct RejectedInput {
    const char *Path;
    neverd::sdk::NativeMobileMetadataObserver Observer;
    std::string ErrorPrefix;
  };
  const RejectedInput Inputs[] = {
      {nullptr, nullptr, "input path is empty"},
      {"", ObservedMobileImage::observe, "input path is empty"},
      {Valid.c_str(), nullptr, "mobile metadata observer is missing"},
      {Missing.c_str(), ObservedMobileImage::observe, "file not found: "},
      {Invalid.c_str(), ObservedMobileImage::observe,
       "invalid selected Mach-O: "},
      {ELF.c_str(), ObservedMobileImage::observe, "invalid selected Mach-O: "},
  };
  for (const auto &Input : Inputs) {
    SCOPED_TRACE(Input.ErrorPrefix);
    testing::internal::CaptureStderr();
    const int Status = neverd::sdk::loadNativeMobileSession(
        Session, Input.Path, Input.Observer, &Observed);
    const auto Trace = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Status, 0);
    expectSessionPhasePair(Trace, "failed");
    EXPECT_TRUE(
        takeString(neverd_last_error(Session)).starts_with(Input.ErrorPrefix));
    EXPECT_EQ(Observed.Calls, 0U);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    EXPECT_EQ(neverd_func_count(Session), 0);
  }
  ASSERT_EQ(neverd::sdk::loadNativeMobileSession(Session, Valid.c_str(),
                                                 ObservedMobileImage::observe,
                                                 &Observed),
            1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(Observed.Calls, 1U);
  EXPECT_EQ(neverd_session_is_loaded(Session), 1);
}

TEST_F(SessionCAPITest,
       NativeMobileObserverFailurePreservesLoadedAnalysisAndEdits) {
  ScopedNativePhaseEnvironment Environment;
  ASSERT_EQ(Environment.set("0"), 0);
  const auto Original = write("mobile-old.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  const auto IR = takeString(neverd_ir_llvm(Session, 0));
  const auto Source = takeString(neverd_decompile(Session, 0));
  ASSERT_FALSE(IR.empty());
  ASSERT_FALSE(Source.empty());
  neverd_annotation_set(Session, 0, "keep observer failure comment");
  ASSERT_EQ(neverd_rename_func(Session, "evm_entry", "retained_entry"), 0);
  const auto Renames = takeString(neverd_renames_json(Session));
  const auto MissingMap = (Directory / "missing-observer.map").string();
  neverd_session_set_map_path(Session, MissingMap.c_str());
  ASSERT_EQ(Environment.set("1"), 0);
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64);
    const auto Bytes = makeObservedMachO(AArch64, "_replacement");
    const auto Replacement = write("observer-replacement.macho", Bytes);
    ObservedMobileImage Observed;
    // This exact exception must win over the configured missing debug file.
    const auto Reject = [](const neverd::BinaryImage &Image, void *Context) {
      ObservedMobileImage::observe(Image, Context);
      throw MobileObserverFailure();
    };
    testing::internal::CaptureStderr();
    EXPECT_THROW(neverd::sdk::loadNativeMobileSession(
                     Session, Replacement.c_str(), Reject, &Observed),
                 MobileObserverFailure);
    const auto Trace = testing::internal::GetCapturedStderr();
    expectSessionPhasePair(Trace, "aborted");
    EXPECT_EQ(Observed.Calls, 1U);
    EXPECT_EQ(Observed.Raw, Bytes);
    EXPECT_EQ(neverd_session_is_loaded(Session), 1);
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Original);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "retained_entry");
    EXPECT_EQ(takeString(neverd_annotation_get(Session, 0)),
              "keep observer failure comment");
    EXPECT_EQ(takeString(neverd_renames_json(Session)), Renames);
    EXPECT_EQ(neverd_session_analyze(Session), 1);
    EXPECT_EQ(takeString(neverd_ir_llvm(Session, 0)), IR);
    EXPECT_EQ(takeString(neverd_decompile(Session, 0)), Source);
    std::array<unsigned char, 2> BytesAtEntry{};
    EXPECT_EQ(
        neverd_read_bytes(Session, 0, BytesAtEntry.data(), BytesAtEntry.size()),
        2);
    EXPECT_EQ(BytesAtEntry[1], 1);
  }
}

TEST_F(SessionCAPITest,
       NativeMobileDebugFailureRetainsDecoderAndAllowsReplacement) {
  ScopedNativePhaseEnvironment Environment;
  ASSERT_EQ(Environment.set("0"), 0);
  const auto Original = write("mobile-before.elf", makeNativeELF(false));
  const auto Map = write("mobile-selected.map",
                         "VMA LMA Size Align Out In Symbol\n"
                         "00400078 00400078 00000006 1 .text\n"
                         "00400078 00400078 00000006 1 kept_function\n");
  neverd_session_set_map_path(Session, Map.c_str());
  ASSERT_EQ(neverd_session_load(Session, Original.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Entry = neverd_session_entry_addr(Session);
  const auto Disassembly = takeString(neverd_disasm_json(Session, Entry, 2));
  ASSERT_NE(Disassembly.find("eax"), std::string::npos);
  ASSERT_EQ(takeString(neverd_session_debug_info_kind(Session)), "map");
  const auto Missing = (Directory / "missing-mobile.map").string();
  neverd_session_set_map_path(Session, Missing.c_str());
  ASSERT_EQ(Environment.set("1"), 0);
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64);
    const auto Replacement = write("debug-replacement.macho",
                                   makeObservedMachO(AArch64, "_replacement"));
    ObservedMobileImage Observed;
    testing::internal::CaptureStderr();
    const int Status = neverd::sdk::loadNativeMobileSession(
        Session, Replacement.c_str(), ObservedMobileImage::observe, &Observed);
    const auto Trace = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Status, 0);
    expectSessionPhasePair(Trace, "failed");
    EXPECT_EQ(Observed.Calls, 1U);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("file not found"),
              std::string::npos);
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Original);
    EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "x86_64");
    EXPECT_EQ(takeString(neverd_session_debug_info_kind(Session)), "map");
    EXPECT_EQ(takeString(neverd_session_debug_info_path(Session)), Map);
    EXPECT_EQ(takeString(neverd_disasm_json(Session, Entry, 2)), Disassembly);
  }
  neverd_session_set_map_path(Session, nullptr);
  const auto Replacement =
      write("successful-mobile.macho", makeObservedMachO(true, "_replacement"));
  ObservedMobileImage Observed;
  ASSERT_EQ(neverd::sdk::loadNativeMobileSession(Session, Replacement.c_str(),
                                                 ObservedMobileImage::observe,
                                                 &Observed),
            1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(Observed.Calls, 1U);
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "aarch64");
  EXPECT_EQ(takeString(neverd_session_file_path(Session)), Replacement);
  EXPECT_EQ(takeString(neverd_session_debug_info_kind(Session)), "none");
  ASSERT_EQ(neverd_func_count(Session), 1);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "_replacement");
  EXPECT_NE(
      takeString(neverd_disasm_json(Session, 0x100000400ULL, 2)).find("w0"),
      std::string::npos);
}

TEST_F(SessionCAPITest, NativePhaseTraceRequiresExactEnvironmentValue) {
  ScopedNativePhaseEnvironment Environment;
  const char *Values[] = {nullptr, "",   "0",  "01", "11",
                          "true",  "1 ", " 1", "1\n"};
  for (const char *Value : Values) {
    SCOPED_TRACE(Value ? Value : "<unset>");
    ASSERT_EQ(Environment.set(Value), 0);
    const auto Load = capturePhaseLoad(Session, nullptr);
    EXPECT_EQ(Load.Status, 0);
    EXPECT_EQ(Load.Error, "input path is empty");
    EXPECT_TRUE(Load.Diagnostic.empty()) << Load.Diagnostic;
  }
}

TEST_F(SessionCAPITest, NativePhaseTracePreservesLoadFailuresAndExactPairing) {
  ScopedNativePhaseEnvironment Environment;
  const std::string Missing = (Directory / "absent-native.elf").string();
  const std::string Malformed = write("malformed.elf", "not an ELF image");
  const char *Paths[] = {nullptr, "", Missing.c_str(), Malformed.c_str()};
  for (const char *Path : Paths) {
    SCOPED_TRACE(Path ? Path : "<null>");
    ASSERT_EQ(Environment.set(nullptr), 0);
    const auto Before = capturePhaseLoad(Session, Path);
    ASSERT_EQ(Before.Status, 0);
    ASSERT_FALSE(Before.Error.empty());
    ASSERT_TRUE(Before.Diagnostic.empty()) << Before.Diagnostic;
    if (!Path || Path[0] == '\0')
      EXPECT_EQ(Before.Error, "input path is empty");
    else if (Missing == Path)
      EXPECT_EQ(Before.Error, "file not found: " + Missing);

    ASSERT_EQ(Environment.set("1"), 0);
    const auto Traced = capturePhaseLoad(Session, Path);
    EXPECT_EQ(Traced.Status, Before.Status);
    EXPECT_EQ(Traced.Error, Before.Error);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    expectSessionPhasePair(Traced.Diagnostic, "failed");
  }
}

TEST_F(SessionCAPITest, NativePhaseTraceDoesNotTraceNullSessions) {
  ScopedNativePhaseEnvironment Environment;
  ASSERT_EQ(Environment.set("1"), 0);
  const std::string Path = (Directory / "not-opened.elf").string();
  testing::internal::CaptureStderr();
  const int NullPath = neverd_session_load(nullptr, nullptr);
  const int NamedPath = neverd_session_load(nullptr, Path.c_str());
  const std::string Diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_EQ(NullPath, 0);
  EXPECT_EQ(NamedPath, 0);
  EXPECT_TRUE(Diagnostic.empty()) << Diagnostic;
}

TEST_F(SessionCAPITest,
       NativePhaseTracePreservesCompletedNativeLoadsAndReports) {
  ScopedNativePhaseEnvironment Environment;
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64);
    // Load-only native queries expose defined symbols, not pipeline discovery.
    const std::string Path =
        write(AArch64 ? "phase-arm64.elf" : "phase-x64.elf",
              makeNamedNativeELF("phase_entry", 0x400000, AArch64));
    ASSERT_EQ(Environment.set(nullptr), 0);
    const auto Before = capturePhaseLoad(Session, Path.c_str());
    ASSERT_EQ(Before.Status, 1) << Before.Error;
    ASSERT_TRUE(Before.Error.empty());
    ASSERT_TRUE(Before.Diagnostic.empty()) << Before.Diagnostic;
    const std::string Headers = takeString(neverd_headers_json(Session));
    ASSERT_FALSE(Headers.empty());
    const neverd_va_t Entry = neverd_session_entry_addr(Session);
    EXPECT_EQ(Entry, 0x400000u + sizeof(llvm::object::ELF64LE::Ehdr) +
                         sizeof(llvm::object::ELF64LE::Phdr));
    ASSERT_EQ(neverd_func_count(Session), 1);
    const neverd_va_t FunctionEntry = neverd_func_entry(Session, 0);
    const int FunctionSize = neverd_func_size(Session, 0);
    const std::string FunctionName = takeString(neverd_func_name(Session, 0));
    EXPECT_EQ(FunctionEntry, Entry);
    EXPECT_EQ(FunctionSize, AArch64 ? 8 : 6);
    EXPECT_EQ(FunctionName, "phase_entry");

    ASSERT_EQ(Environment.set("1"), 0);
    const auto Traced = capturePhaseLoad(Session, Path.c_str());
    EXPECT_EQ(Traced.Status, Before.Status);
    EXPECT_EQ(Traced.Error, Before.Error);
    expectSessionPhasePair(Traced.Diagnostic, "completed");
    EXPECT_EQ(neverd_session_is_loaded(Session), 1);
    EXPECT_EQ(neverd_session_entry_addr(Session), Entry);
    EXPECT_EQ(takeString(neverd_session_file_path(Session)), Path);
    ASSERT_EQ(neverd_func_count(Session), 1);
    EXPECT_EQ(neverd_func_entry(Session, 0), FunctionEntry);
    EXPECT_EQ(neverd_func_size(Session, 0), FunctionSize);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), FunctionName);
    EXPECT_EQ(takeString(neverd_headers_json(Session)), Headers);
  }
}

#ifndef _WIN32
TEST_F(SessionCAPITest,
       NativePhaseTraceClosedStderrPreservesActualLoadContract) {
  const std::string Input = write("phase-closed.elf", makeNativeELF(false));
  const std::string Missing = (Directory / "absent-closed.elf").string();
  for (bool Enabled : {false, true}) {
    SCOPED_TRACE(Enabled);
    ASSERT_EXIT(
        {
          const int Status =
              exerciseFailedPhaseSink(Session, Input, Missing, false, Enabled);
          std::exit(Status);
        },
        ::testing::ExitedWithCode(0), "^phase-sink-restored\n$");
  }
}
#endif

#ifdef __linux__
TEST_F(SessionCAPITest, NativePhaseTraceFullStderrPreservesActualLoadContract) {
  const std::string Input = write("phase-full.elf", makeNativeELF(false));
  const std::string Missing = (Directory / "absent-full.elf").string();
  for (bool Enabled : {false, true}) {
    SCOPED_TRACE(Enabled);
    ASSERT_EXIT(
        {
          const int Status =
              exerciseFailedPhaseSink(Session, Input, Missing, true, Enabled);
          std::exit(Status);
        },
        ::testing::ExitedWithCode(0), "^phase-sink-restored\n$");
  }
}
#endif

TEST_F(SessionCAPITest, DecompileDiffPreservesEqualSuccessfulOutputs) {
  expectDecompileDiff("6001600055", true);
}

TEST_F(SessionCAPITest, DecompileDiffPreservesDistinctSuccessfulOutputs) {
  expectDecompileDiff("6002600055", false);
}

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
  const std::array<TextQuery, 6> Queries{
      neverd_decompile, neverd_decompile_llvm, neverd_ir_low,
      neverd_ir_med,    neverd_ir_high,        neverd_ir_llvm};
  for (size_t Index = 0; Index < Queries.size(); ++Index) {
    SCOPED_TRACE(Index);
    EXPECT_TRUE(takeString(Queries[Index](Session, 0)).empty());
    EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);
    EXPECT_EQ(neverd_session_analyze(Session), 0);
    EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);
  }
  EXPECT_TRUE(takeString(neverd_decompile_llvm_ex(Session, 0, 1)).empty());
  EXPECT_EQ(takeString(neverd_last_error(Session)), Diagnostic);

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

TEST_F(SessionCAPITest, NativeTextDisassemblyUsesCurrentRecoveredFunctionView) {
  for (bool AArch64 : {false, true}) {
    for (bool Lazy : {false, true}) {
      SCOPED_TRACE(AArch64 ? "aarch64" : "x86_64");
      SCOPED_TRACE(Lazy ? "lazy analysis" : "explicit analysis");
      const std::string Tail =
          AArch64 ? std::string("\x1f\x20\x03\xd5\x1f\x20\x03\xd5", 8)
                  : std::string("\x90\x90", 2);
      const std::string Path =
          write((std::string("text-recovered-") + (AArch64 ? "arm-" : "x86-") +
                 (Lazy ? "lazy.elf" : "explicit.elf")),
                makeNativeELF(AArch64, 0x400000, Tail));
      ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
      const neverd_va_t Entry = neverd_session_entry_addr(Session);
      if (Lazy)
        ASSERT_FALSE(takeString(neverd_ir_low(Session, Entry)).empty());
      else
        ASSERT_EQ(neverd_session_analyze(Session), 1);
      const std::string Address = "0x" + llvm::utohexstr(Entry);
      // Text must synchronize recovery before any function-list query does.
      const std::string ByAddress =
          takeString(neverd_disasm_text(Session, Address.c_str(), 0));
      ASSERT_FALSE(ByAddress.empty());
      EXPECT_EQ(std::count(ByAddress.begin(), ByAddress.end(), '\n'), 3);
      const int Index = neverd_func_find_by_addr(Session, Entry);
      ASSERT_GE(Index, 0);
      const std::string Name = takeString(neverd_func_name(Session, Index));
      const std::string Header = "; " + Name + " (" + Address + ", " +
                                 std::to_string(AArch64 ? 8 : 6) + " bytes)\n";
      EXPECT_EQ(ByAddress.substr(0, Header.size()), Header);
      EXPECT_EQ(takeString(neverd_disasm_text(Session, Name.c_str(), 0)),
                ByAddress);
      ASSERT_EQ(neverd_rename_func(Session, Name.c_str(), "reviewed_text"), 0);
      const std::string Renamed =
          takeString(neverd_disasm_text(Session, "reviewed_text", 0));
      ASSERT_FALSE(Renamed.empty());
      EXPECT_EQ(Renamed.find("; reviewed_text ("), 0u);
      EXPECT_EQ(std::count(Renamed.begin(), Renamed.end(), '\n'), 3);
      EXPECT_EQ(takeString(neverd_disasm_text(Session, Address.c_str(), 0)),
                Renamed);
    }
  }
}

TEST_F(SessionCAPITest, NativeTextDisassemblyPreservesLoaderNameFallback) {
  const std::string Path = write("text-named.elf", makeNamedNativeELF("entry"));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const std::string Original =
      takeString(neverd_disasm_text(Session, "entry", 0));
  ASSERT_FALSE(Original.empty());
  EXPECT_EQ(Original.find("; entry ("), 0u);
  ASSERT_EQ(neverd_rename_func(Session, "entry", "reviewed_entry"), 0);
  const std::string Renamed =
      takeString(neverd_disasm_text(Session, "reviewed_entry", 0));
  ASSERT_FALSE(Renamed.empty());
  EXPECT_EQ(Renamed.find("; reviewed_entry ("), 0u);
  EXPECT_EQ(takeString(neverd_disasm_text(Session, "entry", 0)), Original);
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

TEST_F(SessionCAPITest, SignatureJSONListsTheRoutinesOtherNames) {
  // One routine the library defines under three names: the match takes the
  // public one and lists the others.
  expectSignatureJSONName("puts", " :0000 __IO_puts_internal :0000 _IO_puts",
                          {"_IO_puts", "__IO_puts_internal"});
}

TEST_F(SessionCAPITest, SignaturesMatchARoutineOnlyACallReveals) {
  // A stripped program without sections lists no function at all; its entry
  // calls a routine no table names.  Signatures are tried where the
  // pipeline's detector would find functions, so the routine is still named.
  const std::string Body("\xe8\x01\x00\x00\x00\xc3"  // call helper; ret
                         "\xb8\x2a\x00\x00\x00\xc3", // helper: mov eax, 42
                         12);
  const auto Input =
      write("calls-helper.elf", makeNativeELF(false, 0x400000, {}, Body));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const neverd_va_t Entry = neverd_session_entry_addr(Session);
  ASSERT_EQ(neverd_func_count(Session), 0);
  const auto Pattern =
      write("helper.pat", "B82A000000C3 00 0000 0006 :0000 helper_routine\n");

  ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1)
      << takeString(neverd_last_error(Session));

  auto Parsed = llvm::json::parse(takeString(neverd_sig_matches_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Parsed)) << llvm::toString(Parsed.takeError());
  const auto *Matches = Parsed->getAsArray();
  ASSERT_NE(Matches, nullptr);
  ASSERT_EQ(Matches->size(), 1u);
  const auto *Match = (*Matches)[0].getAsObject();
  ASSERT_NE(Match, nullptr);
  EXPECT_EQ(Match->getString("name"), "helper_routine");
  uint64_t Address = 0;
  ASSERT_FALSE(Match->getString("addr").value_or("").drop_front(2).getAsInteger(
      16, Address));
  EXPECT_EQ(Address, Entry + 6);
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
