//===- RuntimeSanitizerCLIContractTests.cpp - strict CLI contracts -------===//

#include "../TestProcess.h"
#include "NeverDSanitizerPublicationCLI.h"
#include "gtest/gtest.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#ifndef NEVERD_BINARY
#define NEVERD_BINARY "neverd"
#endif

#ifndef SAFETY_FIXTURE_ROOT
#define SAFETY_FIXTURE_ROOT ""
#endif

#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
#endif

namespace {

namespace fs = std::filesystem;

using neverd::cli::sanitizer_publication::SuccessDisposition;

static bool hasRuntimeFixtureCompiler() {
  return !llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty();
}

static neverd_sanitize_result_v1
completePublicationReceipt(SuccessDisposition Disposition) {
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  Result.ok = 1;
  Result.status = NEVERD_SANITIZE_STATUS_OK;
  Result.plan_version = 1;
  Result.publication_receipt_version = 1;
  Result.publication_receipt_complete = 1;
  if (Disposition == SuccessDisposition::CreatedExclusive) {
    Result.publication_outcome = NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED;
    Result.publication_namespace_disposition =
        NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE;
    Result.publication_guarantee_flags =
        NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC |
        NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE;
    Result.publication_operand_binding =
        NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_ACCESS_CONTROL_CONFINED_DISTINCT_CREDENTIALS;
  } else {
    Result.publication_outcome =
        NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED;
    Result.publication_namespace_disposition =
        NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NO_CHANGE;
    Result.publication_operand_binding =
        NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE;
  }
  return Result;
}

static uint32_t readLE32(llvm::StringRef Bytes, size_t Offset) {
  return uint32_t(static_cast<uint8_t>(Bytes[Offset])) |
         uint32_t(static_cast<uint8_t>(Bytes[Offset + 1])) << 8 |
         uint32_t(static_cast<uint8_t>(Bytes[Offset + 2])) << 16 |
         uint32_t(static_cast<uint8_t>(Bytes[Offset + 3])) << 24;
}

static size_t countAArch64LengthGuardsToTrap(llvm::StringRef Bytes) {
  size_t Count = 0;
  for (size_t Offset = 0; Offset + 8 <= Bytes.size(); Offset += 4) {
    const uint32_t Compare = readLE32(Bytes, Offset);
    // cmp Xn, #6 (SUBS XZR, Xn, #6), with only Xn unconstrained.
    if ((Compare & 0xfffffc1fu) != 0xf100181fu)
      continue;
    const uint32_t Branch = readLE32(Bytes, Offset + 4);
    // b.hs target: unsigned length >= 6 reaches the failure successor.
    if ((Branch & 0xff00001fu) != 0x54000002u)
      continue;
    int64_t Displacement = (Branch >> 5) & 0x7ffffu;
    if ((Displacement & (int64_t{1} << 18)) != 0)
      Displacement -= int64_t{1} << 19;
    const int64_t Target = static_cast<int64_t>(Offset + 4) + Displacement * 4;
    if (Target >= 0 && uint64_t(Target) + 4 <= Bytes.size() &&
        readLE32(Bytes, static_cast<size_t>(Target)) == 0xd4200020u)
      ++Count; // brk #1
  }
  return Count;
}

static size_t countX86LengthGuardsToTrap(llvm::StringRef Bytes) {
  size_t Count = 0;
  // The short form is only six bytes through the end of the branch.  Its
  // target may immediately contain the two-byte UD2, so do not impose the
  // near form's ten-byte instruction-prefix bound on every candidate.
  for (size_t Offset = 0; Offset + 6 <= Bytes.size(); ++Offset) {
    const auto Byte = [&](size_t Index) {
      return static_cast<uint8_t>(Bytes[Offset + Index]);
    };
    // cmp Rn, 6; accept any 64-bit general register, but require REX.W.
    if ((Byte(0) & 0xf8u) != 0x48u || Byte(1) != 0x83u ||
        (Byte(2) & 0xf8u) != 0xf8u || Byte(3) != 0x06u)
      continue;
    if (Byte(4) == 0x73u) { // short jae
      const int8_t Delta = static_cast<int8_t>(Byte(5));
      const int64_t Target = static_cast<int64_t>(Offset + 6) + Delta;
      if (Target >= 0 && uint64_t(Target) + 2 <= Bytes.size() &&
          static_cast<uint8_t>(Bytes[Target]) == 0x0fu &&
          static_cast<uint8_t>(Bytes[Target + 1]) == 0x0bu) {
        ++Count;
        continue;
      }
    }
    if (Offset + 10 > Bytes.size() || Byte(4) != 0x0fu || Byte(5) != 0x83u)
      continue;
    const int32_t Delta = static_cast<int32_t>(readLE32(Bytes, Offset + 6));
    const int64_t Target = static_cast<int64_t>(Offset + 10) + Delta;
    if (Target >= 0 && uint64_t(Target) + 2 <= Bytes.size() &&
        static_cast<uint8_t>(Bytes[Target]) == 0x0fu &&
        static_cast<uint8_t>(Bytes[Target + 1]) == 0x0bu)
      ++Count;
  }
  return Count;
}

static void appendLE32(std::string &Bytes, uint32_t Value) {
  for (unsigned Shift : {0u, 8u, 16u, 24u})
    Bytes.push_back(static_cast<char>((Value >> Shift) & 0xffu));
}

static std::string words(std::initializer_list<uint32_t> Values) {
  std::string Bytes;
  Bytes.reserve(Values.size() * sizeof(uint32_t));
  for (uint32_t Value : Values)
    appendLE32(Bytes, Value);
  return Bytes;
}

static std::string bytes(std::initializer_list<uint8_t> Values) {
  std::string Bytes;
  Bytes.reserve(Values.size());
  for (uint8_t Value : Values)
    Bytes.push_back(static_cast<char>(Value));
  return Bytes;
}

class RuntimeSanitizerCLI : public ::testing::Test {
protected:
  struct ProcessResult {
    int ExitCode = -1;
    std::string StandardOutput;
    std::string StandardError;
  };

