//===- SessionCAPITests.cpp - public session failure contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NativeMobileSession.h"
#include "SessionImpl.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/ProjectWriteLock.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
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
#include <optional>
#include <regex>
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

#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
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

  /// Build the C program \p Text as a position-independent executable named
  /// \p Name with the host compiler; empty if it does not build.
  std::string buildProgram(const std::string &Name, std::string_view Text) {
    const auto Source = write(Name + ".c", Text);
    const std::string Binary = (Directory / Name).string();
    std::string Message;
    const int Built =
        llvm::sys::ExecuteAndWait(NEVERD_RUNTIME_FIXTURE_COMPILER,
                                  {NEVERD_RUNTIME_FIXTURE_COMPILER, "-O1",
                                   "-fPIE", "-pie", Source, "-o", Binary},
                                  std::nullopt, {}, 60, 0, &Message);
    EXPECT_EQ(Built, 0) << Message;
    return Built == 0 ? Binary : std::string();
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

TEST_F(SessionCAPITest, DisassemblyStatesHowEachInstructionMovesTheStack) {
  // Each row: its bytes, the stack pointer move "sp" (null when unknown) and
  // the register it is relative to, when not the stack pointer.
  struct Row {
    std::string_view Bytes;
    std::optional<int64_t> Move;
    std::string_view Base;
  };
  const Row Rows[] = {
      {"\x55", -8, {}},                       // push rbp
      {"\x48\x89\xe5", 0, {}},                // mov rbp, rsp
      {"\x48\x83\xe4\xf0", std::nullopt, {}}, // and rsp, -16
      {"\x48\x8d\x65\xe8", -0x18, "rbp"},     // lea rsp, [rbp - 0x18]
      {"\x48\x89\xec", 0, "rbp"},             // mov rsp, rbp
      {"\xc9", 8, "rbp"},                     // leave
      {"\x5c", std::nullopt, {}},             // pop rsp
      {std::string_view("\xe8\x00\x00\x00\x00", 5), -8, {}}, // call $+5
      {"\x48\x8d\x64\x24\x18", 0x18, {}}, // lea rsp, [rsp + 0x18]
      {"\xff\x14\x24", 0, {}},            // call qword ptr [rsp]
      {"\x9c", -8, {}},                   // pushfq
      {"\x9d", 8, {}},                    // popfq
      {"\x48\x01\xc4", std::nullopt, {}}, // add rsp, rax
      // ret 0x10: its pops belong to the transfer out of the function.
      {std::string_view("\xc2\x10\x00", 3), 0, {}},
  };
  std::string Code;
  for (const auto &R : Rows)
    Code += R.Bytes;
  const auto Input =
      write("stack-moves.elf", makeNativeELF(false, 0x400000, {}, Code));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Text = takeString(
      neverd_disasm_json_ex(Session, neverd_session_entry_addr(Session),
                            std::size(Rows), NEVERD_DISASM_STACK));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << Text;
  const auto *Array = Parsed->getAsArray();
  ASSERT_TRUE(Array && Array->size() == std::size(Rows)) << Text;
  for (size_t I = 0; I < std::size(Rows); ++I) {
    const auto *Object = (*Array)[I].getAsObject();
    ASSERT_TRUE(Object && Object->get("sp")) << I << ": " << Text;
    EXPECT_EQ(Object->getInteger("sp"), Rows[I].Move) << I << ": " << Text;
    EXPECT_EQ(Object->getString("sp_base").value_or("").str(), Rows[I].Base)
        << I << ": " << Text;
    // Flow fields are not requested.
    EXPECT_FALSE(Object->get("flow")) << I << ": " << Text;
  }
}

TEST_F(SessionCAPITest, DisassemblyStatesTheMemoryEachInstructionReaches) {
  // Each row: its bytes and the reference it states, as an offset from the
  // next instruction or an absolute address, with its kind and, for a read
  // or write, the bytes it accesses.
  struct Row {
    std::string_view Bytes;
    std::optional<int64_t> Relative;
    std::optional<uint64_t> Absolute;
    std::string_view Kind;
    int64_t Size = 0;
  };
  using namespace std::string_view_literals;
  const Row Rows[] = {
      // mov rdi, qword ptr [rip + 0x100]
      {"\x48\x8b\x3d\x00\x01\x00\x00"sv, 0x100, {}, "read", 8},
      // mov dword ptr [rip + 0x200], eax
      {"\x89\x05\x00\x02\x00\x00"sv, 0x200, {}, "write", 4},
      // mov rax, qword ptr fs:[0x28]: an offset in a segment, not an address
      {"\x64\x48\x8b\x04\x25\x28\x00\x00\x00"sv, {}, {}, {}},
      // mov eax, dword ptr [0x404000]
      {"\x8b\x04\x25\x00\x40\x40\x00"sv, {}, 0x404000, "read", 4},
      // lea rax, [rip + 0x10]
      {"\x48\x8d\x05\x10\x00\x00\x00"sv, 0x10, {}, "offset"},
      // mov rax, qword ptr [rbx + 8]
      {"\x48\x8b\x43\x08"sv, {}, {}, {}},
      {"\xc3"sv, {}, {}, {}},
  };
  std::string Code;
  for (const auto &R : Rows)
    Code += R.Bytes;
  const auto Input =
      write("memory-refs.elf", makeNativeELF(false, 0x400000, {}, Code));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const uint64_t Entry = neverd_session_entry_addr(Session);
  const auto Text = takeString(neverd_disasm_json_ex(
      Session, Entry, std::size(Rows), NEVERD_DISASM_FLOW));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << Text;
  const auto *Array = Parsed->getAsArray();
  ASSERT_TRUE(Array && Array->size() == std::size(Rows)) << Text;
  uint64_t Next = Entry;
  for (size_t I = 0; I < std::size(Rows); ++I) {
    Next += Rows[I].Bytes.size();
    const auto *Object = (*Array)[I].getAsObject();
    ASSERT_TRUE(Object) << I << ": " << Text;
    const auto *Refs = Object->getArray("refs");
    if (Rows[I].Kind.empty()) {
      EXPECT_TRUE(!Refs || Refs->empty()) << I << ": " << Text;
      continue;
    }
    ASSERT_TRUE(Refs && Refs->size() == 1) << I << ": " << Text;
    const auto *Ref = (*Refs)[0].getAsObject();
    ASSERT_TRUE(Ref) << I << ": " << Text;
    const uint64_t Expected =
        Rows[I].Absolute ? *Rows[I].Absolute : Next + *Rows[I].Relative;
    EXPECT_EQ(
        std::stoull(Ref->getString("to").value_or("0").str(), nullptr, 16),
        Expected)
        << I << ": " << Text;
    EXPECT_EQ(Ref->getString("kind").value_or("").str(), Rows[I].Kind)
        << I << ": " << Text;
    EXPECT_EQ(Ref->getInteger("size").value_or(0), Rows[I].Size)
        << I << ": " << Text;
  }
  // The function's direct references carry the same widths, as a fourth
  // element of a read or write.
  ASSERT_GE(neverd_session_discover_functions(Session), 1)
      << takeString(neverd_last_error(Session));
  const auto Page = takeString(neverd_code_refs_json(Session, Entry, 1));
  auto PageJson = llvm::json::parse(Page);
  ASSERT_TRUE(static_cast<bool>(PageJson)) << Page;
  const auto *PageRefs = PageJson->getAsObject()->getArray("refs");
  ASSERT_TRUE(PageRefs) << Page;
  size_t Accesses = 0;
  for (const auto &Value : *PageRefs) {
    const auto &Fields = *Value.getAsArray();
    const auto Kind = Fields[2].getAsString().value_or("");
    if (Kind != "read" && Kind != "write") {
      EXPECT_EQ(Fields.size(), 3u) << Page;
      continue;
    }
    ASSERT_EQ(Fields.size(), 4u) << Page;
    const uint64_t From =
        std::stoull(Fields[0].getAsString().value_or("0").str(), nullptr, 16);
    uint64_t At = Entry;
    for (const Row &R : Rows) {
      if (At == From)
        EXPECT_EQ(Fields[3].getAsInteger().value_or(0), R.Size) << Page;
      At += R.Bytes.size();
    }
    ++Accesses;
  }
  EXPECT_EQ(Accesses, 3u) << Page;
}

// The code a data executable's entry runs, and where it and the data start.
constexpr uint64_t DataELFBase = 0x400000;
constexpr uint64_t DataELFEntry = DataELFBase + 0xb0;
constexpr uint64_t DataELFData = DataELFBase + 0x1000;

// An x86-64 executable whose code segment runs \p Code (by default a
// return) and whose separate read-only segment holds \p Data.
std::string makeDataELF(std::string_view Data,
                        std::string_view Code = std::string_view("\xc3", 1)) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  constexpr uint64_t Base = DataELFBase;
  const size_t CodeOffset = sizeof(ELF::Ehdr) + 2 * sizeof(ELF::Phdr);
  static_assert(DataELFEntry - DataELFBase ==
                sizeof(ELF::Ehdr) + 2 * sizeof(ELF::Phdr));
  const size_t DataOffset = 0x1000;
  std::string Bytes(DataOffset + Data.size(), '\0');
  ELF::Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Base + CodeOffset;
  Header.e_phoff = sizeof(ELF::Ehdr);
  Header.e_ehsize = sizeof(ELF::Ehdr);
  Header.e_phentsize = sizeof(ELF::Phdr);
  Header.e_phnum = 2;
  Header.e_shentsize = sizeof(ELF::Shdr);
  ELF::Phdr Segments[2]{};
  Segments[0].p_type = PT_LOAD;
  Segments[0].p_flags = PF_R | PF_X;
  Segments[0].p_vaddr = Base;
  Segments[0].p_paddr = Base;
  Segments[0].p_filesz = CodeOffset + Code.size();
  Segments[0].p_memsz = CodeOffset + Code.size();
  Segments[0].p_align = 0x1000;
  Segments[1].p_type = PT_LOAD;
  Segments[1].p_flags = PF_R;
  Segments[1].p_offset = DataOffset;
  Segments[1].p_vaddr = Base + DataOffset;
  Segments[1].p_paddr = Base + DataOffset;
  Segments[1].p_filesz = Data.size();
  Segments[1].p_memsz = Data.size();
  Segments[1].p_align = 0x1000;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), Segments, sizeof(Segments));
  Bytes.replace(CodeOffset, Code.size(), Code);
  Bytes.replace(DataOffset, Data.size(), Data);
  return Bytes;
}

TEST_F(SessionCAPITest, StringReferencesReadTextFromTheReferencedCharacter) {
  // "hello world" and UTF-8 "\u4e2d\u6587\u5b57\u7b26", each terminated.
  constexpr char Data[] = "hello world\0"
                          "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6";
  // lea rdi/rsi/rdx/rcx, [rip + d] to bytes 0, 6, 9 and 13, then ret.
  std::string Code;
  const std::pair<uint8_t, uint64_t> Leas[] = {
      {0x3d, 0}, {0x35, 6}, {0x15, 9}, {0x0d, 13}};
  for (const auto &[ModRM, Offset] : Leas) {
    const uint64_t Next = DataELFEntry + Code.size() + 7;
    const auto Displacement =
        static_cast<uint32_t>(DataELFData + Offset - Next);
    Code += "\x48\x8d";
    Code += static_cast<char>(ModRM);
    for (unsigned I = 0; I < 4; ++I)
      Code += static_cast<char>(Displacement >> (8 * I));
  }
  Code += '\xc3';
  const auto Input =
      write("string-refs.elf",
            makeDataELF(std::string_view(Data, sizeof(Data)), Code));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  // The image has no symbols; the detector lists the entry's function.
  ASSERT_GE(neverd_session_discover_functions(Session), 1)
      << takeString(neverd_last_error(Session));
  // Rows as (from, to, string, text offset).
  const auto rows = [&](const char *Options) {
    std::vector<std::tuple<uint64_t, uint64_t, uint64_t, int64_t>> Rows;
    const auto Text =
        takeString(neverd_string_refs_json(Session, Options, 0, 16));
    auto Parsed = llvm::json::parse(Text);
    EXPECT_TRUE(static_cast<bool>(Parsed)) << Text;
    if (!Parsed)
      return Rows;
    const auto *Refs = Parsed->getAsObject()->getArray("refs");
    for (const auto &Ref : *Refs) {
      const auto &Row = *Ref.getAsArray();
      const auto address = [&](size_t I) {
        return std::stoull(Row[I].getAsString()->str(), nullptr, 16);
      };
      EXPECT_EQ(Row[4].getAsString()->str(), "offset") << Text;
      EXPECT_TRUE(Row[5].getAsNull()) << Text;
      Rows.emplace_back(address(0), address(1), address(2),
                        *Row[3].getAsInteger());
    }
    return Rows;
  };
  // "world" is long enough on its own, and the reference into "\u4e2d" reads
  // from the next character, three UTF-8 bytes into the string's text:
  // "\u6587\u5b57\u7b26" fills six columns.  "ld" fills two, under the
  // default four.
  using Row = std::tuple<uint64_t, uint64_t, uint64_t, int64_t>;
  const std::vector<Row> Default{
      {DataELFEntry, DataELFData, DataELFData, 0},
      {DataELFEntry + 7, DataELFData + 6, DataELFData, 6},
      {DataELFEntry + 21, DataELFData + 13, DataELFData + 12, 3}};
  EXPECT_EQ(rows(nullptr), Default);
  // Seven columns leave "world" (five) and the ideographs (six) out.
  EXPECT_EQ(rows(R"({"min_length":7})"), (std::vector<Row>{Default[0]}));
  EXPECT_EQ(neverd_string_refs_json(Session, R"({"preferred":"utf-8"})", 0, 16),
            nullptr);
}