  void SetUp() override {
    Directory =
        fs::temp_directory_path() /
        ("neverd-runtime-sanitizer-cli-" +
         std::to_string(neverd::test::currentProcessId()) + "-" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(Directory);
  }

  void TearDown() override {
    std::error_code Error;
    fs::remove_all(Directory, Error);
  }

  ProcessResult execute(llvm::StringRef Command) const {
    const fs::path StandardOutput = Directory / "stdout.txt";
    const fs::path StandardError = Directory / "stderr.txt";
    std::ofstream(StandardOutput, std::ios::binary | std::ios::trunc).close();
    std::ofstream(StandardError, std::ios::binary | std::ios::trunc).close();
    const std::string StandardOutputPath = StandardOutput.string();
    const std::string StandardErrorPath = StandardError.string();
#ifdef _WIN32
    const fs::path ScriptPath = Directory / "command.cmd";
    std::ofstream ScriptStream(ScriptPath, std::ios::binary | std::ios::trunc);
    ScriptStream << "@echo off\r\n" << Command.str() << "\r\n";
    ScriptStream.close();
    if (!ScriptStream)
      return {-1, {}, "cannot write command script"};

    llvm::ErrorOr<std::string> FoundShell =
        llvm::sys::findProgramByName("cmd.exe");
    if (!FoundShell)
      return {-1, {}, "cannot locate cmd.exe"};
    const std::string Shell = *FoundShell;
    const std::string Script = ScriptPath.string();
    const std::vector<llvm::StringRef> Arguments = {Shell, "/d",   "/s",
                                                    "/c",  "call", Script};
#else
    const std::string Shell = "/bin/sh";
    // Replacing the shell makes the timeout own the actual child rather than
    // leaving a grandchild behind after a signal or runtime trap.
    const std::string Script = "exec " + Command.str();
    const std::vector<llvm::StringRef> Arguments = {Shell, "-c", Script};
#endif
    const std::array<std::optional<llvm::StringRef>, 3> Redirects = {
        std::nullopt, llvm::StringRef(StandardOutputPath),
        llvm::StringRef(StandardErrorPath)};
    ProcessResult Result;
    std::string ExecutionError;
    bool ExecutionFailed = false;
    Result.ExitCode =
        llvm::sys::ExecuteAndWait(Shell, Arguments, std::nullopt, Redirects,
                                  /*SecondsToWait=*/30, /*MemoryLimit=*/0,
                                  &ExecutionError, &ExecutionFailed);
    Result.StandardOutput = read(StandardOutput);
    Result.StandardError = read(StandardError);
    if (ExecutionFailed || !ExecutionError.empty()) {
      if (!Result.StandardError.empty())
        Result.StandardError += '\n';
      Result.StandardError += ExecutionError.empty()
                                  ? "child process execution failed"
                                  : ExecutionError;
    }
    return Result;
  }

  ProcessResult run(llvm::StringRef Arguments) const {
    return execute(neverd::test::shellQuote(NEVERD_BINARY) + " " +
                   Arguments.str());
  }

  static std::string fixture() {
#ifdef _WIN32
    return std::string(SAFETY_FIXTURE_ROOT) + "/safety_cases_pe_x64.exe";
#elif defined(__APPLE__)
    return std::string(SAFETY_FIXTURE_ROOT) + "/safety_cases_macho_x64";
#else
    return std::string(SAFETY_FIXTURE_ROOT) + "/safety_cases_elf_x64";
#endif
  }

  std::string patchCommand(llvm::StringRef Options) const {
    return "patch " + Options.str() + " " +
           neverd::test::shellQuote(fixture()) + " -o " +
           neverd::test::shellQuote((Directory / "patched.bin").string());
  }

  fs::path runtimeFixtureSource() const {
    return fs::path(SAFETY_FIXTURE_ROOT).parent_path() /
           "runtime_sanitizer_cli.c";
  }

  fs::path runtimeFixtureBinary() const {
    return Directory / (std::string("runtime-sanitizer-fixture") +
                        neverd::test::executableSuffix());
  }

  fs::path emptyRuntimeFixtureSource() const {
    return fs::path(SAFETY_FIXTURE_ROOT).parent_path() /
           "runtime_sanitizer_empty.c";
  }

  fs::path emptyRuntimeFixtureBinary() const {
    return Directory / (std::string("runtime-sanitizer-empty") +
                        neverd::test::executableSuffix());
  }

#ifdef __APPLE__
  fs::path runtimeFixtureDSYM() const {
    return fs::path(runtimeFixtureBinary().string() + ".dSYM");
  }
#endif

  ProcessResult compileRuntimeFixture() const {
    const auto RunClang = [&](const std::vector<std::string> &Arguments) {
      std::string Command =
          neverd::test::shellQuote(NEVERD_RUNTIME_FIXTURE_COMPILER);
      for (const std::string &Argument : Arguments)
        Command += " " + neverd::test::shellQuote(Argument);
      return execute(Command);
    };
#ifdef __APPLE__
    // The surrounding Codex process may itself run under Rosetta, so compiler
    // predefined macros are not sufficient to select the physical host ISA.
    const std::string Architecture = appleHostIsArm64() ? "arm64" : "x86_64";
    const fs::path Object = Directory / "runtime-sanitizer-fixture.o";
    const ProcessResult Compiled =
        RunClang({"-O0", "-g", "-fno-builtin", "-fno-builtin-memcpy",
                  "-fno-stack-protector", "-arch", Architecture, "-c",
                  runtimeFixtureSource().string(), "-o", Object.string()});
    if (Compiled.ExitCode != 0)
      return Compiled;

    // ld's identityless signature has no requirements or special-slot hashes,
    // which is the strict sanitizer's sole automatic re-signing input class.
    // Keep this tiny runtime fixture out of compact unwind. The sanitizer
    // grows main deliberately, while a compact-unwind record has a fixed
    // authenticated range and the Mach-O patcher correctly refuses to invent
    // capacity beyond it.
    const ProcessResult Linked = RunClang(
        {Object.string(), "-o", runtimeFixtureBinary().string(), "-arch",
         Architecture, "-Wl,-adhoc_codesign", "-Wl,-no_compact_unwind"});
    if (Linked.ExitCode != 0)
      return Linked;

    // The loader accepts typed global extents from a companion dSYM only
    // after matching its UUID to the exact Mach-O snapshot. Keep the compile
    // object alive until explicit dsymutil completes so its debug map cannot
    // resolve to a driver-deleted temporary object.
    std::error_code RemoveError;
    fs::remove_all(runtimeFixtureDSYM(), RemoveError);
    if (RemoveError)
      return {
          -1, {}, "cannot replace test-owned dSYM: " + RemoveError.message()};
    ProcessResult DSYM = execute(
        "/usr/bin/dsymutil " +
        neverd::test::shellQuote(runtimeFixtureBinary().string()) + " -o " +
        neverd::test::shellQuote(runtimeFixtureDSYM().string()));
    if (DSYM.ExitCode != 0)
      return DSYM;
    return Linked;
#else
    return RunClang({"-O0", "-g", "-fno-builtin", "-fno-builtin-memcpy",
                     "-fno-stack-protector", runtimeFixtureSource().string(),
                     "-o", runtimeFixtureBinary().string()});
#endif
  }

  ProcessResult compileEmptyRuntimeFixture() const {
    std::string Command =
        neverd::test::shellQuote(NEVERD_RUNTIME_FIXTURE_COMPILER);
    const auto Add = [&](const std::string &Argument) {
      Command += " " + neverd::test::shellQuote(Argument);
    };
    Add("-O0");
    Add("-fno-stack-protector");
#ifdef __APPLE__
    Add("-arch");
    Add(appleHostIsArm64() ? "arm64" : "x86_64");
    Add("-Wl,-adhoc_codesign");
    Add("-Wl,-no_compact_unwind");
#endif
    Add(emptyRuntimeFixtureSource().string());
    Add("-o");
    Add(emptyRuntimeFixtureBinary().string());
    return execute(Command);
  }

  ProcessResult runRuntimeFixture(const fs::path &Binary,
                                  llvm::StringRef Input) const {
    const fs::path InputPath = Directory / "stdin.bin";
    std::ofstream Stream(InputPath, std::ios::binary | std::ios::trunc);
    Stream.write(Input.data(), static_cast<std::streamsize>(Input.size()));
    Stream.close();
    return execute(neverd::test::shellQuote(Binary.string()) + " <" +
                   neverd::test::shellQuote(InputPath.string()));
  }

  static void expectActionableError(const ProcessResult &Result,
                                    llvm::StringRef Message) {
    EXPECT_EQ(Result.ExitCode, 1);
    EXPECT_EQ(Result.StandardOutput.find("Patched binary written"),
              std::string::npos);
    EXPECT_NE(Result.StandardError.find(Message.str()), std::string::npos)
        << Result.StandardError;
  }

protected:
#ifdef __APPLE__
  static bool appleHostIsArm64() {
    int HasArm64 = 0;
    size_t HasArm64Size = sizeof(HasArm64);
    return sysctlbyname("hw.optional.arm64", &HasArm64, &HasArm64Size, nullptr,
                        0) == 0 &&
           HasArm64 != 0;
  }

  enum class DarwinPatchMode { Original, Section, Inplace };
  enum class DarwinInstructionSet { AArch64, X86_64 };
  enum class DarwinGuardPlacement { Main, Relocated };

  struct DarwinPatchReceipt {
    uint64_t CodeSize = 0;
    uint64_t TrampolineCount = 0;
  };

  struct FileBackedRange {
    std::string SegmentName;
    uint64_t Address = 0;
    uint64_t FileOffset = 0;
    uint64_t Size = 0;
  };

  struct DarwinExecutableLayout {
    DarwinInstructionSet InstructionSet = DarwinInstructionSet::AArch64;
    std::vector<FileBackedRange> Segments;
    FileBackedRange TextSection;
    std::optional<FileBackedRange> NdTextSegment;
  };

  static llvm::StringRef machOName(const char Name[16]) {
    return {Name, strnlen(Name, 16)};
  }

  static bool contains(const FileBackedRange &Outer, uint64_t Address,
                       uint64_t Size) {
    return Address >= Outer.Address && Address - Outer.Address <= Outer.Size &&
           Size <= Outer.Size - (Address - Outer.Address);
  }

  static std::optional<llvm::StringRef> rangeBytes(llvm::StringRef Bytes,
                                                   const FileBackedRange &Range,
                                                   uint64_t Address,
                                                   uint64_t Size) {
    if (!contains(Range, Address, Size))
      return std::nullopt;
    const uint64_t Delta = Address - Range.Address;
    if (Range.FileOffset > Bytes.size() ||
        Delta > Bytes.size() - Range.FileOffset ||
        Size > Bytes.size() - Range.FileOffset - Delta)
      return std::nullopt;
    return Bytes.substr(static_cast<size_t>(Range.FileOffset + Delta),
                        static_cast<size_t>(Size));
  }

  static std::optional<DarwinExecutableLayout>
  collectExecutableLayout(const llvm::object::MachOObjectFile &Object) {
    if (!Object.is64Bit())
      return std::nullopt;
    const llvm::MachO::mach_header_64 &Header = Object.getHeader64();
    if (Header.filetype != llvm::MachO::MH_EXECUTE)
      return std::nullopt;

    DarwinExecutableLayout Layout;
    if (Header.cputype == llvm::MachO::CPU_TYPE_ARM64)
      Layout.InstructionSet = DarwinInstructionSet::AArch64;
    else if (Header.cputype == llvm::MachO::CPU_TYPE_X86_64)
      Layout.InstructionSet = DarwinInstructionSet::X86_64;
    else
      return std::nullopt;

    const llvm::StringRef ObjectBytes = Object.getData();
    unsigned TextSections = 0;
    unsigned NdTextSegments = 0;
    for (const auto &Command : Object.load_commands()) {
      if (Command.C.cmd != llvm::MachO::LC_SEGMENT_64)
        continue;
      const llvm::MachO::segment_command_64 Segment =
          Object.getSegment64LoadCommand(Command);
      const uint64_t SectionBytes =
          uint64_t(Segment.nsects) * sizeof(llvm::MachO::section_64);
      if (Command.C.cmdsize < sizeof(llvm::MachO::segment_command_64) ||
          SectionBytes >
              Command.C.cmdsize - sizeof(llvm::MachO::segment_command_64) ||
          Segment.vmaddr >
              std::numeric_limits<uint64_t>::max() - Segment.filesize ||
          Segment.fileoff > ObjectBytes.size() ||
          Segment.filesize > ObjectBytes.size() - Segment.fileoff)
        return std::nullopt;

      const llvm::StringRef SegmentName = machOName(Segment.segname);
      const bool IsExecutable =
          (Segment.initprot & llvm::MachO::VM_PROT_EXECUTE) != 0;
      if (IsExecutable && Segment.filesize != 0) {
        Layout.Segments.push_back({SegmentName.str(), Segment.vmaddr,
                                   Segment.fileoff, Segment.filesize});
        if (SegmentName == "__NDTEXT") {
          if (++NdTextSegments != 1 || Segment.nsects != 0)
            return std::nullopt;
          Layout.NdTextSegment = Layout.Segments.back();
        }
      }

      for (uint32_t Index = 0; Index < Segment.nsects; ++Index) {
        const llvm::MachO::section_64 Section =
            Object.getSection64(Command, Index);
        const llvm::StringRef SectionName = machOName(Section.sectname);
        const llvm::StringRef SectionSegmentName = machOName(Section.segname);
        if (SectionName != "__text" || SectionSegmentName != "__TEXT" ||
            !IsExecutable ||
            (Section.flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                              llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) == 0)
          continue;
        if (++TextSections != 1 || SectionSegmentName != SegmentName ||
            Section.addr >
                std::numeric_limits<uint64_t>::max() - Section.size ||
            Section.offset > ObjectBytes.size() ||
            Section.size > ObjectBytes.size() - Section.offset ||
            Section.addr < Segment.vmaddr ||
            Section.addr - Segment.vmaddr > Segment.vmsize ||
            Section.size > Segment.vmsize - (Section.addr - Segment.vmaddr) ||
            Section.offset < Segment.fileoff ||
            Section.offset - Segment.fileoff > Segment.filesize ||
            Section.size >
                Segment.filesize - (Section.offset - Segment.fileoff))
          return std::nullopt;
        Layout.TextSection = {SegmentName.str(), Section.addr, Section.offset,
                              Section.size};
      }
    }
    if (TextSections != 1 || Layout.Segments.empty())
      return std::nullopt;

    std::sort(Layout.Segments.begin(), Layout.Segments.end(),
              [](const FileBackedRange &Left, const FileBackedRange &Right) {
                return Left.Address < Right.Address;
              });
    for (size_t Index = 1; Index < Layout.Segments.size(); ++Index) {
      const FileBackedRange &Previous = Layout.Segments[Index - 1];
      const FileBackedRange &Current = Layout.Segments[Index];
      if (Previous.Size > Current.Address - Previous.Address)
        return std::nullopt;
    }
    return Layout;
  }

  static std::optional<FileBackedRange>
  findMainFunctionRange(const llvm::object::MachOObjectFile &Object,
                        const FileBackedRange &TextSection) {
    std::optional<uint64_t> MainAddress;
    unsigned MainSymbols = 0;
    for (const llvm::object::SymbolRef &Symbol : Object.symbols()) {
      llvm::Expected<llvm::object::SymbolRef::Type> Type = Symbol.getType();
      if (!Type) {
        llvm::consumeError(Type.takeError());
        return std::nullopt;
      }
      // A -g fixture also carries an N_FUN STABS record named _main.  The
      // loader-authenticated entry is the one concrete function symbol.
      if (*Type != llvm::object::SymbolRef::ST_Function)
        continue;
      llvm::Expected<llvm::StringRef> Name = Symbol.getName();
      if (!Name) {
        llvm::consumeError(Name.takeError());
        return std::nullopt;
      }
      if (*Name != "_main")
        continue;
      llvm::Expected<uint64_t> Address = Symbol.getAddress();
      if (!Address) {
        llvm::consumeError(Address.takeError());
        return std::nullopt;
      }
      if (++MainSymbols != 1 || !contains(TextSection, *Address, 1))
        return std::nullopt;
      MainAddress = *Address;
    }
    if (MainSymbols != 1 || !MainAddress)
      return std::nullopt;

    if (TextSection.Address >
        std::numeric_limits<uint64_t>::max() - TextSection.Size)
      return std::nullopt;
    uint64_t MainEnd = TextSection.Address + TextSection.Size;
    for (const llvm::object::SymbolRef &Symbol : Object.symbols()) {
      llvm::Expected<llvm::object::SymbolRef::Type> Type = Symbol.getType();
      if (!Type) {
        llvm::consumeError(Type.takeError());
        return std::nullopt;
      }
      if (*Type != llvm::object::SymbolRef::ST_Function)
        continue;
      llvm::Expected<uint64_t> Address = Symbol.getAddress();
      if (!Address) {
        llvm::consumeError(Address.takeError());
        continue;
      }
      if (*Address > *MainAddress && *Address < MainEnd)
        MainEnd = *Address;
    }
    if (MainEnd <= *MainAddress)
      return std::nullopt;
    const uint64_t Delta = *MainAddress - TextSection.Address;
    return FileBackedRange{TextSection.SegmentName, *MainAddress,
                           TextSection.FileOffset + Delta,
                           MainEnd - *MainAddress};
  }

  static std::optional<uint64_t>
  decodeEntryTrampoline(llvm::StringRef MainBytes, uint64_t MainAddress,
                        DarwinInstructionSet InstructionSet) {
    auto AddSigned = [](uint64_t Base,
                        int64_t Delta) -> std::optional<uint64_t> {
      if (Delta < 0) {
        const uint64_t Magnitude = uint64_t(-(Delta + 1)) + 1;
        if (Magnitude > Base)
          return std::nullopt;
        return Base - Magnitude;
      }
      if (uint64_t(Delta) > std::numeric_limits<uint64_t>::max() - Base)
        return std::nullopt;
      return Base + uint64_t(Delta);
    };

    if (InstructionSet == DarwinInstructionSet::AArch64) {
      if (MainBytes.size() < 4)
        return std::nullopt;
      const uint32_t Branch = readLE32(MainBytes, 0);
      if ((Branch & 0xfc000000u) != 0x14000000u)
        return std::nullopt;
      int64_t Displacement = Branch & 0x03ffffffu;
      if ((Displacement & (int64_t{1} << 25)) != 0)
        Displacement -= int64_t{1} << 26;
      return AddSigned(MainAddress, Displacement * 4);
    }

    if (MainBytes.size() < 5 || static_cast<uint8_t>(MainBytes[0]) != 0xe9u)
      return std::nullopt;
    const int32_t Displacement = static_cast<int32_t>(readLE32(MainBytes, 1));
    if (MainAddress > std::numeric_limits<uint64_t>::max() - 5)
      return std::nullopt;
    return AddSigned(MainAddress + 5, Displacement);
  }

  static std::optional<DarwinPatchReceipt>
  parseDarwinPatchReceipt(llvm::StringRef StandardOutput) {
    constexpr llvm::StringLiteral Prefix = "Patched binary written to ";
    const size_t Line = StandardOutput.find(Prefix);
    if (Line == llvm::StringRef::npos ||
        StandardOutput.find(Prefix, Line + Prefix.size()) !=
            llvm::StringRef::npos)
      return std::nullopt;
    const size_t Open = StandardOutput.find(" (", Line + Prefix.size());
    if (Open == llvm::StringRef::npos)
      return std::nullopt;
    constexpr llvm::StringLiteral CodeSuffix = " bytes code, ";
    const size_t CodeEnd = StandardOutput.find(CodeSuffix, Open + 2);
    if (CodeEnd == llvm::StringRef::npos)
      return std::nullopt;
    constexpr llvm::StringLiteral TrampolineSuffix = " trampolines)";
    const size_t TrampolineBegin = CodeEnd + CodeSuffix.size();
    const size_t TrampolineEnd =
        StandardOutput.find(TrampolineSuffix, TrampolineBegin);
    if (TrampolineEnd == llvm::StringRef::npos)
      return std::nullopt;
    DarwinPatchReceipt Receipt;
    if (StandardOutput.slice(Open + 2, CodeEnd)
            .getAsInteger(10, Receipt.CodeSize) ||
        StandardOutput.slice(TrampolineBegin, TrampolineEnd)
            .getAsInteger(10, Receipt.TrampolineCount))
      return std::nullopt;
    return Receipt;
  }

  static std::optional<DarwinGuardPlacement>
  expectedDarwinGuardPlacement(DarwinPatchMode Mode,
                               const DarwinPatchReceipt &Receipt) {
    if (Mode == DarwinPatchMode::Original || Receipt.TrampolineCount > 1)
      return std::nullopt;
    if (Receipt.TrampolineCount == 0) {
      // Section patching always redirects the original entry to newly
      // published code.  A zero-trampoline receipt is only coherent with a
      // true in-place overwrite, whose aggregate relocated-code size is zero.
      if (Mode != DarwinPatchMode::Inplace || Receipt.CodeSize != 0)
        return std::nullopt;
      return DarwinGuardPlacement::Main;
    }
    if (Receipt.CodeSize == 0)
      return std::nullopt;
    return DarwinGuardPlacement::Relocated;
  }

  static size_t countForInstructionSet(llvm::StringRef Bytes,
                                       DarwinInstructionSet InstructionSet) {
    return InstructionSet == DarwinInstructionSet::AArch64
               ? countAArch64LengthGuardsToTrap(Bytes)
               : countX86LengthGuardsToTrap(Bytes);
  }

  static std::optional<size_t> countStaticDarwinViolationGuards(
      const fs::path &Path, DarwinPatchMode Mode,
      std::optional<DarwinPatchReceipt> Receipt = std::nullopt) {
    const std::string Bytes = read(Path);
    llvm::Expected<std::unique_ptr<llvm::object::Binary>> Parsed =
        llvm::object::createBinary(llvm::MemoryBufferRef(Bytes, Path.string()));
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      return std::nullopt;
    }
    const auto *Object =
        llvm::dyn_cast<llvm::object::MachOObjectFile>(Parsed->get());
    if (!Object)
      return std::nullopt;
    const std::optional<DarwinExecutableLayout> Layout =
        collectExecutableLayout(*Object);
    if (!Layout)
      return std::nullopt;
    const std::optional<FileBackedRange> Main =
        findMainFunctionRange(*Object, Layout->TextSection);
    if (!Main)
      return std::nullopt;
    const llvm::StringRef ObjectBytes = Object->getData();

    if (Mode == DarwinPatchMode::Original) {
      if (Receipt)
        return std::nullopt;
      size_t Count = 0;
      for (const FileBackedRange &Segment : Layout->Segments) {
        const std::optional<llvm::StringRef> SegmentBytes =
            rangeBytes(ObjectBytes, Segment, Segment.Address, Segment.Size);
        if (!SegmentBytes)
          return std::nullopt;
        Count += countForInstructionSet(*SegmentBytes, Layout->InstructionSet);
      }
      return Count;
    }

    if (!Receipt)
      return std::nullopt;
    const std::optional<DarwinGuardPlacement> Placement =
        expectedDarwinGuardPlacement(Mode, *Receipt);
    if (!Placement)
      return std::nullopt;

    FileBackedRange GuardedRange;
    if (*Placement == DarwinGuardPlacement::Main) {
      if (Layout->NdTextSegment)
        return std::nullopt;
      if (!contains(Layout->TextSection, Main->Address, Main->Size))
        return std::nullopt;
      GuardedRange = *Main;
    } else {
      if (!Layout->NdTextSegment)
        return std::nullopt;
      const std::optional<llvm::StringRef> MainBytes = rangeBytes(
          ObjectBytes, Layout->TextSection, Main->Address, Main->Size);
      if (!MainBytes)
        return std::nullopt;
      const std::optional<uint64_t> Target = decodeEntryTrampoline(
          *MainBytes, Main->Address, Layout->InstructionSet);
      if (!Target || *Target != Layout->NdTextSegment->Address ||
          !contains(*Layout->NdTextSegment, *Target, Receipt->CodeSize))
        return std::nullopt;
      GuardedRange = {Layout->NdTextSegment->SegmentName, *Target,
                      Layout->NdTextSegment->FileOffset, Receipt->CodeSize};
      const std::optional<llvm::StringRef> OriginalText =
          rangeBytes(ObjectBytes, Layout->TextSection,
                     Layout->TextSection.Address, Layout->TextSection.Size);
      if (!OriginalText ||
          countForInstructionSet(*OriginalText, Layout->InstructionSet) != 0)
        return std::nullopt;
    }

    const FileBackedRange *ContainingSegment = nullptr;
    unsigned ContainingSegments = 0;
    for (const FileBackedRange &Segment : Layout->Segments) {
      if (!contains(Segment, GuardedRange.Address, GuardedRange.Size))
        continue;
      ContainingSegment = &Segment;
      ++ContainingSegments;
    }
    if (ContainingSegments != 1 || !ContainingSegment)
      return std::nullopt;

    const std::optional<llvm::StringRef> GuardedBytes =
        rangeBytes(ObjectBytes, *ContainingSegment, GuardedRange.Address,
                   GuardedRange.Size);
    if (!GuardedBytes)
      return std::nullopt;
    const size_t GuardedCount =
        countForInstructionSet(*GuardedBytes, Layout->InstructionSet);

    size_t OutsideCount = 0;
    for (const FileBackedRange &Segment : Layout->Segments) {
      if (&Segment != ContainingSegment) {
        const std::optional<llvm::StringRef> SegmentBytes =
            rangeBytes(ObjectBytes, Segment, Segment.Address, Segment.Size);
        if (!SegmentBytes)
          return std::nullopt;
        OutsideCount +=
            countForInstructionSet(*SegmentBytes, Layout->InstructionSet);
        continue;
      }
      const uint64_t PrefixSize = GuardedRange.Address - Segment.Address;
      const uint64_t GuardedEnd = GuardedRange.Address + GuardedRange.Size;
      const uint64_t SegmentEnd = Segment.Address + Segment.Size;
      const std::optional<llvm::StringRef> Prefix =
          rangeBytes(ObjectBytes, Segment, Segment.Address, PrefixSize);
      const std::optional<llvm::StringRef> Suffix =
          rangeBytes(ObjectBytes, Segment, GuardedEnd, SegmentEnd - GuardedEnd);
      if (!Prefix || !Suffix)
        return std::nullopt;
      OutsideCount += countForInstructionSet(*Prefix, Layout->InstructionSet);
      OutsideCount += countForInstructionSet(*Suffix, Layout->InstructionSet);
    }
    if (OutsideCount != 0)
      return std::nullopt;
    return GuardedCount;
  }
#endif

  static std::string read(const fs::path &Path) {
    std::ifstream Stream(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(Stream),
            std::istreambuf_iterator<char>()};
  }

  fs::path Directory;
};

TEST(RuntimeSanitizerGuardDecoder,
     AArch64RequiresUnsigned64BitCompareAndExactTrap) {
  const std::string Valid =
      words({0xf100185fu, 0x54000042u, 0xd503201fu, 0xd4200020u});
  EXPECT_EQ(countAArch64LengthGuardsToTrap(Valid), 1u);

  const std::string SignedBranch =
      words({0xf100185fu, 0x5400004au, 0xd503201fu, 0xd4200020u});
  EXPECT_EQ(countAArch64LengthGuardsToTrap(SignedBranch), 0u); // b.ge

  const std::string NarrowCompare =
      words({0x7100185fu, 0x54000042u, 0xd503201fu, 0xd4200020u});
  EXPECT_EQ(countAArch64LengthGuardsToTrap(NarrowCompare), 0u); // cmp Wn

  const std::string WrongTrap =
      words({0xf100185fu, 0x54000042u, 0xd503201fu, 0xd4200000u});
  EXPECT_EQ(countAArch64LengthGuardsToTrap(WrongTrap), 0u);

  const std::string OutOfBounds =
      words({0xf100185fu, 0x54000082u, 0xd503201fu, 0xd4200020u});
  EXPECT_EQ(countAArch64LengthGuardsToTrap(OutOfBounds), 0u);
}

TEST(RuntimeSanitizerPublicationCLI,
     CompleteReceiptsHaveDistinctAndTruthfulSuccessMessages) {
  const std::string RequestedOutputPath = "/tmp/complete-receipt.bin";
  for (SuccessDisposition Disposition :
       {SuccessDisposition::CreatedExclusive,
        SuccessDisposition::AuthenticatedNoChange}) {
    SCOPED_TRACE(static_cast<unsigned>(Disposition));
    const neverd_sanitize_result_v1 Result =
        completePublicationReceipt(Disposition);
    EXPECT_FALSE(neverd::cli::sanitizer_publication::validateSuccessResult(
        Result, RequestedOutputPath));
    EXPECT_EQ(neverd::cli::sanitizer_publication::successDisposition(Result),
              Disposition);
  }
  EXPECT_STREQ(neverd::cli::sanitizer_publication::successMessage(
                   SuccessDisposition::CreatedExclusive),
               "created exclusively");
  EXPECT_STREQ(neverd::cli::sanitizer_publication::successMessage(
                   SuccessDisposition::AuthenticatedNoChange),
               "authenticated existing source / no namespace change");
}

TEST(RuntimeSanitizerPublicationCLI,
     SuccessPathRejectsIncompleteOrIncoherentPublicationReceipts) {
  const std::string RequestedOutputPath = "/tmp/rejected-success.bin";
  neverd_sanitize_result_v1 Result =
      completePublicationReceipt(SuccessDisposition::CreatedExclusive);
  Result.publication_receipt_complete = 0;
  std::optional<std::string> Error =
      neverd::cli::sanitizer_publication::validateSuccessResult(
          Result, RequestedOutputPath);
  ASSERT_TRUE(Error);
  EXPECT_NE(Error->find("destination may exist"), std::string::npos);
  EXPECT_NE(Error->find("inspection"), std::string::npos);
  EXPECT_NE(Error->find(RequestedOutputPath), std::string::npos);

  Result = completePublicationReceipt(SuccessDisposition::CreatedExclusive);
  Result.publication_outcome =
      NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED;
  Error = neverd::cli::sanitizer_publication::validateSuccessResult(
      Result, RequestedOutputPath);
  ASSERT_TRUE(Error);
  EXPECT_NE(Error->find("published outcome"), std::string::npos);
  EXPECT_NE(Error->find("destination state is unknown"), std::string::npos);
  EXPECT_NE(Error->find("destination may exist"), std::string::npos);
  EXPECT_NE(Error->find("inspect it"), std::string::npos);

  Result = completePublicationReceipt(SuccessDisposition::CreatedExclusive);
  Result.publication_guarantee_flags |=
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_COMPARE_AND_SWAP;
  Error = neverd::cli::sanitizer_publication::validateSuccessResult(
      Result, RequestedOutputPath);
  ASSERT_TRUE(Error);
  EXPECT_NE(Error->find("replacement compare-and-swap"), std::string::npos);

  Result = completePublicationReceipt(SuccessDisposition::CreatedExclusive);
  Result.publication_guarantee_flags |=
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_CRASH_DURABLE;
  Error = neverd::cli::sanitizer_publication::validateSuccessResult(
      Result, RequestedOutputPath);
  ASSERT_TRUE(Error);
  EXPECT_NE(Error->find("crash-durable"), std::string::npos);

  Result =
      completePublicationReceipt(SuccessDisposition::AuthenticatedNoChange);
  Result.publication_guarantee_flags =
      NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC;
  Error = neverd::cli::sanitizer_publication::validateSuccessResult(
      Result, RequestedOutputPath);
  ASSERT_TRUE(Error);
  EXPECT_NE(Error->find("namespace operation that did not occur"),
            std::string::npos);
}

TEST(RuntimeSanitizerPublicationCLI,
     MalformedTerminalTuplesRequireUnknownDestinationInspection) {
  neverd_sanitize_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  Result.status = NEVERD_SANITIZE_STATUS_PIPELINE_FAILED;
  Result.publication_receipt_version = NEVERD_SANITIZE_PUBLICATION_ABI_VERSION;
  Result.publication_outcome =
      NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED;
  EXPECT_FALSE(neverd::cli::sanitizer_publication::
                   terminalTupleRequiresUnknownDestinationAdvisory(0, Result));

  Result.ok = 1;
  EXPECT_TRUE(neverd::cli::sanitizer_publication::
                  terminalTupleRequiresUnknownDestinationAdvisory(0, Result));
  Result.ok = 0;
  Result.status = NEVERD_SANITIZE_STATUS_OK;
  EXPECT_TRUE(neverd::cli::sanitizer_publication::
                  terminalTupleRequiresUnknownDestinationAdvisory(0, Result));
  Result.status = NEVERD_SANITIZE_STATUS_PIPELINE_FAILED;

  EXPECT_TRUE(neverd::cli::sanitizer_publication::
                  terminalTupleRequiresUnknownDestinationAdvisory(1, Result));
  Result.struct_size = sizeof(Result) - 1;
  EXPECT_TRUE(neverd::cli::sanitizer_publication::
                  terminalTupleRequiresUnknownDestinationAdvisory(0, Result));
  Result.struct_size = sizeof(Result);
  Result.publication_outcome = 0xff;
  EXPECT_TRUE(neverd::cli::sanitizer_publication::
                  terminalTupleRequiresUnknownDestinationAdvisory(0, Result));
  Result.publication_outcome =
      NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED;
  Result.publication_guarantee_flags = 0x80000000u;
  EXPECT_TRUE(neverd::cli::sanitizer_publication::
                  terminalTupleRequiresUnknownDestinationAdvisory(0, Result));

  const std::string Message =
      neverd::cli::sanitizer_publication::unknownDestinationStateMessage(
          "native terminal tuple is inconsistent",
          "/tmp/malformed-terminal.bin");
  EXPECT_NE(Message.find("destination state is unknown"), std::string::npos);
  EXPECT_NE(Message.find("destination may exist"), std::string::npos);
  EXPECT_NE(Message.find("inspect it"), std::string::npos);
  EXPECT_NE(Message.find("/tmp/malformed-terminal.bin"), std::string::npos);
}

TEST(RuntimeSanitizerPublicationCLI,
     SuccessfulNativeCallWithoutOutputPathBindsAdvisoryToRequest) {
  const std::string RequestedOutputPath = "/tmp/missing-native-output.bin";
  std::optional<std::string> Error =
      neverd::cli::sanitizer_publication::validateSuccessOutputPath(
          nullptr, RequestedOutputPath);
  ASSERT_TRUE(Error);
  EXPECT_NE(Error->find("destination state is unknown"), std::string::npos);
  EXPECT_NE(Error->find("destination may exist"), std::string::npos);
  EXPECT_NE(Error->find("inspect it"), std::string::npos);
  EXPECT_NE(Error->find(RequestedOutputPath), std::string::npos);

  Error = neverd::cli::sanitizer_publication::validateSuccessOutputPath(
      "./native-normalized-output.bin", RequestedOutputPath);
  EXPECT_FALSE(Error);
}

TEST(RuntimeSanitizerPublicationCLI,
     FailurePresentationNeverSoftensUncertainDestinationState) {
  const std::string RequestedOutputPath = "/tmp/uncertain-destination.bin";
  neverd_sanitize_result_v1 Result{};
  Result.status = NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE;
  std::string Message = neverd::cli::sanitizer_publication::failureMessage(
      Result, "native publish callback failed", RequestedOutputPath);
  EXPECT_NE(Message.find("destination state is unknown"), std::string::npos);
  EXPECT_NE(Message.find("destination may exist"), std::string::npos);
  EXPECT_NE(Message.find("inspect it"), std::string::npos);
  EXPECT_NE(Message.find(RequestedOutputPath), std::string::npos);

  Result.status = NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE;
  Message = neverd::cli::sanitizer_publication::failureMessage(
      Result, "final authentication failed", RequestedOutputPath);
  EXPECT_NE(Message.find("destination may exist"), std::string::npos);
  EXPECT_NE(Message.find("receipt is incomplete"), std::string::npos);
  EXPECT_NE(Message.find("inspect it"), std::string::npos);
  EXPECT_NE(Message.find(RequestedOutputPath), std::string::npos);

  Result.status = NEVERD_SANITIZE_STATUS_PIPELINE_FAILED;
  Result.publication_outcome = NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED;
  Result.publication_receipt_complete = 1;
  Message = neverd::cli::sanitizer_publication::failureMessage(
      Result, "inconsistent native failure", RequestedOutputPath);
  EXPECT_NE(Message.find("failure after publishing the destination"),
            std::string::npos);
  EXPECT_NE(Message.find("inspect it"), std::string::npos);
  EXPECT_NE(Message.find(RequestedOutputPath), std::string::npos);
}

TEST(RuntimeSanitizerPublicationCLI,
     ExistingDestinationFailureExplainsMissingReplacementCAS) {
  neverd_sanitize_result_v1 Result{};
  Result.status = NEVERD_SANITIZE_STATUS_PUBLISH_FAILED;
  const std::string Message =
      neverd::cli::sanitizer_publication::failureMessage(
          Result, "distinct existing destination cannot be published",
          "/tmp/already-exists.bin");
  EXPECT_NE(Message.find("no authenticated replacement CAS"),
            std::string::npos);
  EXPECT_NE(Message.find("choose a new output path"), std::string::npos);

  const std::string Unrelated =
      neverd::cli::sanitizer_publication::failureMessage(
          Result, "candidate metadata authentication failed",
          "/tmp/prepublication-failure.bin");
  EXPECT_EQ(Unrelated.find("replacement CAS"), std::string::npos);
  EXPECT_EQ(Unrelated.find("destination state is unknown"), std::string::npos);
}

TEST(RuntimeSanitizerGuardDecoder, AArch64AcceptsBackwardUnsignedTrapEdge) {
  const std::string Backward =
      words({0xd4200020u, 0xd503201fu, 0xf100185fu, 0x54ffffa2u});
  EXPECT_EQ(countAArch64LengthGuardsToTrap(Backward), 1u);
}

TEST(RuntimeSanitizerGuardDecoder,
     X86ShortUnsignedBranchAcceptsMinimalForwardAndBackwardForms) {
  const std::string Forward =
      bytes({0x48, 0x83, 0xf8, 0x06, 0x73, 0x00, 0x0f, 0x0b});
  EXPECT_EQ(countX86LengthGuardsToTrap(Forward), 1u);

  const std::string Backward =
      bytes({0x0f, 0x0b, 0x90, 0x90, 0x48, 0x83, 0xf8, 0x06, 0x73, 0xf6});
  EXPECT_EQ(countX86LengthGuardsToTrap(Backward), 1u);
}

TEST(RuntimeSanitizerGuardDecoder,
     X86NearUnsignedBranchAcceptsForwardAndBackwardForms) {
  const std::string Forward = bytes(
      {0x48, 0x83, 0xf8, 0x06, 0x0f, 0x83, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x0b});
  EXPECT_EQ(countX86LengthGuardsToTrap(Forward), 1u);

  const std::string Backward =
      bytes({0x0f, 0x0b, 0x90, 0x90, 0x48, 0x83, 0xf8, 0x06, 0x0f, 0x83, 0xf2,
             0xff, 0xff, 0xff});
  EXPECT_EQ(countX86LengthGuardsToTrap(Backward), 1u);
}

TEST(RuntimeSanitizerGuardDecoder,
     X86RejectsSignedNarrowWrongTrapAndOutOfBoundsForms) {
  const std::string Signed =
      bytes({0x48, 0x83, 0xf8, 0x06, 0x7d, 0x00, 0x0f, 0x0b});
  EXPECT_EQ(countX86LengthGuardsToTrap(Signed), 0u); // jge

  const std::string SignedNear = bytes(
      {0x48, 0x83, 0xf8, 0x06, 0x0f, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x0b});
  EXPECT_EQ(countX86LengthGuardsToTrap(SignedNear), 0u); // near jge

  const std::string Narrow =
      bytes({0x40, 0x83, 0xf8, 0x06, 0x73, 0x00, 0x0f, 0x0b});
  EXPECT_EQ(countX86LengthGuardsToTrap(Narrow), 0u); // no REX.W

  const std::string WrongTrap =
      bytes({0x48, 0x83, 0xf8, 0x06, 0x73, 0x00, 0x90, 0x90});
  EXPECT_EQ(countX86LengthGuardsToTrap(WrongTrap), 0u);

  const std::string ShortOutOfBounds =
      bytes({0x48, 0x83, 0xf8, 0x06, 0x73, 0x7f});
  EXPECT_EQ(countX86LengthGuardsToTrap(ShortOutOfBounds), 0u);

  const std::string NearOutOfBounds =
      bytes({0x48, 0x83, 0xf8, 0x06, 0x0f, 0x83, 0x7f, 0x00, 0x00, 0x00});
  EXPECT_EQ(countX86LengthGuardsToTrap(NearOutOfBounds), 0u);
}

#ifdef __APPLE__
TEST_F(RuntimeSanitizerCLI,
       DarwinReceiptSelectsOnlyCoherentSingleFunctionGuardPlacement) {
  const std::optional<DarwinPatchReceipt> Inplace = parseDarwinPatchReceipt(
      "Patched binary written to out (0 bytes code, 0 trampolines)\n");
  ASSERT_TRUE(Inplace);
  const std::optional<DarwinGuardPlacement> InplacePlacement =
      expectedDarwinGuardPlacement(DarwinPatchMode::Inplace, *Inplace);
  ASSERT_TRUE(InplacePlacement);
  EXPECT_EQ(*InplacePlacement, DarwinGuardPlacement::Main);
  EXPECT_FALSE(
      expectedDarwinGuardPlacement(DarwinPatchMode::Section, *Inplace));
  EXPECT_FALSE(
      expectedDarwinGuardPlacement(DarwinPatchMode::Original, *Inplace));

  const std::optional<DarwinPatchReceipt> Relocated = parseDarwinPatchReceipt(
      "Patched binary written to out (272 bytes code, 1 trampolines)\n");
  ASSERT_TRUE(Relocated);
  for (DarwinPatchMode Mode :
       {DarwinPatchMode::Section, DarwinPatchMode::Inplace}) {
    const std::optional<DarwinGuardPlacement> Placement =
        expectedDarwinGuardPlacement(Mode, *Relocated);
    ASSERT_TRUE(Placement);
    EXPECT_EQ(*Placement, DarwinGuardPlacement::Relocated);
  }

  EXPECT_FALSE(expectedDarwinGuardPlacement(DarwinPatchMode::Inplace,
                                            DarwinPatchReceipt{272, 0}));
  EXPECT_FALSE(expectedDarwinGuardPlacement(DarwinPatchMode::Section,
                                            DarwinPatchReceipt{0, 1}));
  EXPECT_FALSE(expectedDarwinGuardPlacement(DarwinPatchMode::Inplace,
                                            DarwinPatchReceipt{272, 2}));
}

TEST_F(RuntimeSanitizerCLI, DarwinReceiptParserRejectsAmbiguousOutput) {
  EXPECT_FALSE(parseDarwinPatchReceipt(
      "Patched binary written to out (272 bytes code, one trampolines)\n"));
  EXPECT_FALSE(parseDarwinPatchReceipt(
      "Patched binary written to a (272 bytes code, 1 trampolines)\n"
      "Patched binary written to b (272 bytes code, 1 trampolines)\n"));
}
#endif

TEST_F(RuntimeSanitizerCLI, HelpAdvertisesTheOnlySupportedPolicy) {
  const ProcessResult Result = run("patch --help");
  EXPECT_EQ(Result.ExitCode, 0);
  EXPECT_TRUE(Result.StandardError.empty());
  EXPECT_NE(Result.StandardOutput.find("--sanitize"), std::string::npos);
  EXPECT_NE(Result.StandardOutput.find("strict"), std::string::npos);
}

TEST_F(RuntimeSanitizerCLI, RejectsMissingEmptyUnknownAndRepeatedPolicies) {
  struct Case {
    const char *Options;
    const char *Message;
  };
  const Case Cases[] = {
      {"--sanitize", "--sanitize requires a value; supported value: strict"},
      {"--sanitize=", "--sanitize requires a value; supported value: strict"},
      {"--sanitize=relaxed",
       "unsupported --sanitize value 'relaxed'; supported value: strict"},
      {"--sanitize=strict --sanitize=strict",
       "--sanitize may be specified at most once"},
  };
  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Options);
    expectActionableError(run(patchCommand(Entry.Options)), Entry.Message);
  }
}