// A 32-bit PE image based at 0x400000 running \p Code at 0x401000, with
// \p Data at 0x402000 and, when \p Relocated lists any, base relocations
// (HIGHLOW) for the code bytes at those offsets.
std::string makePE32(std::string_view Code, std::string_view Data,
                     llvm::ArrayRef<uint16_t> Relocated) {
  constexpr uint32_t ImageBase = 0x400000, Headers = 0x200, Raw = 0x200;
  std::string Reloc;
  const auto put16 = [](std::string &Out, size_t At, uint16_t Value) {
    if (Out.size() < At + 2)
      Out.resize(At + 2);
    Out[At] = static_cast<char>(Value);
    Out[At + 1] = static_cast<char>(Value >> 8);
  };
  const auto put32 = [&](std::string &Out, size_t At, uint32_t Value) {
    put16(Out, At, static_cast<uint16_t>(Value));
    put16(Out, At + 2, static_cast<uint16_t>(Value >> 16));
  };
  if (!Relocated.empty()) {
    // One block for the code page, padded to four bytes with an absolute
    // entry.
    const size_t Entries = (Relocated.size() + 1) / 2 * 2;
    put32(Reloc, 0, 0x1000);
    put32(Reloc, 4, static_cast<uint32_t>(8 + 2 * Entries));
    for (size_t I = 0; I < Entries; ++I)
      put16(Reloc, 8 + 2 * I,
            I < Relocated.size() ? static_cast<uint16_t>(0x3000 | Relocated[I])
                                 : 0);
  }
  struct SectionSpec {
    const char *Name;
    uint32_t Rva;
    std::string_view Bytes;
    uint32_t Flags;
  };
  const SectionSpec Sections[] = {{".text", 0x1000, Code, 0x60000020},
                                  {".rdata", 0x2000, Data, 0x40000040},
                                  {".reloc", 0x3000, Reloc, 0x42000040}};
  const unsigned Count = Relocated.empty() ? 2 : 3;
  std::string Bytes(Headers + Count * Raw, '\0');
  Bytes[0] = 'M';
  Bytes[1] = 'Z';
  put32(Bytes, 0x3c, 0x40);
  Bytes.replace(0x40, 4, std::string("PE\0\0", 4));
  put16(Bytes, 0x44, 0x14c); // Machine: i386
  put16(Bytes, 0x46, static_cast<uint16_t>(Count));
  put16(Bytes, 0x54, 0xe0);   // SizeOfOptionalHeader
  put16(Bytes, 0x56, 0x0102); // Executable, 32-bit
  constexpr size_t Optional = 0x58;
  put16(Bytes, Optional, 0x10b);       // PE32
  put32(Bytes, Optional + 16, 0x1000); // AddressOfEntryPoint
  put32(Bytes, Optional + 28, ImageBase);
  put32(Bytes, Optional + 32, 0x1000);               // SectionAlignment
  put32(Bytes, Optional + 36, 0x200);                // FileAlignment
  put16(Bytes, Optional + 40, 4);                    // OS version
  put16(Bytes, Optional + 48, 4);                    // Subsystem version
  put32(Bytes, Optional + 56, 0x1000 * (Count + 1)); // SizeOfImage
  put32(Bytes, Optional + 60, Headers);
  put16(Bytes, Optional + 68, 3);  // Console
  put32(Bytes, Optional + 92, 16); // NumberOfRvaAndSizes
  if (!Relocated.empty()) {
    put32(Bytes, Optional + 96 + 5 * 8, 0x3000);
    put32(Bytes, Optional + 96 + 5 * 8 + 4,
          static_cast<uint32_t>(Reloc.size()));
  }
  for (unsigned I = 0; I < Count; ++I) {
    const size_t Header = Optional + 0xe0 + 40 * I;
    Bytes.replace(Header, std::strlen(Sections[I].Name), Sections[I].Name);
    put32(Bytes, Header + 8, static_cast<uint32_t>(Sections[I].Bytes.size()));
    put32(Bytes, Header + 12, Sections[I].Rva);
    put32(Bytes, Header + 16, Raw);
    put32(Bytes, Header + 20, Headers + I * Raw);
    put32(Bytes, Header + 36, Sections[I].Flags);
    Bytes.replace(Headers + I * Raw, Sections[I].Bytes.size(),
                  Sections[I].Bytes);
  }
  return Bytes;
}

TEST_F(SessionCAPITest, RelocatedImmediatesAreOffsetsAndNumbersAreNot) {
  // push 0x402000; mov dword ptr [esp], 0x402000; mov eax, 0x402006; ret
  using namespace std::string_view_literals;
  constexpr auto Code = "\x68\x00\x20\x40\x00"
                        "\xc7\x04\x24\x00\x20\x40\x00"
                        "\xb8\x06\x20\x40\x00"
                        "\xc3"sv;
  constexpr auto Data = "hello\0world\0"sv;
  const auto offsets = [&](bool Relocated) {
    const uint16_t Fields[] = {0x001, 0x008, 0x00d};
    const auto Input =
        write(Relocated ? "relocated.exe" : "fixed.exe",
              makePE32(Code, Data,
                       Relocated ? llvm::ArrayRef<uint16_t>(Fields)
                                 : llvm::ArrayRef<uint16_t>()));
    EXPECT_EQ(neverd_session_load(Session, Input.c_str()), 1)
        << takeString(neverd_last_error(Session));
    std::vector<uint64_t> Targets;
    const auto Text = takeString(
        neverd_disasm_json_ex(Session, 0x401000, 4, NEVERD_DISASM_FLOW));
    auto Parsed = llvm::json::parse(Text);
    EXPECT_TRUE(static_cast<bool>(Parsed)) << Text;
    if (!Parsed)
      return Targets;
    for (const auto &Row : *Parsed->getAsArray())
      if (const auto *Refs = Row.getAsObject()->getArray("refs"))
        for (const auto &Ref : *Refs)
          if (Ref.getAsObject()->getString("kind") == "offset")
            Targets.push_back(std::stoull(
                Ref.getAsObject()->getString("to")->str(), nullptr, 16));
    return Targets;
  };
  // The loader relocates each immediate, so each is an address.
  EXPECT_EQ(offsets(true),
            (std::vector<uint64_t>{0x402000, 0x402000, 0x402006}));
  ASSERT_GE(neverd_session_discover_functions(Session), 1);
  const auto Refs =
      takeString(neverd_string_refs_json(Session, nullptr, 0, 16));
  EXPECT_NE(
      Refs.find(
          R"(["0x401000","0x402000","0x402000",0,"offset",null,"push 0x402000"])"),
      std::string::npos)
      << Refs;
  EXPECT_NE(
      Refs.find(
          R"(["0x40100C","0x402006","0x402006",0,"offset",null,"mov eax, 0x402006"])"),
      std::string::npos)
      << Refs;
  // Without relocations the same immediates are numbers.
  EXPECT_TRUE(offsets(false).empty());
}

TEST_F(SessionCAPITest, HexDumpsCrossGapsAndReadTextInAnyEncoding) {
  // GBK "\u4e2d\u6587" then "ab", in the data segment after a gap.
  constexpr char Data[] = "\xd6\xd0\xce\xc4"
                          "ab";
  const auto Input =
      write("hex-dump.elf", makeDataELF(std::string_view(Data, 6)));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  // The bytes before the data segment are unmapped; the dump goes on past
  // them and leaves out the unmapped lines after the last mapped byte.
  const auto Dump =
      takeString(neverd_hex_dump_ex(Session, DataELFData - 16, 48, "gbk"));
  const auto Lines = llvm::StringRef(Dump).split('\n');
  EXPECT_TRUE(Lines.first.contains("?? ?? ?? ?? ?? ?? ?? ??")) << Dump;
  EXPECT_TRUE(
      Lines.second.contains("d6 d0 ce c4 61 62 ?? ??  ?? ?? ?? ?? ?? ?? ?? ??"))
      << Dump;
  // Each wide character draws two columns over its two bytes; the segment
  // ends after "ab".
  EXPECT_TRUE(Lines.second.contains("|\xe4\xb8\xad\xe6\x96\x87"
                                    "ab          |"))
      << Dump;
  EXPECT_EQ(llvm::StringRef(Dump).count('\n'), 2u) << Dump;
  // ASCII by default, and an unknown encoding fails.
  EXPECT_TRUE(
      llvm::StringRef(takeString(neverd_hex_dump(Session, DataELFData, 6)))
          .contains("|....ab|"));
  EXPECT_EQ(neverd_hex_dump_ex(Session, DataELFData, 6, "klingon"), nullptr);
  EXPECT_EQ(neverd_hex_dump(Session, DataELFData + 0x100000, 16), nullptr);
}

TEST_F(SessionCAPITest, StringScanFindsUTF8AndWideStringsByOption) {
  // "hello", UTF-8 "中文字符" and UTF-16LE "Wide text", each terminated.
  constexpr char Data[] = "hello\0"
                          "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6\0"
                          "\0"
                          "W\0i\0d\0e\0 \0t\0e\0x\0t\0\0\0";
  const auto Input = write(
      "strings.elf", makeDataELF(std::string_view(Data, sizeof(Data) - 1)));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Scan = [&](const char *Options) {
    const char *Raw = neverd_strings_ex_json(Session, Options);
    if (!Raw)
      return std::string("error: ") + takeString(neverd_last_error(Session));
    return takeString(Raw);
  };
  const auto Defaults = Scan(nullptr);
  auto Parsed = llvm::json::parse(Defaults);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << Defaults;
  const auto *Rows = Parsed->getAsArray();
  ASSERT_TRUE(Rows && Rows->size() == 3) << Defaults;
  const auto Field = [&](size_t Row, llvm::StringRef Key) {
    return (*Rows)[Row].getAsObject()->getString(Key).value_or("").str();
  };
  EXPECT_EQ(Field(0, "encoding"), "ascii");
  EXPECT_EQ(Field(0, "addr"), "0x401000");
  EXPECT_EQ(Field(1, "encoding"), "utf-8");
  EXPECT_EQ(Field(1, "value"),
            "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97\xe7\xac\xa6");
  EXPECT_EQ((*Rows)[1].getAsObject()->getInteger("chars"), 4);
  EXPECT_EQ(Field(2, "encoding"), "utf-16le");
  EXPECT_EQ(Field(2, "value"), "Wide text");
  EXPECT_EQ((*Rows)[2].getAsObject()->getInteger("length"), 18);

  // Options select encodings and the minimum length.
  EXPECT_EQ(Scan(R"({"encodings":["utf-16le"]})").find("hello"),
            std::string::npos);
  EXPECT_EQ(Scan(R"({"min_length":6})").find("hello"), std::string::npos);
  // Malformed options fail clearly.
  EXPECT_EQ(Scan(R"({"encodings":["ebcdic"]})"),
            "error: unknown string encoding: ebcdic");
  EXPECT_EQ(Scan(R"({"min_length":0})"),
            "error: min_length must be an integer from 1 to 1024");
  EXPECT_EQ(Scan(R"({"limit":3})"), "error: unknown string option: limit");
  EXPECT_EQ(Scan(R"({"preferred":"utf-8"})"),
            "error: preferred must name a legacy code page");
  EXPECT_EQ(Scan("[]"), "error: string options must be a JSON object");

  // Pages of one string read the same rows from a cursor, ending with none.
  std::string Paged = "[";
  for (std::optional<uint64_t> Cursor = 0; Cursor;) {
    const auto Page =
        takeString(neverd_strings_page_json(Session, nullptr, *Cursor, 1));
    auto PageJson = llvm::json::parse(Page);
    ASSERT_TRUE(static_cast<bool>(PageJson)) << Page;
    const auto *Object = PageJson->getAsObject();
    const auto *Strings = Object->getArray("strings");
    ASSERT_TRUE(Strings && Strings->size() == 1) << Page;
    if (Paged.size() > 1)
      Paged += ",";
    Paged += llvm::formatv("{0}", (*Strings)[0]).str();
    Cursor.reset();
    if (const auto Next = Object->getString("next_addr")) {
      uint64_t Value = 0;
      ASSERT_FALSE(Next->getAsInteger(0, Value)) << Page;
      Cursor = Value;
    }
  }
  EXPECT_EQ(Paged + "]", Defaults);
  EXPECT_EQ(takeString(neverd_strings_page_json(Session, nullptr,
                                                0x401000 + 0x1000, 8)),
            "{\"next_addr\":null,\"strings\":[]}");
  EXPECT_EQ(neverd_strings_page_json(Session, R"({"min_length":0})", 0, 8),
            nullptr);

  const auto Encodings = takeString(neverd_string_encodings_json());
  EXPECT_NE(Encodings.find(R"("name":"utf-16le","spelling":"UTF-16LE")"),
            std::string::npos)
      << Encodings;
}

TEST_F(SessionCAPITest, StringScanReadsTheCodePagesSearched) {
  // "中文字符串" in GBK after a NUL.
  constexpr char Data[] = "\0\xd6\xd0\xce\xc4\xd7\xd6\xb7\xfb\xb4\xae\0";
  const auto Input =
      write("gbk.elf", makeDataELF(std::string_view(Data, sizeof(Data) - 1)));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  constexpr char Found[] =
      "[{\"addr\":\"0x401001\",\"chars\":5,\"encoding\":\"gbk\","
      "\"length\":10,\"value\":\"\xe4\xb8\xad\xe6\x96\x87\xe5\xad"
      "\x97\xe7\xac\xa6\xe4\xb8\xb2\"}]";
  // The common code pages are searched by default, and an alias names one.
  EXPECT_EQ(takeString(neverd_strings_ex_json(Session, nullptr)), Found);
  EXPECT_EQ(takeString(neverd_strings_ex_json(
                Session, R"({"encodings":["ascii","cp936"]})")),
            Found);
  EXPECT_EQ(
      takeString(neverd_strings_ex_json(Session, R"({"encodings":["ascii"]})")),
      "[]");
}