TEST_F(RuntimeSanitizerCLI, StrictPolicyRejectsEveryAmbiguousPipelineMode) {
  struct Case {
    const char *Option;
    const char *Flag;
  };
  const Case Cases[] = {
      {"--from-ir=ignored.ll", "--from-ir"},
      {"--from-c=ignored.c", "--from-c"},
      {"--func=401000", "--func"},
      {"--max-func=0", "--max-func"},
      {"--hello", "--hello"},
      {"--nop", "--nop"},
  };
  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Option);
    const ProcessResult Result =
        run(patchCommand("--sanitize=strict " + std::string(Entry.Option)));
    expectActionableError(Result, "--sanitize=strict cannot be combined with " +
                                      std::string(Entry.Flag));
  }
}

#ifdef __APPLE__
TEST_F(RuntimeSanitizerCLI,
       StrictSectionAndInplacePreserveTypedGlobalBoundaryAndTrapOnePastEnd) {
  if (!hasRuntimeFixtureCompiler())
    GTEST_SKIP() << "runtime sanitizer fixture tests require a GNU/Clang host "
                    "C compiler";
  const ProcessResult Compiled = compileRuntimeFixture();
  ASSERT_EQ(Compiled.ExitCode, 0) << Compiled.StandardError;
  ASSERT_TRUE(fs::is_regular_file(runtimeFixtureBinary()));
  ASSERT_TRUE(fs::is_directory(runtimeFixtureDSYM()));
  const std::optional<size_t> OriginalGuardCount =
      countStaticDarwinViolationGuards(runtimeFixtureBinary(),
                                       DarwinPatchMode::Original);
  ASSERT_TRUE(OriginalGuardCount);
  EXPECT_EQ(*OriginalGuardCount, 0u);

  const ProcessResult Original =
      runRuntimeFixture(runtimeFixtureBinary(), "ABCDE");
  ASSERT_EQ(Original.ExitCode, 0) << Original.StandardError;
  ASSERT_EQ(Original.StandardOutput, "ABCDE");

  for (llvm::StringRef Mode :
       {llvm::StringRef("section"), llvm::StringRef("inplace")}) {
    SCOPED_TRACE(Mode.str());
    const fs::path Patched =
        runtimeFixtureBinary().parent_path() /
        ("patched-" + Mode.str() + neverd::test::executableSuffix());
    const ProcessResult Sanitized =
        run("patch --sanitize=strict --mode=" + Mode.str() + " -o " +
            neverd::test::shellQuote(Patched.string()) + " " +
            neverd::test::shellQuote(runtimeFixtureBinary().string()));
    ASSERT_EQ(Sanitized.ExitCode, 0)
        << Sanitized.StandardOutput << Sanitized.StandardError;
    EXPECT_TRUE(Sanitized.StandardError.empty());
    EXPECT_NE(Sanitized.StandardOutput.find("guarded sites: 1"),
              std::string::npos);
    EXPECT_NE(Sanitized.StandardOutput.find("unsupported sites: 0"),
              std::string::npos);
    EXPECT_NE(Sanitized.StandardOutput.find("patched functions: 1"),
              std::string::npos);
    EXPECT_NE(Sanitized.StandardOutput.find(
                  "strict sanitizer publication: created exclusively"),
              std::string::npos);
    ASSERT_TRUE(fs::is_regular_file(Patched));

    const std::optional<DarwinPatchReceipt> Receipt =
        parseDarwinPatchReceipt(Sanitized.StandardOutput);
    ASSERT_TRUE(Receipt);
    const ProcessResult Signature =
        execute("/usr/bin/codesign --verify --strict " +
                neverd::test::shellQuote(Patched.string()));
    ASSERT_EQ(Signature.ExitCode, 0) << Signature.StandardError;

    const ProcessResult Exact = runRuntimeFixture(Patched, "ABCDE");
    EXPECT_EQ(Exact.ExitCode, Original.ExitCode) << Exact.StandardError;
    EXPECT_EQ(Exact.StandardOutput, Original.StandardOutput);
    EXPECT_EQ(Exact.StandardError, Original.StandardError);

    // The desktop host installs an inherited Mach exception server that can
    // retain BRK/UD2 children even after SIGKILL. Decode this guarded
    // callsite's exact unsigned threshold and failure successor instead; the
    // pass matrix executes the corresponding len=6-to-llvm.trap IR edge.
    const DarwinPatchMode InspectionMode =
        Mode == "section" ? DarwinPatchMode::Section : DarwinPatchMode::Inplace;
    const std::optional<size_t> GuardCount =
        countStaticDarwinViolationGuards(Patched, InspectionMode, *Receipt);
    ASSERT_TRUE(GuardCount);
    EXPECT_EQ(*GuardCount, 1u);
  }
}