TEST_F(SessionCAPITest, SwitchTablesComeFromWholeProgramAnalysis) {
  // switch (x) on 0..3 through offsets from the table, as GCC lays out
  // position-independent code: cmp edi, 3; ja default; mov edi, edi;
  // lea rax, [rip + table]; movsxd rdx, [rax + rdi*4]; add rdx, rax;
  // jmp rdx; then the four cases and the default, `mov eax, N; ret` each.
  constexpr uint64_t Jump = DataELFEntry + 21, Cases = DataELFEntry + 23;
  std::string Code("\x83\xff\x03\x77\x2a\x89\xff\x48\x8d\x05", 10);
  const auto appendLE32 = [](std::string &Out, uint64_t Value) {
    for (unsigned I = 0; I < 4; ++I)
      Out.push_back(static_cast<char>(Value >> (8 * I)));
  };
  appendLE32(Code, DataELFData - (DataELFEntry + 14));
  Code.append("\x48\x63\x14\xb8\x48\x01\xc2\xff\xe2", 9);
  for (unsigned Value : {10u, 11u, 12u, 13u, 0xffffffffu}) {
    Code.push_back('\xb8');
    appendLE32(Code, Value);
    Code.push_back('\xc3');
  }
  std::string Table;
  for (unsigned Case = 0; Case < 4; ++Case)
    appendLE32(Table, Cases + 6 * Case - DataELFData);
  const auto Input = write("switch.elf", makeDataELF(Table, Code));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  // Tables come from the whole program's arbitrated analysis only.
  EXPECT_EQ(neverd_switches_json(Session, 0, 16), nullptr);
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const auto Text = takeString(neverd_switches_json(Session, 0, 16));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(static_cast<bool>(Parsed)) << Text;
  const auto *Switches = Parsed->getAsObject()->getArray("switches");
  ASSERT_TRUE(Switches && Switches->size() == 1) << Text;
  const auto &Switch = *(*Switches)[0].getAsObject();
  const auto hex = [](uint64_t Value) { return "0x" + llvm::utohexstr(Value); };
  EXPECT_EQ(Switch.getString("jump"), hex(Jump)) << Text;
  EXPECT_EQ(Switch.getString("table"), hex(DataELFData)) << Text;
  EXPECT_EQ(Switch.getString("form"), "table_relative") << Text;
  EXPECT_EQ(Switch.getInteger("entry_size"), 4) << Text;
  const auto *Targets = Switch.getArray("targets");
  ASSERT_TRUE(Targets && Targets->size() == 4) << Text;
  for (unsigned Case = 0; Case < 4; ++Case) {
    const auto &Target = *(*Targets)[Case].getAsArray();
    EXPECT_EQ(Target[0].getAsString(), hex(Cases + 6 * Case)) << Text;
    EXPECT_EQ(Target[1].getAsInteger(), Case) << Text;
    EXPECT_EQ(Target[2].getAsInteger(), Case) << Text;
  }
}

TEST(SessionNames, DemanglesAsIdentitiesDo) {
  EXPECT_EQ(takeString(neverd_demangle("_ZN8QDomNodeC1Ev")),
            "QDomNode::QDomNode()");
  // An ELF PLT entry or a Mach-O symbol carries one underscore more.
  EXPECT_EQ(takeString(neverd_demangle("__ZNK14QMessageLogger7warningEPKcz")),
            "QMessageLogger::warning(char const*, ...) const");
  EXPECT_EQ(takeString(neverd_demangle("?f@@YAXXZ")), "void __cdecl f(void)");
  // Rust reads without the legacy hash, Swift as a declaration path.
  EXPECT_EQ(
      takeString(neverd_demangle("_ZN4core3fmt5write17h0123456789abcdefE")),
      "core::fmt::write");
  EXPECT_EQ(takeString(neverd_demangle("_$s4Demo3BoxC5countSivg")),
            "Demo.Box.count.getter");
  EXPECT_EQ(takeString(neverd_demangle("main")), "main");
  EXPECT_EQ(neverd_demangle(nullptr), nullptr);
}

TEST(SessionTextDecoding, DecodesBytesForDisplayInAnyEncoding) {
  const unsigned char GBK[] = {'a', 0xd6, 0xd0, 0xff, 0x80};
  EXPECT_EQ(takeString(neverd_decode_text_json(GBK, sizeof(GBK), "gbk")),
            "{\"cells\":[\"a\",\"\xe4\xb8\xad\",\"\",null,\"\xe2\x82\xac\"]}");
  const unsigned char Wide[] = {'O', 0, 'K', 0, 0x0a, 0};
  EXPECT_EQ(takeString(neverd_decode_text_json(Wide, sizeof(Wide), "utf-16le")),
            R"({"cells":["O","","K","",null,null]})");
  EXPECT_EQ(neverd_decode_text_json(GBK, sizeof(GBK), "ebcdic"), nullptr);
  EXPECT_EQ(neverd_decode_text_json(GBK, -1, "gbk"), nullptr);
}

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

TEST_F(SessionCAPITest, DashboardHashesTheInputFile) {
  const auto Input = write("hashed.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  auto Dashboard =
      llvm::json::parse(takeString(neverd_dashboard_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Dashboard))
      << llvm::toString(Dashboard.takeError());
  const auto *Hashes = Dashboard->getAsObject()->getObject("hashes");
  ASSERT_NE(Hashes, nullptr);
  // zlib's CRC-32 and MD5 of the ten input bytes.
  EXPECT_EQ(Hashes->getString("crc32"), "704fe0d2");
  EXPECT_EQ(Hashes->getString("md5"), "9ee02fc015f79641c0620e674c5be3c7");
}

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

TEST_F(SessionCAPITest, OperandFormatsPersistByInstructionAndOperand) {
  const auto Input = write("operands.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Entry = neverd_session_entry_addr(Session);
  const std::string EntryHex = "0x" + llvm::utohexstr(Entry);
  ASSERT_EQ(
      neverd_operand_format_set(Session, Entry, 1, R"({"base":"decimal"})"), 0)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_operand_format_set(Session, Entry, 0,
                                      R"({"base":"hex","negate":true})"),
            0);
  const std::string Rows =
      "[{\"addr\":\"" + EntryHex +
      "\",\"operands\":[{\"base\":\"hex\",\"invert\":false,\"negate\":true,"
      "\"operand\":0},{\"base\":\"decimal\",\"invert\":false,\"negate\":"
      "false,\"operand\":1}]}]";
  EXPECT_EQ(takeString(neverd_operand_formats_json(Session)), Rows);
  // A base the table does not name, an operand past the eighth and a flag
  // that is no boolean are refused.
  EXPECT_EQ(neverd_operand_format_set(Session, Entry, 1, R"({"base":"octal"})"),
            -1);
  EXPECT_EQ(neverd_operand_format_set(Session, Entry, 8, R"({"base":"hex"})"),
            -1);
  EXPECT_EQ(neverd_operand_format_set(Session, Entry, 1,
                                      R"({"base":"hex","negate":1})"),
            -1);
  EXPECT_EQ(takeString(neverd_operand_formats_json(Session)), Rows);
  // Saved, the formats come back with the input.
  ASSERT_EQ(neverd_operand_formats_save(Session), 0)
      << takeString(neverd_last_error(Session));
  {
    neverd_session_t Reopened = neverd_session_create();
    ASSERT_EQ(neverd_session_load(Reopened, Input.c_str()), 1);
    EXPECT_EQ(takeString(neverd_operand_formats_json(Reopened)), Rows);
    neverd_session_destroy(Reopened);
  }
  // The listing's own spelling, or none, forgets an operand's format.
  ASSERT_EQ(
      neverd_operand_format_set(Session, Entry, 0, R"({"base":"number"})"), 0);
  ASSERT_EQ(neverd_operand_format_set(Session, Entry, 1, nullptr), 0);
  EXPECT_EQ(takeString(neverd_operand_formats_json(Session)), "[]");
  // Formats belong to code; data takes none.
  const auto Data = write("operands-data.elf",
                          makeDataELF(std::string_view("\x01\x02\x03\x04", 4)));
  neverd_session_t DataSession = neverd_session_create();
  ASSERT_EQ(neverd_session_load(DataSession, Data.c_str()), 1);
  EXPECT_EQ(neverd_operand_format_set(DataSession, DataELFData, 0,
                                      R"({"base":"hex"})"),
            -1);
  EXPECT_NE(takeString(neverd_last_error(DataSession)).find("executable"),
            std::string::npos);
  EXPECT_EQ(neverd_operand_format_set(DataSession, DataELFEntry, 0,
                                      R"({"base":"hex"})"),
            0);
  neverd_session_destroy(DataSession);
}

TEST_F(SessionCAPITest, UserNamesNameDataInSymbolsAndC) {
  // lea rax, [rip + d] to the data, then ret: the code takes the address of
  // data no symbol names.
  std::string Code = "\x48\x8d\x05";
  const auto Displacement =
      static_cast<uint32_t>(DataELFData - (DataELFEntry + 7));
  for (unsigned I = 0; I < 4; ++I)
    Code += static_cast<char>(Displacement >> (8 * I));
  Code += '\xc3';
  const auto Input =
      write("user-names.elf",
            makeDataELF(std::string_view("\x01\x02\x03\x04", 4), Code));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const std::string Row = "{\"addr\":\"0x" + llvm::utohexstr(DataELFData) +
                          "\",\"name\":\"table_base\",\"size\":0}";
  ASSERT_EQ(neverd_rename_addr(Session, DataELFData, "table_base"), 0)
      << takeString(neverd_last_error(Session));
  // The symbols name it, and so does the C.
  EXPECT_NE(takeString(neverd_data_symbols_json(Session)).find(Row),
            std::string::npos);
  ASSERT_GE(neverd_session_discover_functions(Session), 1);
  EXPECT_NE(
      takeString(neverd_decompile(Session, DataELFEntry)).find("table_base"),
      std::string::npos);
  // The name is saved with the input and comes back with it.
  {
    neverd_session_t Reopened = neverd_session_create();
    ASSERT_EQ(neverd_session_load(Reopened, Input.c_str()), 1);
    EXPECT_NE(takeString(neverd_data_symbols_json(Reopened)).find(Row),
              std::string::npos);
    neverd_session_destroy(Reopened);
  }
  // A name has no spaces, and names an address in the image.
  EXPECT_EQ(neverd_rename_addr(Session, DataELFData, "table base"), -1);
  EXPECT_EQ(neverd_rename_addr(Session, 0x10, "nowhere"), -1);
  // Taking the name away leaves the data unnamed again.
  ASSERT_EQ(neverd_rename_addr(Session, DataELFData, nullptr), 0);
  EXPECT_EQ(takeString(neverd_data_symbols_json(Session)).find("table_base"),
            std::string::npos);
  EXPECT_EQ(
      takeString(neverd_decompile(Session, DataELFEntry)).find("table_base"),
      std::string::npos);
}

// A universal Mach-O file of \p Slices, each at a page boundary.
std::string makeFatMachO(const std::vector<std::string> &Slices) {
  using namespace llvm::MachO;
  constexpr uint32_t Alignment = 12;
  std::string Bytes(sizeof(fat_header) + Slices.size() * sizeof(fat_arch),
                    '\0');
  const auto put32 = [&](size_t Offset, uint32_t Value) {
    llvm::support::endian::write32be(Bytes.data() + Offset, Value);
  };
  put32(0, FAT_MAGIC);
  put32(4, static_cast<uint32_t>(Slices.size()));
  for (size_t I = 0; I < Slices.size(); ++I) {
    Bytes.resize(llvm::alignTo(Bytes.size(), uint64_t(1) << Alignment), '\0');
    const auto &Slice = Slices[I];
    const auto *Header = reinterpret_cast<const mach_header_64 *>(Slice.data());
    const size_t Entry = sizeof(fat_header) + I * sizeof(fat_arch);
    put32(Entry, Header->cputype);
    put32(Entry + 4, Header->cpusubtype);
    put32(Entry + 8, static_cast<uint32_t>(Bytes.size()));
    put32(Entry + 12, static_cast<uint32_t>(Slice.size()));
    put32(Entry + 16, Alignment);
    Bytes += Slice;
  }
  return Bytes;
}

TEST_F(SessionCAPITest, IdentifyListsWhatLoadingReads) {
  const auto rows = [](const std::string &Path) {
    auto Parsed =
        llvm::json::parse(takeString(neverd_identify_json(Path.c_str())));
    std::vector<llvm::json::Object> Result;
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      return Result;
    }
    if (const auto *Rows = Parsed->getAsObject()->getArray("rows"))
      for (const auto &Row : *Rows)
        Result.push_back(*Row.getAsObject());
    return Result;
  };
  const auto text = [](const llvm::json::Object &Row) {
    return Row.getString("text").value_or("").str();
  };
  const auto loadable = [](const llvm::json::Object &Row) {
    return Row.getBoolean("loadable").value_or(false);
  };
  // An ELF executable has its row, and any file is a binary file last.
  const std::string Native = makeNativeELF(false);
  const auto NativePath = write("identify.elf", Native);
  auto Rows = rows(NativePath);
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_EQ(text(Rows[0]), "ELF64 for x86-64 (Executable)");
  EXPECT_EQ(Rows[0].getString("loader").value_or(""), "elf");
  EXPECT_EQ(Rows[0].getString("processor").value_or(""), "x86_64");
  EXPECT_TRUE(loadable(Rows[0]));
  EXPECT_EQ(text(Rows[1]), "Binary file");
  EXPECT_EQ(Rows[1].getString("loader").value_or(""), "binary");
  EXPECT_TRUE(loadable(Rows[1]));
  ASSERT_EQ(neverd_session_load(Session, NativePath.c_str()), 1);
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "x86_64");
  // A processor NeverD lacks and a big-endian file are named and refused,
  // as loading refuses them.
  std::string Mips = Native;
  llvm::support::endian::write16le(Mips.data() + llvm::ELF::EI_NIDENT + 2,
                                   llvm::ELF::EM_MIPS);
  const auto MipsPath = write("identify-mips.elf", Mips);
  Rows = rows(MipsPath);
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_EQ(text(Rows[0]), "ELF64 for MIPS (Executable)");
  EXPECT_FALSE(loadable(Rows[0]));
  EXPECT_EQ(Rows[0].getString("reason").value_or(""),
            "NeverD has no MIPS processor");
  EXPECT_EQ(neverd_session_load(Session, MipsPath.c_str()), 0);
  std::string Big = Native;
  Big[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2MSB;
  Rows = rows(write("identify-big.elf", Big));
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_FALSE(loadable(Rows[0]));
  // A thin Mach-O file, and a universal one with a row per slice: only the
  // slice loading reads is loadable.
  Rows = rows(write("identify.macho", makeObservedMachO(true, "_start")));
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_EQ(text(Rows[0]), "Mach-O file (EXECUTE). ARM64");
  EXPECT_TRUE(loadable(Rows[0]));
  const auto FatPath =
      write("identify-fat", makeFatMachO({makeObservedMachO(false, "_start"),
                                          makeObservedMachO(true, "_start")}));
  Rows = rows(FatPath);
  ASSERT_EQ(Rows.size(), 3u);
  EXPECT_EQ(text(Rows[0]), "Fat Mach-O file, 1. X86_64");
  EXPECT_EQ(text(Rows[1]), "Fat Mach-O file, 2. ARM64");
  ASSERT_NE(loadable(Rows[0]), loadable(Rows[1]));
  const auto &Chosen = loadable(Rows[0]) ? Rows[0] : Rows[1];
  const auto &Other = loadable(Rows[0]) ? Rows[1] : Rows[0];
  EXPECT_NE(Other.getString("reason").value_or("").find("slice"),
            llvm::StringRef::npos);
  ASSERT_EQ(neverd_session_load(Session, FatPath.c_str()), 1);
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)),
            Chosen.getString("processor").value_or(""));
  // Bytecode text is EVM by its contents; other contents named as bytecode
  // are EVM for the name alone, which a dialog does not choose by default.
  Rows = rows(write("identify-contract.evm", "6001600055"));
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_EQ(text(Rows[0]), "EVM bytecode");
  EXPECT_TRUE(loadable(Rows[0]));
  EXPECT_FALSE(Rows[0].getBoolean("by_name").value_or(true));
  Rows = rows(write("identify-firmware.bin", std::string(64, '\x07')));
  ASSERT_EQ(Rows.size(), 2u);
  EXPECT_EQ(text(Rows[0]), "EVM bytecode");
  EXPECT_TRUE(Rows[0].getBoolean("by_name").value_or(false));
  // Data no loader reads is a binary file only.
  Rows = rows(write("identify.dat", std::string(64, '\x07')));
  ASSERT_EQ(Rows.size(), 1u);
  EXPECT_EQ(text(Rows[0]), "Binary file");
}

TEST_F(SessionCAPITest, BinaryFilesReadTheirProcessorFromTheirBytes) {
  // This program's own code, cut from its ELF file: bytes no header
  // describes, which still name the processor they were built for.
#if defined(__x86_64__) || defined(_M_X64)
  const std::string Host = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  const std::string Host = "aarch64";
#else
  const std::string Host;
#endif
  static int Anchor;
  const std::string Self = llvm::sys::fs::getMainExecutable(nullptr, &Anchor);
  std::ifstream Input(Self, std::ios::binary);
  const std::string Image((std::istreambuf_iterator<char>(Input)),
                          std::istreambuf_iterator<char>());
  using Ehdr = llvm::object::ELF64LE::Ehdr;
  using Shdr = llvm::object::ELF64LE::Shdr;
  if (Host.empty() || Image.size() < sizeof(Ehdr) ||
      Image.compare(0, 4, llvm::ELF::ElfMagic) != 0 ||
      Image[llvm::ELF::EI_CLASS] != llvm::ELF::ELFCLASS64 ||
      Image[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2LSB)
    GTEST_SKIP() << "the test reads code from a little-endian ELF64 host";
  const auto *Header = reinterpret_cast<const Ehdr *>(Image.data());
  const auto *Sections =
      reinterpret_cast<const Shdr *>(Image.data() + Header->e_shoff);
  const Shdr &Names = Sections[Header->e_shstrndx];
  std::string Code;
  for (unsigned I = 0; I < Header->e_shnum; ++I)
    if (llvm::StringRef(Image.data() + Names.sh_offset + Sections[I].sh_name) ==
        ".text")
      Code = Image.substr(Sections[I].sh_offset,
                          std::min<uint64_t>(Sections[I].sh_size, 1 << 18));
  ASSERT_GE(Code.size(), 1u << 16);
  const auto Path = write("dump.bin", Code);

  auto Reply =
      llvm::json::parse(takeString(neverd_identify_json(Path.c_str())));
  ASSERT_TRUE(static_cast<bool>(Reply));
  const auto *Rows = Reply->getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows && !Rows->empty());
  const auto &Binary = *Rows->back().getAsObject();
  EXPECT_EQ(Binary.getString("status"), "settled");
  EXPECT_EQ(Binary.getString("detected"), Host);
  EXPECT_EQ(Binary.getInteger("code_offset"), 0);

  // Opened as a binary file with no processor named, it reads as that one,
  // and the load says what decided it.
  ASSERT_EQ(neverd_session_set_load_options(Session, R"({"loader":"binary"})"),
            0);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), Host);
  auto Load =
      llvm::json::parse(takeString(neverd_session_load_options_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Load));
  EXPECT_EQ(Load->getAsObject()->getString("processor"), Host);
  EXPECT_EQ(Load->getAsObject()->getString("processor_source"), "detected");
  EXPECT_NE(Load->getAsObject()
                ->getString("processor_evidence")
                .value_or("")
                .find("of the code"),
            llvm::StringRef::npos);

  // Bytes that look like no code are refused, and say so.
  const auto Zeros = write("zeros.bin", std::string(1 << 16, '\0'));
  ASSERT_EQ(neverd_session_set_load_options(Session, R"({"loader":"binary"})"),
            0);
  EXPECT_EQ(neverd_session_load(Session, Zeros.c_str()), 0);
  EXPECT_NE(takeString(neverd_last_error(Session))
                .find("no part of the file looks like code"),
            std::string::npos);
}

TEST_F(SessionCAPITest, BinaryFilesLoadAsAskedAndDecompile) {
  // nop; nop; mov eax, 7; ret: x86-64 code no header describes, named as EVM
  // bytecode often is.
  const std::string Code("\x90\x90\xb8\x07\x00\x00\x00\xc3", 8);
  const auto Path = write("firmware.bin", Code);
  ASSERT_EQ(neverd_session_set_load_options(
                Session, R"({"loader":"binary","processor":"x86_64",)"
                         R"("base":"0x400000","entry":"0x400002"})"),
            0)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "x86_64");
  EXPECT_EQ(neverd_session_entry_addr(Session), 0x400002u);
  EXPECT_NE(takeString(neverd_disasm_json(Session, 0x400002, 1)).find("mov"),
            std::string::npos);
  // Three instructions show no platform: System V is assumed, and says so.
  auto Load =
      llvm::json::parse(takeString(neverd_session_load_options_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Load));
  EXPECT_EQ(Load->getAsObject()->getString("platform"), "sysv");
  EXPECT_EQ(Load->getAsObject()->getString("platform_source"), "detected");
  EXPECT_NE(Load->getAsObject()
                ->getString("platform_evidence")
                .value_or("")
                .find("assumed"),
            llvm::StringRef::npos);
  EXPECT_NE(takeString(neverd_decompile(Session, 0x400002)).find("7"),
            std::string::npos)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(neverd_session_analyze(Session), 1);
  // Kept with the input, the choice reads the file the same way unasked.
  // "auto" refuses it: only its .bin name ties it to EVM bytecode, which
  // the user reads it as by choosing that loader.
  ASSERT_EQ(neverd_load_options_save(Session), 0)
      << takeString(neverd_last_error(Session));
  {
    neverd_session_t Again = neverd_session_create();
    ASSERT_EQ(neverd_session_load(Again, Path.c_str()), 1);
    EXPECT_EQ(takeString(neverd_session_arch_name(Again)), "x86_64");
    EXPECT_NE(
        takeString(neverd_session_load_options_json(Again)).find("binary"),
        std::string::npos);
    ASSERT_EQ(neverd_session_set_load_options(Again, R"({"loader":"auto"})"),
              0);
    EXPECT_EQ(neverd_session_load(Again, Path.c_str()), 0);
    EXPECT_NE(takeString(neverd_last_error(Again)).find("choose its loader"),
              std::string::npos);
    ASSERT_EQ(neverd_session_set_load_options(Again, R"({"loader":"evm"})"), 0);
    ASSERT_EQ(neverd_session_load(Again, Path.c_str()), 1);
    EXPECT_EQ(takeString(neverd_session_arch_name(Again)), "evm");
    EXPECT_EQ(takeString(neverd_session_load_options_json(Again)),
              R"({"loader":"evm"})");
    neverd_session_destroy(Again);
  }
  // Options the loader cannot follow are refused.
  EXPECT_EQ(neverd_session_set_load_options(
                Session, R"({"loader":"binary","processor":"mips"})"),
            -1);
  ASSERT_EQ(
      neverd_session_set_load_options(
          Session, R"({"loader":"binary","processor":"x86","size":"0x100"})"),
      0);
  EXPECT_EQ(neverd_session_load(Session, Path.c_str()), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("end of the file"),
            std::string::npos);
  // A 32-bit processor addresses no byte past 4 GiB.
  ASSERT_EQ(neverd_session_set_load_options(
                Session, R"({"loader":"binary","processor":"thumb",)"
                         R"("base":"0xfffffffc"})"),
            0);
  EXPECT_EQ(neverd_session_load(Session, Path.c_str()), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("4 GiB"),
            std::string::npos);
}

/// Six calls of a two-argument function, and the function, as a System V or
/// a Windows x86-64 compiler emits them; \p Add is where the function starts.
std::string twoArgumentProgram(bool Windows, size_t &Add) {
  constexpr unsigned Calls = 6;
  constexpr size_t CallBytes = 15, FrameBytes = 4;
  const auto le32 = [](std::string &Code, uint32_t Value) {
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Code += static_cast<char>(Value >> (8 * Byte));
  };
  std::string Code;
  // Windows reserves the home slots of a callee's arguments.
  if (Windows)
    Code.append("\x48\x83\xec\x28", FrameBytes); // sub rsp, 0x28
  const size_t Returns =
      Code.size() + Calls * CallBytes + (Windows ? FrameBytes : 0) + 1;
  Add = (Returns + 15) & ~size_t(15);
  for (unsigned I = 0; I < Calls; ++I) {
    // mov second, 2*I+2; mov first, 2*I+1; call add -- edx and ecx under
    // Windows, esi and edi under System V.
    Code += static_cast<char>(Windows ? 0xba : 0xbe);
    le32(Code, 2 * I + 2);
    Code += static_cast<char>(Windows ? 0xb9 : 0xbf);
    le32(Code, 2 * I + 1);
    Code += '\xe8';
    le32(Code, static_cast<uint32_t>(Add - (Code.size() + 4)));
  }
  if (Windows)
    Code.append("\x48\x83\xc4\x28", FrameBytes); // add rsp, 0x28
  Code += '\xc3';
  Code.resize(Add, '\xcc');
  // lea eax, [first+second]; ret
  Code.append(Windows ? "\x8d\x04\x11" : "\x8d\x04\x37", 3);
  Code += '\xc3';
  return Code;
}

/// The parameter count of \p Name's definition in decompiled \p Source.
int parameterCount(const std::string &Source, const std::string &Name) {
  const size_t At = Source.find(Name + "(");
  if (At == std::string::npos)
    return -1;
  const size_t Open = At + Name.size() + 1, Close = Source.find(')', Open);
  const llvm::StringRef Inside =
      llvm::StringRef(Source).slice(Open, Close).trim();
  return Inside.empty() || Inside == "void"
             ? 0
             : static_cast<int>(Inside.count(',')) + 1;
}

TEST_F(SessionCAPITest, BinaryFilesDecompileUnderThePlatformTheirCodeShows) {
  for (const bool Windows : {false, true}) {
    size_t Add = 0;
    const auto Path = write(Windows ? "windows.bin" : "sysv.bin",
                            twoArgumentProgram(Windows, Add));
    neverd_session_t Program = neverd_session_create();
    ASSERT_EQ(neverd_session_set_load_options(
                  Program, R"({"loader":"binary","processor":"x86_64",)"
                           R"("base":"0x400000"})"),
              0);
    ASSERT_EQ(neverd_session_load(Program, Path.c_str()), 1)
        << takeString(neverd_last_error(Program));
    auto Load = llvm::json::parse(
        takeString(neverd_session_load_options_json(Program)));
    ASSERT_TRUE(static_cast<bool>(Load));
    EXPECT_EQ(Load->getAsObject()->getString("platform"),
              Windows ? "windows" : "sysv");
    EXPECT_EQ(Load->getAsObject()->getString("platform_source"), "detected");
    // Two arguments in rcx and rdx under Windows, not four with phantom rdi
    // and rsi, as a System V reading gives.
    const uint64_t AddVA = 0x400000 + Add;
    const std::string Source = takeString(neverd_decompile(Program, AddVA));
    EXPECT_EQ(parameterCount(Source, "sub_" + llvm::utohexstr(AddVA)), 2)
        << Source << takeString(neverd_last_error(Program));
    neverd_session_destroy(Program);
  }
  // A platform the user names is theirs, the code notwithstanding.
  size_t Add = 0;
  const auto Path = write("named.bin", twoArgumentProgram(true, Add));
  ASSERT_EQ(neverd_session_set_load_options(
                Session, R"({"loader":"binary","processor":"x86_64",)"
                         R"("base":"0x400000","platform":"sysv"})"),
            0);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  auto Load =
      llvm::json::parse(takeString(neverd_session_load_options_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Load));
  EXPECT_EQ(Load->getAsObject()->getString("platform"), "sysv");
  EXPECT_EQ(Load->getAsObject()->getString("platform_source"), "user");
  EXPECT_EQ(neverd_session_set_load_options(
                Session, R"({"loader":"binary","processor":"x86_64",)"
                         R"("platform":"beos"})"),
            -1);
}