TEST_F(RuntimeSanitizerCLI,
       EmptyPlanCanAuthenticateTheExistingSourceWithoutNamespaceMutation) {
  if (!hasRuntimeFixtureCompiler())
    GTEST_SKIP() << "runtime sanitizer fixture tests require a GNU/Clang host "
                    "C compiler";
  const ProcessResult Compiled = compileEmptyRuntimeFixture();
  ASSERT_EQ(Compiled.ExitCode, 0) << Compiled.StandardError;
  const fs::path Source = fs::canonical(emptyRuntimeFixtureBinary());
  const std::string Before = read(Source);
  ASSERT_FALSE(Before.empty());

  const ProcessResult Sanitized =
      run("patch --sanitize=strict --mode=section -o " +
          neverd::test::shellQuote(Source.string()) + " " +
          neverd::test::shellQuote(Source.string()));
  ASSERT_EQ(Sanitized.ExitCode, 0)
      << Sanitized.StandardOutput << Sanitized.StandardError;
  EXPECT_TRUE(Sanitized.StandardError.empty());
  EXPECT_EQ(Sanitized.StandardOutput.find("Patched binary written"),
            std::string::npos);
  EXPECT_NE(Sanitized.StandardOutput.find("Existing binary authenticated at"),
            std::string::npos);
  EXPECT_NE(
      Sanitized.StandardOutput.find(
          "strict sanitizer publication: authenticated existing source / no "
          "namespace change"),
      std::string::npos);
  EXPECT_EQ(read(Source), Before);
}