TEST_F(SessionCAPITest, InputSha256IsTheHashOfTheLoadedFile) {
  const auto Expected = [](const std::string &Bytes) {
    return llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(Bytes)),
                       /*LowerCase=*/true);
  };
  EXPECT_EQ(takeString(neverd_session_input_sha256(Session)), "");
  // The loader's hash for a native image, and the dashboard reports the same.
  const std::string Native = makeNativeELF(false);
  const auto NativePath = write("hashed.elf", Native);
  ASSERT_EQ(neverd_session_load(Session, NativePath.c_str()), 1);
  EXPECT_EQ(takeString(neverd_session_input_sha256(Session)), Expected(Native));
  auto Dashboard =
      llvm::json::parse(takeString(neverd_dashboard_json(Session)));
  ASSERT_TRUE(static_cast<bool>(Dashboard));
  EXPECT_EQ(Dashboard->getAsObject()
                ->getObject("hashes")
                ->getString("sha256")
                .value_or(""),
            Expected(Native));
  // An input its loader does not hash is hashed from its file.
  const std::string Contract = "6001600055";
  const auto ContractPath = write("hashed.evm", Contract);
  ASSERT_EQ(neverd_session_load(Session, ContractPath.c_str()), 1);
  EXPECT_EQ(takeString(neverd_session_input_sha256(Session)),
            Expected(Contract));
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

TEST_F(SessionCAPITest, CSourcePagesRetainCanonicalReturnAnchors) {
  for (bool AArch64 : {false, true}) {
    const uint64_t Base = AArch64 ? 0xffff800000400000ULL : 0x400000;
    const auto Path = write("source-anchors.elf", makeNativeELF(AArch64, Base));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
    const auto Entry = neverd_session_entry_addr(Session);
    const auto Return = Entry + (AArch64 ? 4 : 5);
    for (const char *Stage : {"c", "llvmc", "source"}) {
      SCOPED_TRACE(Stage);
      const auto Original =
          takeString(std::strcmp(Stage, "llvmc") == 0
                         ? neverd_decompile_llvm(Session, Entry)
                         : neverd_decompile(Session, Entry));
      ASSERT_FALSE(Original.empty());
      std::string Text;
      size_t Offset = 0, Mapped = 0;
      for (;;) {
        auto Page =
            takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 2));
        ASSERT_EQ(Page.getString("mapping_status"), "instruction_anchors");
        EXPECT_EQ(Page.getInteger("byte_offset"), Text.size());
        const auto Chunk = Page.getString("text");
        ASSERT_TRUE(Chunk);
        Text += Chunk->str();
        const auto *Rows = Page.getArray("rows");
        ASSERT_NE(Rows, nullptr);
        for (const auto &Value : *Rows) {
          const auto *Row = Value.getAsObject();
          ASSERT_NE(Row, nullptr);
          const auto *Addresses = Row->getArray("addresses");
          ASSERT_NE(Addresses, nullptr);
          if (!Addresses->empty()) {
            ASSERT_EQ(Addresses->size(), 1u);
            const auto Address = Addresses->front().getAsString();
            ASSERT_TRUE(Address);
            EXPECT_EQ(std::stoull(Address->str(), nullptr, 16), Return);
            EXPECT_EQ(Row->getString("mapping_status"), "instruction_anchor");
            ++Mapped;
          }
        }
        if (Page.getBoolean("complete").value_or(false))
          break;
        ASSERT_TRUE(Page.getInteger("next_offset"));
        Offset = *Page.getInteger("next_offset");
        ASSERT_LT(Offset, 1000u);
      }
      EXPECT_EQ(Text, Original);
      EXPECT_GT(Mapped, 0u);
    }
  }
}

TEST_F(SessionCAPITest, DialectNavigationMapsOnlyTheReturnRowAcrossPages) {
  const std::pair<const char *, const char *> Cases[] = {
      {"cpp", "_ZN4Demo4workEi"},
      {"rust", "_RNvCs8vGhbR5OvgK_13rust_eh_probe4main"},
      {"go", "runtime.deferreturn"}};
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64 ? "aarch64" : "x64");
    const uint64_t Base = AArch64 ? 0xffff800000400000ULL : 0x400000;
    for (const auto &[Dialect, Symbol] : Cases) {
      SCOPED_TRACE(Dialect);
      const auto Path = write("dialect-anchors.elf",
                              makeNamedNativeELF(Symbol, Base, AArch64));
      ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
      const auto Entry = neverd_session_entry_addr(Session);
      const auto Return = Entry + (AArch64 ? 4 : 5);
      for (const char *Stage : {Dialect, "source"}) {
        SCOPED_TRACE(Stage);
        const auto Full =
            takeView(neverd_ir_view_json(Session, Entry, Stage, 0, 2048));
        ASSERT_EQ(Full.getString("dialect"), Dialect);
        ASSERT_EQ(Full.getString("mapping_status"), "instruction_anchors");
        ASSERT_TRUE(Full.getString("text"));
        const auto *ExpectedRows = Full.getArray("rows");
        ASSERT_NE(ExpectedRows, nullptr);
        std::string Text;
        llvm::json::Array Rows;
        size_t Offset = 0, Mapped = 0, Unmapped = 0;
        for (;;) {
          const auto Page =
              takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 1));
          ASSERT_TRUE(Page.getString("text"));
          EXPECT_EQ(Page.getInteger("byte_offset"), Text.size());
          const auto Line = *Page.getString("text");
          Text += Line.str();
          const auto *PageRows = Page.getArray("rows");
          ASSERT_NE(PageRows, nullptr);
          ASSERT_EQ(PageRows->size(), 1u);
          const auto *Row = PageRows->front().getAsObject();
          ASSERT_NE(Row, nullptr);
          const auto *Addresses = Row->getArray("addresses");
          ASSERT_NE(Addresses, nullptr);
          if (Line.trim().starts_with("return ")) {
            ASSERT_EQ(Addresses->size(), 1u) << Line.str();
            ASSERT_TRUE(Addresses->front().getAsString());
            EXPECT_EQ(std::stoull(Addresses->front().getAsString()->str(),
                                  nullptr, 16),
                      Return);
            EXPECT_EQ(Row->getString("mapping_status"), "instruction_anchor");
            ++Mapped;
          } else {
            EXPECT_TRUE(Addresses->empty()) << Line.str();
            EXPECT_EQ(Row->getString("mapping_status"), "unmapped");
            ++Unmapped;
          }
          Rows.push_back(PageRows->front());
          if (Page.getBoolean("complete").value_or(false))
            break;
          ASSERT_TRUE(Page.getInteger("next_offset"));
          Offset = *Page.getInteger("next_offset");
          ASSERT_LT(Offset, 1000u);
        }
        EXPECT_EQ(Mapped, 1u);
        EXPECT_GT(Unmapped, 0u);
        EXPECT_EQ(Text, *Full.getString("text"));
        EXPECT_EQ(Rows, *ExpectedRows);
      }
    }
  }
}