TEST_F(RuntimeSanitizerCLI,
       ExistingDistinctDestinationIsPreservedAndExplainsV1ReplacementLimit) {
  if (!hasRuntimeFixtureCompiler())
    GTEST_SKIP() << "runtime sanitizer fixture tests require a GNU/Clang host "
                    "C compiler";
  const ProcessResult Compiled = compileRuntimeFixture();
  ASSERT_EQ(Compiled.ExitCode, 0) << Compiled.StandardError;
  const fs::path Destination = Directory / "already-exists.bin";
  const std::string Sentinel = "existing destination sentinel";
  {
    std::ofstream Stream(Destination, std::ios::binary | std::ios::trunc);
    Stream << Sentinel;
  }

  const ProcessResult Sanitized =
      run("patch --sanitize=strict --mode=section -o " +
          neverd::test::shellQuote(Destination.string()) + " " +
          neverd::test::shellQuote(runtimeFixtureBinary().string()));
  expectActionableError(Sanitized, "v1 does not support replacement CAS");
  EXPECT_NE(Sanitized.StandardError.find("choose a new output path"),
            std::string::npos);
  EXPECT_EQ(read(Destination), Sentinel);
}
#else
TEST_F(RuntimeSanitizerCLI,
       StrictSanitizeFailsBeforeMutationWithoutNativePublicationReceipt) {
  if (!hasRuntimeFixtureCompiler())
    GTEST_SKIP() << "runtime sanitizer fixture tests require a GNU/Clang host "
                    "C compiler";
  const ProcessResult Compiled = compileRuntimeFixture();
  ASSERT_EQ(Compiled.ExitCode, 0) << Compiled.StandardError;
  ASSERT_TRUE(fs::is_regular_file(runtimeFixtureBinary()));

  const ProcessResult Original =
      runRuntimeFixture(runtimeFixtureBinary(), "ABCDE");
  ASSERT_EQ(Original.ExitCode, 0) << Original.StandardError;
  ASSERT_EQ(Original.StandardOutput, "ABCDE");

  for (llvm::StringRef Mode :
       {llvm::StringRef("section"), llvm::StringRef("inplace")}) {
    SCOPED_TRACE(Mode.str());
    const fs::path Patched =
        runtimeFixtureBinary().parent_path() /
        ("patched-" + Mode.str() + neverd::test::executableSuffix());
    const ProcessResult Sanitized =
        run("patch --sanitize=strict --mode=" + Mode.str() + " -o " +
            neverd::test::shellQuote(Patched.string()) + " " +
            neverd::test::shellQuote(runtimeFixtureBinary().string()));
    expectActionableError(Sanitized, "authenticated sanitizer publication");
    EXPECT_NE(Sanitized.StandardError.find("unsupported"), std::string::npos);
    EXPECT_FALSE(fs::exists(Patched));
  }
}
#endif

} // namespace