TEST_F(SessionCAPITest, SourceViewsReadInTheProgramsLanguages) {
  // Pages of two lines reassemble the whole text; the first page says what
  // the text reads in, and each page the source names on it.
  std::vector<llvm::json::Object> SourceNames;
  auto Assemble = [&](neverd_va_t Entry, const char *Stage,
                      llvm::json::Object &First) {
    std::string Text;
    size_t Offset = 0;
    SourceNames.clear();
    for (;;) {
      auto Page =
          takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 2));
      EXPECT_EQ(Page.getString("representation"), Stage);
      if (Offset == 0)
        First = Page;
      if (const auto *Names = Page.getArray("source_names"))
        for (const auto &Name : *Names)
          if (const auto *Object = Name.getAsObject())
            SourceNames.push_back(*Object);
      if (!Page.getString("text"))
        return Text;
      EXPECT_EQ(Page.getInteger("byte_offset"), Text.size());
      Text += Page.getString("text")->str();
      if (Page.getBoolean("complete").value_or(false))
        return Text;
      const auto Next = Page.getInteger("next_offset");
      if (!Next || *Next <= static_cast<int64_t>(Offset) || *Next > 10000)
        return Text;
      Offset = static_cast<size_t>(*Next);
    }
  };
  auto Languages = [&] {
    std::vector<std::string> Result;
    auto Headers = takeView(neverd_headers_json(Session));
    if (const auto *Language = Headers.getObject("language"))
      if (const auto *Pseudocode = Language->getArray("pseudocode"))
        for (const auto &Key : *Pseudocode)
          Result.push_back(Key.getAsString().value_or("").str());
    return Result;
  };

  // A C program's pseudocode is C; Rust and Go are not offered for it.
  const std::string CPath =
      write("mapped.elf", makeNativeELF(/*AArch64=*/false, 0x400000));
  ASSERT_EQ(neverd_session_load(Session, CPath.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(Languages(), std::vector<std::string>({"c"}));
  const auto CEntry = neverd_session_entry_addr(Session);
  llvm::json::Object CPage, SourcePage, Refused;
  const std::string C = Assemble(CEntry, "c", CPage);
  ASSERT_FALSE(C.empty()) << takeString(neverd_last_error(Session));
  EXPECT_EQ(CPage.getString("dialect"), "c");
  EXPECT_EQ(Assemble(CEntry, "source", SourcePage), C);
  EXPECT_EQ(SourcePage.getString("dialect"), "c");
  for (const char *Stage : {"rust", "go"}) {
    Assemble(CEntry, Stage, Refused);
    EXPECT_EQ(Refused.getString("mapping_status"),
              "unsupported_representation");
    ASSERT_TRUE(Refused.getString("reason"));
    EXPECT_NE(Refused.getString("reason")->find("reads in C"),
              llvm::StringRef::npos);
  }

  // C++ without a runtime import is still recognized by a complete symbol.
  const std::string CppPath =
      write("cpp.elf", makeNamedNativeELF("_ZN4Demo4workEi"));
  ASSERT_EQ(neverd_session_load(Session, CppPath.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(Languages(), std::vector<std::string>({"c", "cpp"}));
  const auto CppEntry = neverd_session_entry_addr(Session);
  llvm::json::Object CppPage;
  const std::string Cpp = Assemble(CppEntry, "source", CppPage);
  EXPECT_EQ(CppPage.getString("dialect"), "cpp");
  EXPECT_NE(Cpp.find("Demo::work("), std::string::npos) << Cpp;
  EXPECT_EQ(Assemble(CppEntry, "cpp", CppPage), Cpp);
  const std::string PlainC = Assemble(CppEntry, "c", CPage);
  EXPECT_EQ(CPage.getString("dialect"), "c");
  EXPECT_NE(PlainC.find("Demo_work("), std::string::npos) << PlainC;

  // A Rust program reads its Rust functions as Rust, named as Rust names
  // them, and offers C beside it but not Go.
  const std::string RustPath = write(
      "rust.elf", makeNamedNativeELF("_RNvCs8vGhbR5OvgK_13rust_eh_probe4main"));
  ASSERT_EQ(neverd_session_load(Session, RustPath.c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(Languages(), std::vector<std::string>({"c", "rust"}));
  const auto RustEntry = neverd_session_entry_addr(Session);
  llvm::json::Object RustPage, GoPage;
  const std::string Rust = Assemble(RustEntry, "source", RustPage);
  EXPECT_EQ(RustPage.getString("dialect"), "rust");
  EXPECT_NE(Rust.find("unsafe fn rust_eh_probe::main("), std::string::npos)
      << Rust;
  ASSERT_NE(RustPage.getArray("unread"), nullptr);
  ASSERT_NE(RustPage.getArray("source_names"), nullptr);
  bool Named = false;
  for (const auto &Name : SourceNames) {
    const auto Begin = Name.getInteger("begin_byte");
    const auto End = Name.getInteger("end_byte");
    ASSERT_TRUE(Begin && End && *Begin < *End &&
                static_cast<size_t>(*End) <= Rust.size());
    Named |=
        Name.getString("symbol") == "_RNvCs8vGhbR5OvgK_13rust_eh_probe4main" &&
        Rust.substr(*Begin, *End - *Begin) == "rust_eh_probe::main";
  }
  EXPECT_TRUE(Named);
  // The prelude ends where the spelled definition begins, at a line start.
  if (const auto *Prelude = RustPage.getObject("prelude")) {
    const auto End = Prelude->getInteger("end_byte");
    ASSERT_TRUE(End);
    ASSERT_GT(*End, 0);
    ASSERT_LT(static_cast<size_t>(*End), Rust.size());
    EXPECT_EQ(Rust[*End - 1], '\n');
  }
  EXPECT_EQ(Assemble(RustEntry, "rust", RustPage), Rust);
  Assemble(RustEntry, "go", GoPage);
  EXPECT_EQ(GoPage.getString("mapping_status"), "unsupported_representation");

  // Bytecode has no native source to spell.
  const std::string Contract = write("mapped.evm", "600160020100");
  ASSERT_EQ(neverd_session_load(Session, Contract.c_str()), 1);
  for (const char *Stage : {"source", "rust", "go"}) {
    auto Unsupported = takeView(neverd_ir_view_json(Session, 0, Stage, 0, 2));
    EXPECT_EQ(Unsupported.getString("mapping_status"),
              "unsupported_architecture");
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
  for (const char *Stage : {"high", "llvm"}) {
    auto Unsupported = takeView(neverd_ir_view_json(Session, 0, Stage, 0, 2));
    EXPECT_EQ(Unsupported.getString("mapping_status"),
              "unsupported_representation");
    EXPECT_TRUE(Unsupported.getArray("rows")->empty());
  }
  for (const char *Stage : {"c", "llvmc"}) {
    auto Unsupported = takeView(neverd_ir_view_json(Session, 0, Stage, 0, 2));
    EXPECT_EQ(Unsupported.getString("mapping_status"),
              "unsupported_architecture");
    EXPECT_TRUE(Unsupported.getArray("rows")->empty());
  }
}

TEST_F(SessionCAPITest, LibrarySourcePagesPreserveTextAndReloadEvidence) {
  const auto Path =
      (std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) / "accessors-inline.o")
          .string();
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  int Index = neverd_func_find_by_name(Session, "_nd_vector_u32_data_inline");
  if (Index < 0)
    Index = neverd_func_find_by_name(Session, "nd_vector_u32_data_inline");
  ASSERT_GE(Index, 0);
  const auto Entry = neverd_func_entry(Session, Index);
  auto Before = takeView(neverd_ir_view_json(Session, Entry, "c", 0, 2048));
  ASSERT_NE(Before.getArray("library_regions"), nullptr);
  EXPECT_TRUE(Before.getArray("library_regions")->empty());
  const auto Rule = (std::filesystem::path(NEVERD_LIBRARY_FEATURE_DIR) /
                     "libcxx-23git-arm64-macos-abi1-alternate-clang22.json")
                        .string();
  ASSERT_GE(neverd_apply_signature_file(Session, Rule.c_str()), 0)
      << takeString(neverd_last_error(Session));
  for (const char *Stage : {"c", "llvmc"}) {
    SCOPED_TRACE(Stage);
    const std::string Original = takeString(
        std::strcmp(Stage, "c") == 0 ? neverd_decompile(Session, Entry)
                                     : neverd_decompile_llvm(Session, Entry));
    ASSERT_FALSE(Original.empty()) << takeString(neverd_last_error(Session));
    std::string Assembled;
    size_t Offset = 0;
    std::string RegionID;
    for (;;) {
      auto Page =
          takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 2));
      EXPECT_TRUE(Page.getString("mapping_status") == "library_regions" ||
                  Page.getString("mapping_status") == "instruction_anchors");
      EXPECT_EQ(Page.getInteger("byte_offset"), Assembled.size());
      ASSERT_TRUE(Page.getString("text"));
      Assembled += Page.getString("text")->str();
      const auto *Regions = Page.getArray("library_regions");
      ASSERT_NE(Regions, nullptr);
      ASSERT_EQ(Regions->size(), 1u);
      const auto *Region = Regions->front().getAsObject();
      ASSERT_NE(Region, nullptr);
      EXPECT_EQ(Region->getString("rule_id"), "libcxx.vector-u32.data");
      EXPECT_EQ(Region->getString("scope"), "inline-expression");
      EXPECT_EQ(Region->getString("linkage_name"), "");
      EXPECT_EQ(Region->getBoolean("foldable"), true);
      ASSERT_TRUE(Region->getString("id"));
      if (RegionID.empty())
        RegionID = Region->getString("id")->str();
      EXPECT_EQ(Region->getString("id"), RegionID);
      ASSERT_NE(Region->getArray("occurrences"), nullptr);
      EXPECT_FALSE(Region->getArray("occurrences")->empty());
      ASSERT_NE(Region->getArray("spans"), nullptr);
      EXPECT_FALSE(Region->getArray("spans")->empty());
      if (Page.getBoolean("complete").value_or(false))
        break;
      ASSERT_TRUE(Page.getInteger("next_offset"));
      Offset = *Page.getInteger("next_offset");
      ASSERT_LT(Offset, 10000u);
    }
    EXPECT_EQ(Assembled, Original);
  }
  std::filesystem::create_directories(Directory / "features" / "rules");
  const auto BadRule = write("features/rules/broken.json", "{}");
  EXPECT_EQ(neverd_apply_signature_file(Session, BadRule.c_str()), -1);
  auto Preserved = takeView(neverd_ir_view_json(Session, Entry, "c", 0, 2048));
  ASSERT_NE(Preserved.getArray("library_regions"), nullptr);
  EXPECT_EQ(Preserved.getArray("library_regions")->size(), 1u);
  const auto EmptyRoot = Directory / "empty-signature-tree";
  std::filesystem::create_directories(EmptyRoot);
  ASSERT_GE(neverd_auto_apply_signatures(Session, EmptyRoot.string().c_str()),
            0);
  auto Withdrawn = takeView(neverd_ir_view_json(Session, Entry, "c", 0, 2048));
  ASSERT_NE(Withdrawn.getArray("library_regions"), nullptr);
  EXPECT_TRUE(Withdrawn.getArray("library_regions")->empty());
  EXPECT_EQ(Withdrawn.getString("text"), Before.getString("text"));
}

TEST_F(SessionCAPITest, SourcePagesReadTheFunctionsKeptEmission) {
  // A copy: renaming writes its sidecar beside the input.
  const auto Input = Directory / "kept-emission.o";
  std::filesystem::copy_file(std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) /
                                 "accessors-inline.o",
                             Input);
  ASSERT_EQ(neverd_session_load(Session, Input.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  int Index = neverd_func_find_by_name(Session, "_nd_vector_u32_data_inline");
  if (Index < 0)
    Index = neverd_func_find_by_name(Session, "nd_vector_u32_data_inline");
  ASSERT_GE(Index, 0);
  const auto Entry = neverd_func_entry(Session, Index);
  auto &State = *neverd::sdk::toSession(Session);
  using Route = neverd::sdk::Session::SourceRoute;
  const auto Decompile = [&](bool HighC) {
    return takeString(HighC ? neverd_decompile(Session, Entry)
                            : neverd_decompile_llvm(Session, Entry));
  };
  std::string HighCText;
  for (const bool HighC : {true, false}) {
    const char *Stage = HighC ? "c" : "llvmc";
    SCOPED_TRACE(Stage);
    const Route Kind = HighC ? Route::PlainC : Route::LLVMC;
    const std::string Original = Decompile(HighC);
    ASSERT_FALSE(Original.empty()) << takeString(neverd_last_error(Session));
    if (HighC)
      HighCText = Original;
    // A decompile keeps its text; the first page adds the map pages need.
    auto *Kept = State.findFunctionSource(Entry, Kind);
    ASSERT_NE(Kept, nullptr);
    EXPECT_EQ(Kept->Text, Original);
    EXPECT_FALSE(Kept->Map);
    std::string Assembled;
    for (size_t Offset = 0;;) {
      auto Page =
          takeView(neverd_ir_view_json(Session, Entry, Stage, Offset, 2));
      ASSERT_TRUE(Page.getString("text"));
      Assembled += Page.getString("text")->str();
      if (Page.getBoolean("complete").value_or(false))
        break;
      ASSERT_TRUE(Page.getInteger("next_offset"));
      Offset = *Page.getInteger("next_offset");
      ASSERT_LT(Offset, 10000u);
    }
    EXPECT_EQ(Assembled, Original);
    Kept = State.findFunctionSource(Entry, Kind);
    ASSERT_NE(Kept, nullptr);
    EXPECT_TRUE(Kept->Map);
    // Later pages and decompiles read the kept text instead of emitting the
    // function again.
    Kept->Text = "/* kept */\n";
    EXPECT_EQ(takeView(neverd_ir_view_json(Session, Entry, Stage, 0, 2))
                  .getString("text"),
              "/* kept */\n");
    EXPECT_EQ(Decompile(HighC), "/* kept */\n");
  }
  // The user's names are the emitters' input too (data and labels take them,
  // UserNamesNameDataInSymbolsAndC): naming anything emits the source again.
  const std::string Old = takeString(
      neverd_func_name(Session, neverd_func_find_by_addr(Session, Entry)));
  ASSERT_EQ(neverd_rename_func(Session, Old.c_str(), "kept_accessor"), 0)
      << takeString(neverd_last_error(Session));
  EXPECT_TRUE(State.FunctionSources.empty());
  EXPECT_EQ(Decompile(true).find("/* kept */"), std::string::npos);
  Decompile(false);
  ASSERT_EQ(neverd_rename_addr(Session, Entry, nullptr), 0)
      << takeString(neverd_last_error(Session));
  EXPECT_TRUE(State.FunctionSources.empty());
  // A new pipeline emits the source again.
  Decompile(true);
  neverd_session_restrict_function(Session, 0);
  EXPECT_TRUE(State.FunctionSources.empty());
  EXPECT_EQ(Decompile(true), HighCText);
}

TEST_F(SessionCAPITest, CViewIncludesOnlyTheRequestedFunctionsNativeHandlers) {
  const auto Path = write("handlers.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  auto &State = *neverd::sdk::toSession(Session);
  neverd::HighFunc Parent;
  Parent.Entry = Entry;
  Parent.Name = "parent";
  Parent.ReturnType = neverd::NdType::makeVoid();
  neverd::HighStmt Region;
  Region.Kind = neverd::StmtKind::CxxTry;
  Region.EHRange = {Entry, Entry + 6};
  neverd::HighEHClause Catch;
  Catch.Kind = neverd::HighEHClauseKind::CxxCatch;
  Catch.HandlerVA = Entry + 0x100;
  Region.EHClauses.push_back(Catch);
  Parent.Body.push_back(Region);
  Parent.StructuredExceptionRegions = 1;
  neverd::HighFunc Handler;
  Handler.Entry = Catch.HandlerVA;
  Handler.Name = "native_handler";
  Handler.ReturnType = neverd::NdType::makeVoid();
  neverd::HighStmt Return;
  Return.Kind = neverd::StmtKind::Return;
  Handler.Body.push_back(Return);
  auto Unrelated = Handler;
  Unrelated.Entry += 0x100;
  Unrelated.Name = "unrelated_function";
  State.PipeResult.HighFuncs = {Parent, Handler, Unrelated};
  const auto Text = takeString(neverd_decompile(Session, Entry));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(Text.find("native_handler(void)"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("unrelated_function"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("try {"), std::string::npos) << Text;
  EXPECT_EQ(takeView(neverd_ir_view_json(Session, Entry, "c", 0, 2048))
                .getString("text"),
            Text);
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

TEST_F(SessionCAPITest,
       LibraryIdentityIsSharedAndDoesNotReplaceUserOrLinkageNames) {
  const auto Input = Directory / "library-identity.o";
  std::filesystem::copy_file(std::filesystem::path(NEVERD_LIBRARY_FIXTURE_DIR) /
                                 "accessors-inline.o",
                             Input);
  ASSERT_EQ(neverd_session_load(Session, Input.string().c_str()), 1);
  const auto Pack = (std::filesystem::path(NEVERD_LIBRARY_FEATURE_DIR) /
                     "libcxx-23git-arm64-macos-abi1-alternate-clang22.json")
                        .string();
  ASSERT_GE(neverd_apply_signature_file(Session, Pack.c_str()), 0);
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const auto Graph = takeView(neverd_callgraph_json(Session));
  const auto *Nodes = Graph.getArray("nodes");
  ASSERT_NE(Nodes, nullptr);
  unsigned Checked = 0;
  std::set<std::string> Displays;
  for (const auto &Node : *Nodes) {
    const auto *N = Node.getAsObject();
    ASSERT_NE(N, nullptr);
    const auto *Identity = N->getObject("identity");
    ASSERT_NE(Identity, nullptr);
    const auto *Annotations = Identity->getArray("library_annotations");
    ASSERT_NE(Annotations, nullptr);
    bool Whole = false;
    for (const auto &A : *Annotations)
      Whole |= A.getAsObject()->getString("scope") == "whole-function";
    if (!Whole)
      continue;
    uint64_t Entry;
    ASSERT_TRUE(N->getString("addr"));
    ASSERT_FALSE(N->getString("addr")->getAsInteger(0, Entry));
    auto Resolved = takeView(neverd_resolve_addr(Session, Entry));
    const std::string Raw = Resolved.getString("name")->str();
    EXPECT_EQ(Resolved.getString("name_origin"), "stated");
    EXPECT_EQ(Resolved.getString("linkage_name"), Raw);
    EXPECT_EQ(Resolved.getString("display_name"), N->getString("display_name"));
    EXPECT_NE(Resolved.getString("display_name")->find("::"),
              llvm::StringRef::npos);
    Displays.insert(Resolved.getString("display_name")->str());
    for (const char *Route : {"c", "llvmc"}) {
      auto Page = takeView(neverd_ir_view_json(Session, Entry, Route, 0, 2048));
      const auto *SourceIdentity = Page.getObject("function_identity");
      ASSERT_NE(SourceIdentity, nullptr);
      EXPECT_EQ(SourceIdentity->getString("display_name"),
                Resolved.getString("display_name"));
      EXPECT_EQ(SourceIdentity->getString("linkage_name"), Raw);
    }
    const auto User = "chosen_" + std::to_string(Checked++);
    ASSERT_EQ(neverd_rename_func(Session, Raw.c_str(), User.c_str()), 0);
    auto Renamed = takeView(neverd_resolve_addr(Session, Entry));
    EXPECT_EQ(Renamed.getString("name"), User);
    EXPECT_EQ(Renamed.getString("display_name"), User);
    EXPECT_EQ(Renamed.getString("linkage_name"), Raw);
    EXPECT_EQ(Renamed.getString("name_origin"), "user");
    EXPECT_FALSE(Renamed.getArray("library_annotations")->empty());
  }
  EXPECT_EQ(Checked, 16u);
  EXPECT_EQ(Displays.size(), Checked);
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

TEST_F(SessionCAPITest, SourcePagesLocateTheFunctionAfterItsPrelude) {
  const auto Path = write("prelude.elf", makeNamedNativeELF("entry_fn"));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Entry = neverd_session_entry_addr(Session);
  for (const char *Stage : {"c", "llvmc"}) {
    SCOPED_TRACE(Stage);
    auto Page = takeView(neverd_ir_view_json(Session, Entry, Stage, 0, 2048));
    ASSERT_TRUE(Page.getString("text"))
        << takeString(neverd_last_error(Session));
    ASSERT_EQ(Page.getBoolean("complete"), true);
    const std::string Text = Page.getString("text")->str();
    EXPECT_EQ(Text, takeString(std::strcmp(Stage, "c") == 0
                                   ? neverd_decompile(Session, Entry)
                                   : neverd_decompile_llvm(Session, Entry)));
    const auto *Prelude = Page.getObject("prelude");
    ASSERT_NE(Prelude, nullptr) << Text;
    const auto Lines = Prelude->getInteger("lines");
    const auto End = Prelude->getInteger("end_byte");
    ASSERT_TRUE(Lines && End);
    ASSERT_GT(*End, 0);
    ASSERT_LT(static_cast<size_t>(*End), Text.size());
    // The prelude is whole lines: includes and declarations before the
    // definition, which holds the function's name.
    EXPECT_EQ(Text[*End - 1], '\n');
    EXPECT_EQ(std::count(Text.begin(), Text.begin() + *End, '\n'), *Lines);
    EXPECT_NE(Text.substr(0, *End).find("#include"), std::string::npos);
    EXPECT_NE(Text.substr(*End).find("entry_fn"), std::string::npos) << Text;
    if (std::strcmp(Stage, "c") == 0)
      EXPECT_EQ(Text.compare(*End, 18, "/* neverd.entry: 0"), 0) << Text;
  }
}

TEST_F(SessionCAPITest, EveryFunctionOfALazilyBoundProgramShowsAsLLVMC) {
#if !defined(__linux__)
  GTEST_SKIP() << "builds a glibc program";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // printf binds lazily: its PLT entry jumps through a GOT slot the dynamic
  // linker writes, and the first PLT entry through the slot it fills with its
  // resolver.  Every function, stubs and all, is C a compiler accepts: no
  // call goes through a data pointer.
  const std::string Binary =
      buildProgram("lazy", "#include <stdio.h>\n"
                           "int main(int argc, char **argv) {\n"
                           "  printf(\"%d %s\\n\", argc, argv[0]);\n"
                           "  return 0;\n"
                           "}\n");
  ASSERT_FALSE(Binary.empty());
  ASSERT_EQ(neverd_session_load(Session, Binary.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const int Count = neverd_func_count(Session);
  ASSERT_GT(Count, 0);
  const std::regex DataPointerCall(R"(\(\(void\s*\*\)[^()]*\)\s*\()");
  for (int I = 0; I < Count; ++I) {
    const neverd_va_t Entry = neverd_func_entry(Session, I);
    const std::string LLVMC = takeString(neverd_decompile_llvm(Session, Entry));
    EXPECT_FALSE(LLVMC.empty()) << "0x" << llvm::utohexstr(Entry) << ": "
                                << takeString(neverd_last_error(Session));
    EXPECT_FALSE(std::regex_search(LLVMC, DataPointerCall)) << LLVMC;
  }
#endif
}

TEST_F(SessionCAPITest, TLSDescriptorCallsShowAsCallsThroughTheDescriptor) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "builds an x86-64 ELF shared object";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // Under the GNU2 dialect, code reaches a thread-local variable by calling
  // the resolver the dynamic linker writes into its TLS descriptor.
  const auto Source = write("tls.c", "__thread int counter;\n"
                                     "int bump(int by) {\n"
                                     "  counter += by;\n"
                                     "  return counter;\n"
                                     "}\n");
  const std::string Library = (Directory / "libtls.so").string();
  std::string Message;
  if (llvm::sys::ExecuteAndWait(NEVERD_RUNTIME_FIXTURE_COMPILER,
                                {NEVERD_RUNTIME_FIXTURE_COMPILER, "-O1",
                                 "-fPIC", "-shared", "-mtls-dialect=gnu2",
                                 Source, "-o", Library},
                                std::nullopt, {}, 60, 0, &Message) != 0)
    GTEST_SKIP() << "the compiler has no GNU2 TLS dialect: " << Message;
  ASSERT_EQ(neverd_session_load(Session, Library.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const int Bump = neverd_func_find_by_name(Session, "bump");
  ASSERT_GE(Bump, 0);
  const std::string LLVMC = takeString(
      neverd_decompile_llvm(Session, neverd_func_entry(Session, Bump)));
  ASSERT_FALSE(LLVMC.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(LLVMC.find("(*)("), std::string::npos) << LLVMC;
#endif
}

TEST_F(SessionCAPITest, DebugParametersNameTheRegistersTheyArriveIn) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "builds an x86-64 System V program";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // a and b arrive in xmm0 and xmm1 and c, an x87 long double, on the
  // stack: the reads of xmm0 and xmm1 are a and b, as the debug
  // declaration names them, whatever numbering the lift gave them.
  const auto Source =
      write("mix.c", "__attribute__((noinline))\n"
                     "double mix(double a, float b, long double c) {\n"
                     "  return a * 1.5 + b / 3.0 + (double)(c + c);\n"
                     "}\n"
                     "int main(int argc, char **argv) {\n"
                     "  return (int)mix(argc, 2.0f, 3.0L);\n"
                     "}\n");
  const std::string Binary = (Directory / "mix").string();
  std::string Message;
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(NEVERD_RUNTIME_FIXTURE_COMPILER,
                                {NEVERD_RUNTIME_FIXTURE_COMPILER, "-O2", "-g",
                                 "-fPIE", "-pie", Source, "-o", Binary},
                                std::nullopt, {}, 60, 0, &Message),
      0)
      << Message;
  ASSERT_EQ(neverd_session_load(Session, Binary.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const int Mix = neverd_func_find_by_name(Session, "mix");
  ASSERT_GE(Mix, 0);
  const std::string HighC =
      takeString(neverd_decompile(Session, neverd_func_entry(Session, Mix)));
  ASSERT_FALSE(HighC.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(HighC.find("double mix(double a, float b"), std::string::npos)
      << HighC;
  EXPECT_EQ(HighC.find("arg6"), std::string::npos) << HighC;
  EXPECT_EQ(HighC.find("arg7"), std::string::npos) << HighC;
  // The x87 value converts where it is used: no helper function.
  EXPECT_EQ(HighC.find("neverd_x87_value"), std::string::npos) << HighC;
  EXPECT_EQ(HighC.find("neverd_mem_"), std::string::npos) << HighC;
#endif
}

TEST_F(SessionCAPITest, StubsOfVariadicImportsPassTheirArgumentsOn) {
#if !defined(__linux__)
  GTEST_SKIP() << "builds a glibc program";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // Each call reaches its import through a PLT stub, which passes every
  // argument on.  C passes no `...` on, so the stub shows as a function that
  // hands printf's variadic arguments to vprintf, reads open's mode, and
  // collects execl's pointers for execv, in both C backends.
  const std::string Binary =
      buildProgram("variadic", "#include <fcntl.h>\n"
                               "#include <stdio.h>\n"
                               "#include <unistd.h>\n"
                               "int main(int argc, char **argv) {\n"
                               "  printf(\"%d %s\\n\", argc, argv[0]);\n"
                               "  int fd = open(argv[0], O_RDONLY);\n"
                               "  if (argc > 100)\n"
                               "    execl(argv[0], argv[0], (char *)0);\n"
                               "  return fd < 0;\n"
                               "}\n");
  ASSERT_FALSE(Binary.empty());
  ASSERT_EQ(neverd_session_load(Session, Binary.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const std::map<std::string, std::string> Forms = {
      {"printf", "vprintf(format, arguments)"},
      {"open", "va_arg(arguments, int)"},
      {"execl", "execv(arg0, (char *const *)vector)"}};
  std::set<std::string> Shown;
  for (int I = 0; I < neverd_func_count(Session); ++I) {
    const neverd_va_t Entry = neverd_func_entry(Session, I);
    for (const bool LLVM : {false, true}) {
      const std::string Page =
          takeString(LLVM ? neverd_decompile_llvm(Session, Entry)
                          : neverd_decompile(Session, Entry));
      for (const auto &[Import, Form] : Forms) {
        if (Page.find("jumps to the import " + Import + ",") ==
            std::string::npos)
          continue;
        Shown.insert(Import + (LLVM ? " in LLVM-C" : " in HighC"));
        EXPECT_NE(Page.find(Form), std::string::npos) << Page;
        const auto Source = write("stub.c", Page);
        std::string Message;
        EXPECT_EQ(llvm::sys::ExecuteAndWait(
                      NEVERD_RUNTIME_FIXTURE_COMPILER,
                      {NEVERD_RUNTIME_FIXTURE_COMPILER, "-std=gnu11", "-w",
                       "-fno-strict-aliasing", "-c", Source, "-o",
                       (Directory / "stub.o").string()},
                      std::nullopt, {}, 60, 0, &Message),
                  0)
            << Message << Page;
      }
    }
  }
  EXPECT_EQ(Shown.size(), 2 * Forms.size()) << llvm::join(Shown, ", ");
#endif
}

TEST_F(SessionCAPITest, BothCBackendsAgreeWhichFunctionsReturnAValue) {
#if !defined(__linux__)
  GTEST_SKIP() << "builds a glibc program";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // Whether a function returns a value is settled once, on MedIR.  LLVM-C
  // once showed a function that hands back its callee's result as void while
  // its callers used the result, and register_tm_clones, which returns 0,
  // too; HighC showed register_tm_clones' branch on a link-time 0.
  const std::string Binary = buildProgram(
      "returns",
      "int g;\n"
      "__attribute__((noinline)) void sink(int x) { g = x; }\n"
      "__attribute__((noinline)) int value(int x) { return x * 3; }\n"
      "__attribute__((noinline)) void tail_void(int x) {\n"
      "  sink(x + 1);\n"
      "}\n"
      "__attribute__((noinline)) int tail_value(int x) {\n"
      "  return value(x + 1);\n"
      "}\n"
      "int main(int argc, char **argv) {\n"
      "  (void)argv;\n"
      "  tail_void(argc);\n"
      "  return tail_value(argc) + g;\n"
      "}\n");
  ASSERT_FALSE(Binary.empty());
  ASSERT_EQ(neverd_session_load(Session, Binary.c_str()), 1)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const auto ReturnType = [&](const std::string &Page,
                              const std::string &Name) -> std::string {
    std::smatch Match;
    const std::regex Definition("(^|\n)([^ \n][^\n]*?)\\b" + Name +
                                "\\([^\n]*\\) \\{");
    return std::regex_search(Page, Match, Definition)
               ? llvm::StringRef(Match[2].str()).trim().str()
               : "";
  };
  const std::map<std::string, bool> Void = {{"sink", true},
                                            {"tail_void", true},
                                            {"value", false},
                                            {"tail_value", false},
                                            {"register_tm_clones", false}};
  for (const auto &[Name, IsVoid] : Void) {
    SCOPED_TRACE(Name);
    const int Index = neverd_func_find_by_name(Session, Name.c_str());
    ASSERT_GE(Index, 0);
    const neverd_va_t Entry = neverd_func_entry(Session, Index);
    const std::string HighC = takeString(neverd_decompile(Session, Entry));
    const std::string LLVMC = takeString(neverd_decompile_llvm(Session, Entry));
    EXPECT_EQ(ReturnType(HighC, Name) == "void", IsVoid) << HighC;
    EXPECT_EQ(ReturnType(LLVMC, Name) == "void", IsVoid) << LLVMC;
    if (Name == "register_tm_clones") {
      EXPECT_EQ(HighC.find("if ("), std::string::npos) << HighC;
      EXPECT_NE(LLVMC.find("return 0;"), std::string::npos) << LLVMC;
    }
  }
#endif
}

TEST_F(SessionCAPITest, StartAloneCallsTheBoundStartupImportWithMain) {
#if !defined(__linux__)
  GTEST_SKIP() << "builds a glibc program";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  const std::string Binary =
      buildProgram("startup", "int main(void) { return 0; }\n");
  ASSERT_FALSE(Binary.empty());
  ASSERT_EQ(neverd_session_load(Session, Binary.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const int Start = neverd_func_find_by_name(Session, "_start");
  if (Start < 0)
    GTEST_SKIP() << "the start file defines no _start symbol";
  const auto Entry = neverd_func_entry(Session, Start);
  // Only _start is lifted.  It passes main's address to __libc_start_main,
  // which it calls through the GOT slot the loader binds.
  const std::string LLVMC = takeString(neverd_decompile_llvm(Session, Entry));
  ASSERT_FALSE(LLVMC.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(LLVMC.find("__libc_start_main("), std::string::npos) << LLVMC;
  EXPECT_NE(LLVMC.find("main(void);"), std::string::npos) << LLVMC;
  const std::string HighC = takeString(neverd_decompile(Session, Entry));
  EXPECT_NE(HighC.find("__libc_start_main("), std::string::npos) << HighC;
#endif
}

TEST_F(SessionCAPITest, AnalyzedLLVMPagesEmitTheirFunctionAlone) {
#if !defined(__linux__)
  GTEST_SKIP() << "builds a glibc program";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // The puts import brings the lazy-binding stub, whose jump through the
  // loader's resolver slot the emitter refuses.
  const std::string Binary = buildProgram(
      "puts", "#include <stdio.h>\nint main(void) { puts(\"neverd\"); }\n");
  ASSERT_FALSE(Binary.empty());
  ASSERT_EQ(neverd_session_load(Session, Binary.c_str()), 1)
      << takeString(neverd_last_error(Session));
  // Analysis covers the whole program, as the workbench's does.
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const int Start = neverd_func_find_by_name(Session, "_start");
  if (Start < 0)
    GTEST_SKIP() << "the start file defines no _start symbol";
  const auto Entry = neverd_func_entry(Session, Start);
  // A page emits its own function; a refused sibling cannot hide it.
  const std::string LLVMC = takeString(neverd_decompile_llvm(Session, Entry));
  ASSERT_FALSE(LLVMC.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(LLVMC.find("__libc_start_main("), std::string::npos) << LLVMC;
  EXPECT_NE(LLVMC.find("main(void);"), std::string::npos) << LLVMC;
  const std::string IR = takeString(neverd_ir_llvm(Session, Entry));
  ASSERT_FALSE(IR.empty()) << takeString(neverd_last_error(Session));
  EXPECT_NE(IR.find("define "), std::string::npos) << IR;
#endif
}

TEST_F(SessionCAPITest, UserFunctionEditsShapeTheListAnalysisAndSidecar) {
  // The second routine is unreachable: only a user's edit makes it a
  // function.
  const auto Input = write(
      "edits.elf", makeNativeELF(false, 0x400000,
                                 std::string_view("\xb8\x09\0\0\0\xc3", 6)));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  const auto Second = Entry + 6;
  const std::string EntryHex = "0x" + llvm::utohexstr(Entry);
  const std::string SecondHex = "0x" + llvm::utohexstr(Second);
  ASSERT_GE(neverd_func_find_by_addr(Session, Entry), 0);
  ASSERT_LT(neverd_func_find_by_addr(Session, Second), 0);

  // A function starts in executable code where none does yet.
  EXPECT_EQ(neverd_func_create(Session, 0x10), -1);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("executable"),
            std::string::npos);
  EXPECT_EQ(neverd_func_create(Session, Entry), -1);
  ASSERT_EQ(neverd_func_create(Session, Second), 0)
      << takeString(neverd_last_error(Session));
  const int Created = neverd_func_find_by_addr(Session, Second);
  ASSERT_GE(Created, 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, Created)),
            "sub_" + llvm::utohexstr(Second));
  EXPECT_NE(takeString(neverd_decompile(Session, Second)).find("9"),
            std::string::npos)
      << takeString(neverd_last_error(Session));
  // Whole-program analysis keeps it; as the workbench does, it first lifts
  // the restriction the decompilation left.
  const auto analyzeAll = [&] {
    neverd_session_restrict_function(Session, 0);
    return neverd_session_analyze(Session);
  };
  ASSERT_EQ(analyzeAll(), 1);
  EXPECT_GE(neverd_func_find_by_addr(Session, Second), 0);

  // Saved, the edit comes back with the input.
  EXPECT_EQ(takeString(neverd_functions_json(Session)),
            "[{\"addr\":\"" + SecondHex + "\",\"state\":\"created\"}]");
  ASSERT_EQ(neverd_functions_save(Session), 0)
      << takeString(neverd_last_error(Session));
  {
    neverd_session_t Reopened = neverd_session_create();
    ASSERT_EQ(neverd_session_load(Reopened, Input.c_str()), 1);
    EXPECT_GE(neverd_func_find_by_addr(Reopened, Second), 0);
    neverd_session_destroy(Reopened);
  }

  // A deleted function stays deleted through analysis.  The last edit at an
  // address decides, and a function made again keeps its own name.
  const std::string EntryName = takeString(
      neverd_func_name(Session, neverd_func_find_by_addr(Session, Entry)));
  ASSERT_EQ(neverd_func_delete(Session, Entry), 0)
      << takeString(neverd_last_error(Session));
  EXPECT_LT(neverd_func_find_by_addr(Session, Entry), 0);
  ASSERT_EQ(analyzeAll(), 1);
  EXPECT_LT(neverd_func_find_by_addr(Session, Entry), 0);
  EXPECT_EQ(neverd_func_delete(Session, Entry), -1);
  ASSERT_EQ(neverd_func_delete(Session, Second), 0);
  EXPECT_LT(neverd_func_find_by_addr(Session, Second), 0);
  ASSERT_EQ(neverd_func_create(Session, Entry), 0);
  EXPECT_EQ(takeString(neverd_functions_json(Session)),
            "[{\"addr\":\"" + EntryHex +
                "\",\"state\":\"created\"},{\"addr\":\"" + SecondHex +
                "\",\"state\":\"deleted\"}]");
  ASSERT_EQ(analyzeAll(), 1);
  const int Restored = neverd_func_find_by_addr(Session, Entry);
  ASSERT_GE(Restored, 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, Restored)), EntryName);
  EXPECT_LT(neverd_func_find_by_addr(Session, Second), 0);
}

TEST_F(SessionCAPITest, UserDataItemsDefineBytesAndPersist) {
  // After the entry routine: "hello", then a word, in the image's one
  // segment.
  const auto Input = write(
      "items.elf", makeNativeELF(false, 0x400000,
                                 std::string_view("hello\0\x34\x12\0\0", 10)));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  const auto Text = Entry + 6, Word = Entry + 12;
  const std::string TextHex = "0x" + llvm::utohexstr(Text);
  const std::string WordHex = "0x" + llvm::utohexstr(Word);

  // The string reads where it starts, however short.
  EXPECT_EQ(takeString(neverd_string_at(Session, Text, nullptr)),
            "{\"addr\":\"" + TextHex +
                "\",\"chars\":5,\"encoding\":\"ascii\",\"length\":5,"
                "\"unit\":1,\"value\":\"hello\"}");
  EXPECT_EQ(neverd_string_at(Session, Text + 5, nullptr), nullptr);

  // Values of a size, strings in an encoding and undefined bytes.
  ASSERT_EQ(neverd_item_set(Session, Word, "{\"kind\":\"word\"}"), 0)
      << takeString(neverd_last_error(Session));
  ASSERT_EQ(neverd_item_set(Session, Text,
                            "{\"kind\":\"string\",\"encoding\":\"utf-8\","
                            "\"size\":6}"),
            0)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_items_json(Session)),
            "[{\"addr\":\"" + TextHex +
                "\",\"encoding\":\"utf-8\",\"kind\":\"string\","
                "\"size\":6},{\"addr\":\"" +
                WordHex + "\",\"kind\":\"word\",\"size\":2}]");
  // An item may not share bytes with another, a string ends in its
  // terminator, and a kind is one the tables name.
  EXPECT_EQ(neverd_item_set(Session, Text + 2, "{\"kind\":\"byte\"}"), -1);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("overlaps"),
            std::string::npos);
  EXPECT_EQ(neverd_item_set(Session, Text,
                            "{\"kind\":\"string\",\"encoding\":\"utf-8\","
                            "\"size\":5}"),
            -1);
  EXPECT_EQ(neverd_item_set(Session, Word, "{\"kind\":\"oword\"}"), -1);
  // Four bytes remain in the segment: a dword fits, a qword does not.
  EXPECT_EQ(neverd_item_set(Session, Word, "{\"kind\":\"qword\"}"), -1);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("segment"),
            std::string::npos);
  EXPECT_EQ(neverd_item_set(Session, Word, "{\"kind\":\"dword\"}"), 0);
  ASSERT_EQ(
      neverd_item_set(Session, Word, "{\"kind\":\"undefined\",\"size\":4}"), 0);
  // Undefined bytes give way to an item and stay undefined around it.
  ASSERT_EQ(neverd_item_set(Session, Word + 1, "{\"kind\":\"byte\"}"), 0)
      << takeString(neverd_last_error(Session));
  const std::string ByteHex = "0x" + llvm::utohexstr(Word + 1);
  const std::string RestHex = "0x" + llvm::utohexstr(Word + 2);
  EXPECT_EQ(takeString(neverd_items_json(Session)),
            "[{\"addr\":\"" + TextHex +
                "\",\"encoding\":\"utf-8\",\"kind\":\"string\","
                "\"size\":6},{\"addr\":\"" +
                WordHex + "\",\"kind\":\"undefined\",\"size\":1},{\"addr\":\"" +
                ByteHex + "\",\"kind\":\"byte\",\"size\":1},{\"addr\":\"" +
                RestHex + "\",\"kind\":\"undefined\",\"size\":2}]");

  // Saved, the items come back with the input.
  ASSERT_EQ(neverd_items_save(Session), 0)
      << takeString(neverd_last_error(Session));
  const std::string Saved = takeString(neverd_items_json(Session));
  {
    neverd_session_t Reopened = neverd_session_create();
    ASSERT_EQ(neverd_session_load(Reopened, Input.c_str()), 1);
    EXPECT_EQ(takeString(neverd_items_json(Reopened)), Saved);
    neverd_session_destroy(Reopened);
  }
  ASSERT_EQ(neverd_item_clear(Session, Word), 0);
  EXPECT_EQ(neverd_item_clear(Session, Word), -1);
}

TEST_F(SessionCAPITest, UserCodeItemsValidateInstructionsAndPersist) {
  for (bool Arm64 : {false, true}) {
    SCOPED_TRACE(Arm64);
    const std::string Tail =
        Arm64 ? std::string("\x1f\x20\x03\xd5\xc0\x03\x5f\xd6\x0f", 9)
              : std::string("\x90\x48\x83\xc4\x28\xc3\x0f", 7);
    const auto Input = write(Arm64 ? "code-arm64.elf" : "code-x64.elf",
                             makeNativeELF(Arm64, 0x400000, Tail));
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
    const auto Start = neverd_session_entry_addr(Session) + (Arm64 ? 8 : 6);
    const int FirstSize = Arm64 ? 4 : 1;
    ASSERT_EQ(neverd_item_set(Session, Start, "{\"kind\":\"code\"}"), 0)
        << takeString(neverd_last_error(Session));
    auto Rows = llvm::json::parse(takeString(neverd_items_json(Session)));
    ASSERT_TRUE(Rows && Rows->getAsArray());
    ASSERT_EQ(Rows->getAsArray()->size(), 1u);
    EXPECT_EQ(Rows->getAsArray()->front().getAsObject()->getInteger("size"),
              FirstSize);
    const auto Before = takeString(neverd_items_json(Session));
    EXPECT_EQ(neverd_item_set(Session, Start, "{\"kind\":\"code\",\"size\":2}"),
              -1);
    EXPECT_EQ(neverd_item_set(Session, Start + Tail.size() - 1,
                              "{\"kind\":\"code\"}"),
              -1);
    EXPECT_EQ(takeString(neverd_items_json(Session)), Before);
    const auto Next = Start + FirstSize;
    ASSERT_EQ(neverd_item_set(Session, Next, "{\"kind\":\"code\"}"), 0);
    ASSERT_EQ(neverd_items_save(Session), 0);
    const auto Saved = takeString(neverd_items_json(Session));
    neverd_session_t Reopened = neverd_session_create();
    ASSERT_EQ(neverd_session_load(Reopened, Input.c_str()), 1);
    EXPECT_EQ(takeString(neverd_items_json(Reopened)), Saved);
    neverd_session_destroy(Reopened);
    // A code definition is presentation state, not a new function entry.
    EXPECT_LT(neverd_func_find_by_addr(Session, Start), 0);
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

TEST_F(SessionCAPITest, SignatureCannotReplaceAStatedHexPlaceholderName) {
  for (const std::string Name : {"sub_400078", "func_aabb", "entry"}) {
    SCOPED_TRACE(Name);
    const auto Input = write(Name + ".elf", makeNamedNativeELF(Name));
    ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
    ASSERT_EQ(neverd_func_count(Session), 1);
    const auto Pattern = write(
        "identity.pat", "B807000000C3 00 0000 0006 :0000 library_guess\n");
    ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
    ASSERT_EQ(neverd_rename_func(Session, Name.c_str(), "user_choice"), 0);
    ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "user_choice");
    write(Name + ".elf.neverd-renames.json", "[]");
    ASSERT_EQ(neverd_renames_load(Session), 0);
    EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
  }
}

TEST_F(SessionCAPITest,
       SignatureIdentitySurvivesDiscoveryAndWithdrawsOnReload) {
  const auto Input = write("stripped-identity.elf", makeNativeELF(false));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  const auto Pattern =
      write("identity.pat", "B807000000C3 00 0000 0006 :0000 library_guess\n");
  ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 1);
  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  const int Index = neverd_func_find_by_addr(Session, Entry);
  ASSERT_GE(Index, 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, Index)), "library_guess");
  ASSERT_EQ(neverd_rename_func(Session, "library_guess", "user_choice"), 0);
  write("stripped-identity.elf.neverd-renames.json", "[]");
  ASSERT_EQ(neverd_renames_load(Session), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, Index)), "library_guess");
  write("identity.pat", "B809000000C3 00 0000 0006 :0000 other_guess\n");
  ASSERT_EQ(neverd_apply_signature_file(Session, Pattern.c_str()), 0);
  EXPECT_EQ(takeString(neverd_func_name(Session, Index)),
            "sub_" + llvm::utohexstr(Entry));
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  EXPECT_EQ(neverd_sig_match_count(Session), 0);
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
