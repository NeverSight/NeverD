//===- SBFExternalOracleTests.cpp - Out-of-process sbpf oracle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SBFFixtureBuilder.h"
#include "SBFSourceDifferentialDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

#define SBF_OFFICIAL_ORACLE_STRING(ID, RUST_ID, VALUE)                         \
  constexpr llvm::StringLiteral kOracle##ID(VALUE);
#include "SBFOfficialOracleProtocol.def"

#define SBF_UPSTREAM_SOURCE(ID, NAME, REVISION)                                \
  constexpr llvm::StringLiteral k##ID##Revision(REVISION);
#include "neverd/sbf/runtime/SBFUpstreamSources.def"

enum class LoadExpectation : uint8_t { Accept, Reject };
enum class VMProfile : uint8_t {
#define SBF_UPSTREAM_VM_PROFILE(ID, OPTIMIZE_RODATA) ID,
#include "SBFUpstreamProfiles.def"
};
enum class InputProfile : uint8_t {
#define SBF_UPSTREAM_INPUT_PROFILE(ID, FIRST_BYTE) ID,
#include "SBFUpstreamProfiles.def"
};
enum class SyscallProfile : uint8_t {
#define SBF_UPSTREAM_SYSCALL_PROFILE(ID, NAME) ID,
#include "SBFUpstreamProfiles.def"
};
enum class ExecutionExpectation : uint8_t {
  NotRun,
  Return,
  FaultCallDepth,
};

struct VMProfileDefinition {
  bool OptimizeRodata;
};

struct InputProfileDefinition {
  uint8_t FirstByte;
};

struct SyscallProfileDefinition {
  llvm::StringLiteral Name;
};

constexpr std::array VMProfiles = {
#define SBF_UPSTREAM_VM_PROFILE(ID, OPTIMIZE_RODATA)                           \
  VMProfileDefinition{OPTIMIZE_RODATA},
#include "SBFUpstreamProfiles.def"
};

constexpr std::array InputProfiles = {
#define SBF_UPSTREAM_INPUT_PROFILE(ID, FIRST_BYTE)                             \
  InputProfileDefinition{FIRST_BYTE},
#include "SBFUpstreamProfiles.def"
};

constexpr std::array SyscallProfiles = {
#define SBF_UPSTREAM_SYSCALL_PROFILE(ID, NAME) SyscallProfileDefinition{NAME},
#include "SBFUpstreamProfiles.def"
};

struct FixtureExpectation {
  const char *File;
  Version TheVersion;
  LoadExpectation Load;
  VMProfile VM;
  InputProfile Input;
  SyscallProfile Syscalls;
  uint64_t Budget;
  ExecutionExpectation Execution;
  uint64_t Result;
};

constexpr FixtureExpectation Fixtures[] = {
#define SBF_UPSTREAM_ELF(FILE, VERSION, LOAD, VM_PROFILE, INPUT_PROFILE,       \
                         SYSCALL_PROFILE, BUDGET, EXECUTION, RESULT)           \
  {FILE,                                                                       \
   Version::VERSION,                                                           \
   LoadExpectation::LOAD,                                                      \
   VMProfile::VM_PROFILE,                                                      \
   InputProfile::INPUT_PROFILE,                                                \
   SyscallProfile::SYSCALL_PROFILE,                                            \
   BUDGET,                                                                     \
   ExecutionExpectation::EXECUTION,                                            \
   RESULT},
#include "SBFUpstreamManifest.def"
};

constexpr unsigned kOracleProcessTimeoutSeconds = 30;
constexpr uint64_t kRawExecutionBudget = 16;
constexpr std::array kOracleVersions = {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  Version::NAME,
#include "neverd/sbf/image/SBFVersions.def"
};
constexpr size_t kConcreteVersionCount = 0
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  +1
#include "neverd/sbf/image/SBFVersions.def"
    ;
static_assert(kOracleVersions.size() == kConcreteVersionCount,
              "official oracle matrix must contain every concrete version");
constexpr Version kMinimumOracleVersion = kOracleVersions.front();
constexpr Version kMaximumOracleVersion = kOracleVersions.back();
constexpr unsigned kOpcodeEncodingCount =
    static_cast<unsigned>(std::numeric_limits<uint8_t>::max()) + 1;
constexpr size_t kExhaustiveEncodingCaseCount =
    kOracleVersions.size() * kOpcodeEncodingCount;

enum class OracleOutcomeKind : uint8_t {
  Accepted,
  Rejected,
  Returned,
  CallDepthFault,
  OtherFault,
};

llvm::StringRef outcomeName(OracleOutcomeKind Kind) {
  switch (Kind) {
  case OracleOutcomeKind::Accepted:
    return kOracleAccepted;
  case OracleOutcomeKind::Rejected:
    return kOracleRejected;
  case OracleOutcomeKind::Returned:
    return kOracleReturned;
  case OracleOutcomeKind::CallDepthFault:
    return kOracleCallDepthFault;
  case OracleOutcomeKind::OtherFault:
    return kOracleOtherFault;
  }
  llvm_unreachable("unknown oracle outcome kind");
}

struct OracleOutcome {
  OracleOutcomeKind Kind = OracleOutcomeKind::OtherFault;
  uint64_t Result = 0;
  uint64_t InstructionCount = 0;

  friend bool operator==(const OracleOutcome &,
                         const OracleOutcome &) = default;
};

struct OracleProcessResult {
  int ExitCode = -1;
  std::string StandardOutput;
  std::string StandardError;
  std::string ExecutionError;
};

class TemporaryOutputPair {
public:
  TemporaryOutputPair() {
    Error = llvm::sys::fs::createTemporaryFile("neverd-sbf-oracle", "stdout",
                                               StandardOutputPath);
    if (!Error)
      Error = llvm::sys::fs::createTemporaryFile("neverd-sbf-oracle", "stderr",
                                                 StandardErrorPath);
  }

  ~TemporaryOutputPair() {
    if (!StandardOutputPath.empty())
      llvm::sys::fs::remove(StandardOutputPath);
    if (!StandardErrorPath.empty())
      llvm::sys::fs::remove(StandardErrorPath);
  }

  std::error_code error() const { return Error; }
  llvm::StringRef standardOutputPath() const { return StandardOutputPath; }
  llvm::StringRef standardErrorPath() const { return StandardErrorPath; }

private:
  llvm::SmallString<128> StandardOutputPath;
  llvm::SmallString<128> StandardErrorPath;
  std::error_code Error;
};

std::string readFile(llvm::StringRef Path) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  if (!Buffer)
    return {};
  return (*Buffer)->getBuffer().str();
}

OracleProcessResult runOracle(llvm::StringRef Executable,
                              llvm::ArrayRef<llvm::StringRef> Arguments) {
  TemporaryOutputPair Output;
  OracleProcessResult Result;
  if (Output.error()) {
    Result.ExecutionError = Output.error().message();
    return Result;
  }

  llvm::SmallVector<llvm::StringRef, 24> FullArguments;
  FullArguments.push_back(Executable);
  FullArguments.append(Arguments.begin(), Arguments.end());
  std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, Output.standardOutputPath(), Output.standardErrorPath()};
  Result.ExitCode = llvm::sys::ExecuteAndWait(
      Executable, FullArguments, std::nullopt, Redirects,
      kOracleProcessTimeoutSeconds, /*MemoryLimit=*/0, &Result.ExecutionError);
  Result.StandardOutput = readFile(Output.standardOutputPath());
  Result.StandardError = readFile(Output.standardErrorPath());
  return Result;
}

llvm::Expected<llvm::StringRef> protocolLine(llvm::StringRef Output) {
  std::optional<llvm::StringRef> Record;
  while (!Output.empty()) {
    auto [Line, Rest] = Output.split('\n');
    Line = Line.trim();
    Output = Rest;
    if (Line.empty())
      continue;
    if (!Line.starts_with(kOracleProtocol))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "oracle emitted non-protocol output '%s'",
                                     Line.str().c_str());
    if (Record)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "oracle emitted more than one protocol record");
    Record = Line;
  }
  if (!Record)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "oracle emitted no protocol record");
  return *Record;
}

llvm::Expected<uint64_t> parseUnsigned(llvm::StringRef Value) {
  uint64_t Result = 0;
  if (Value.getAsInteger(10, Result))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "invalid unsigned oracle value '%s'",
                                   Value.str().c_str());
  return Result;
}

llvm::Expected<OracleOutcome> parseOutcome(llvm::StringRef Output) {
  auto Line = protocolLine(Output);
  if (!Line)
    return Line.takeError();

  llvm::SmallVector<llvm::StringRef, 5> Fields;
  Line->split(Fields, ' ', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  if (Fields.size() < 2 || Fields[0] != kOracleProtocol)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "malformed oracle record '%s'",
                                   Line->str().c_str());

  if (Fields[1] == kOracleAccepted && Fields.size() == 2)
    return OracleOutcome{OracleOutcomeKind::Accepted};
  if (Fields[1] == kOracleRejected && Fields.size() == 2)
    return OracleOutcome{OracleOutcomeKind::Rejected};
  if (Fields[1] == kOracleReturned && Fields.size() == 4) {
    auto Result = parseUnsigned(Fields[2]);
    if (!Result)
      return Result.takeError();
    auto Count = parseUnsigned(Fields[3]);
    if (!Count)
      return Count.takeError();
    return OracleOutcome{OracleOutcomeKind::Returned, *Result, *Count};
  }
  if (Fields[1] == kOracleCallDepthFault && Fields.size() == 3) {
    auto Count = parseUnsigned(Fields[2]);
    if (!Count)
      return Count.takeError();
    return OracleOutcome{OracleOutcomeKind::CallDepthFault, 0, *Count};
  }
  if (Fields[1] == kOracleOtherFault && Fields.size() == 3) {
    auto Count = parseUnsigned(Fields[2]);
    if (!Count)
      return Count.takeError();
    return OracleOutcome{OracleOutcomeKind::OtherFault, 0, *Count};
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unknown oracle record '%s'",
                                 Line->str().c_str());
}

SBFVMConfig makeVMConfig(VMProfile Profile) {
  const size_t Index = static_cast<size_t>(Profile);
  EXPECT_LT(Index, VMProfiles.size());
  SBFVMConfig Config;
  if (Index < VMProfiles.size())
    Config.OptimizeRodata = VMProfiles[Index].OptimizeRodata;
  return Config;
}

ExecutionEnvironment makeEnvironment(InputProfile Input,
                                     SyscallProfile Syscalls) {
  ExecutionEnvironment Environment;
  const size_t InputIndex = static_cast<size_t>(Input);
  const size_t SyscallIndex = static_cast<size_t>(Syscalls);
  if (Input == InputProfile::ByteOne && InputIndex < InputProfiles.size())
    Environment.Memory.push_back({kInputStart,
                                  {InputProfiles[InputIndex].FirstByte},
                                  false,
                                  "oracle.input"});
  if (Syscalls == SyscallProfile::Log) {
    EXPECT_LT(SyscallIndex, SyscallProfiles.size());
    if (SyscallIndex >= SyscallProfiles.size())
      return Environment;
    const uint32_t LogHash = hashSymbolName(SyscallProfiles[SyscallIndex].Name);
    Environment.Syscall =
        [LogHash](uint32_t Hash,
                  const SyscallArguments &) -> std::optional<uint64_t> {
      if (Hash != LogHash)
        return std::nullopt;
      return 0;
    };
  }
  return Environment;
}

OracleOutcome runNeverDELF(const FixtureExpectation &Fixture,
                           const std::filesystem::path &Path) {
  auto Image = loadBinary(Path);
  if (!Image) {
    llvm::consumeError(Image.takeError());
    return {OracleOutcomeKind::Rejected};
  }

  AnalyzeOptions Analyze;
  Analyze.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
      kMinimumOracleVersion, kMaximumOracleVersion, makeVMConfig(Fixture.VM)};
  auto Program = analyze(*Image, Analyze);
  if (!Program) {
    llvm::consumeError(Program.takeError());
    return {OracleOutcomeKind::Rejected};
  }
  if (Fixture.Execution == ExecutionExpectation::NotRun)
    return {OracleOutcomeKind::Accepted};

  InterpreterOptions Options;
  Options.MaxSteps = std::max<uint64_t>(Fixture.Budget, 1'024);
  auto Result = executeRaw(
      *Program, makeEnvironment(Fixture.Input, Fixture.Syscalls), Options);
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return {OracleOutcomeKind::OtherFault};
  }
  if (Result->Status == ExecutionStatus::Returned)
    return {OracleOutcomeKind::Returned, Result->ReturnValue, Result->Steps};
  if (Result->Status == ExecutionStatus::Faulted &&
      Result->Fault == FaultCode::CallDepth)
    return {OracleOutcomeKind::CallDepthFault, 0, Result->Steps};
  return {OracleOutcomeKind::OtherFault, 0, Result->Steps};
}

OracleOutcome expectedELFOutcome(const FixtureExpectation &Fixture) {
  if (Fixture.Load == LoadExpectation::Reject)
    return {OracleOutcomeKind::Rejected};
  switch (Fixture.Execution) {
  case ExecutionExpectation::NotRun:
    return {OracleOutcomeKind::Accepted};
  case ExecutionExpectation::Return:
    return {OracleOutcomeKind::Returned, Fixture.Result};
  case ExecutionExpectation::FaultCallDepth:
    return {OracleOutcomeKind::CallDepthFault};
  }
  llvm_unreachable("unknown upstream execution expectation");
}

std::string inputProfileName(InputProfile Profile) {
  return Profile == InputProfile::ByteOne ? kOracleByteOneProfile.str()
                                          : kOracleNoneProfile.str();
}

std::string syscallProfileName(SyscallProfile Profile) {
  return Profile == SyscallProfile::Log ? kOracleLogProfile.str()
                                        : kOracleNoneProfile.str();
}

llvm::Expected<OracleOutcome> runOfficialELF(llvm::StringRef Oracle,
                                             const FixtureExpectation &Fixture,
                                             const std::filesystem::path &Path,
                                             OracleProcessResult &Process) {
  const bool OptimizeRodata =
      VMProfiles[static_cast<size_t>(Fixture.VM)].OptimizeRodata;
  const std::string PathString = Path.string();
  const std::string Input = inputProfileName(Fixture.Input);
  const std::string Syscalls = syscallProfileName(Fixture.Syscalls);
  const std::string Budget = llvm::utostr(Fixture.Budget);
  llvm::SmallVector<llvm::StringRef, 14> Arguments{
      kOracleELFCommand,
      kOracleELFArgument,
      PathString,
      kOracleOptimizeRodataArgument,
      OptimizeRodata ? kOracleTrueValue : kOracleFalseValue,
      kOracleInputArgument,
      Input,
      kOracleSyscallsArgument,
      Syscalls,
      kOracleBudgetArgument,
      Budget,
  };
  Process = runOracle(Oracle, Arguments);
  if (Process.ExitCode != 0)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "official oracle exited with %d: %s%s",
        Process.ExitCode, Process.ExecutionError.c_str(),
        Process.StandardError.c_str());
  return parseOutcome(Process.StandardOutput);
}

struct OracleConfiguration {
  std::string Executable;
  std::filesystem::path Corpus;
};

struct RawFunction {
  uint32_t Key;
  size_t TargetSlot;
  std::string Name;
};

struct RawCase {
  const char *Name;
  Version TheVersion;
  /// Loader-input instructions consumed by NeverD. Legacy ELF CALL
  /// relocations are still relative at this boundary.
  std::vector<test::EncodedInstruction> Instructions;
  /// Optional post-relocation text consumed by upstream's from_text_bytes().
  /// Modern versions use the same bytes on both sides.
  std::vector<test::EncodedInstruction> OfficialInstructions;
  std::vector<RawFunction> Functions;
  SyscallProfile Syscalls;
  uint64_t Budget;
  OracleOutcomeKind ExpectedKind;
  uint64_t ExpectedResult;
  uint64_t ExpectedInstructionCount;
};

enum class AcceptanceExpectation : uint8_t { Accept, Reject };

llvm::StringRef acceptanceName(AcceptanceExpectation Expectation) {
  return Expectation == AcceptanceExpectation::Accept ? kOracleAccepted
                                                      : kOracleRejected;
}

llvm::StringRef oracleVersionName(Version TheVersion) {
  assert(isConcreteVersion(TheVersion) &&
         "official oracle version must be concrete");
  return versionName(TheVersion);
}

enum class VerifierCaseBuilder : uint8_t {
#define SBF_OFFICIAL_VERIFIER_CASE(ID, NAME, BUILDER, VERSION_MASK,            \
                                   EXPECTATION)                                \
  BUILDER,
#include "SBFOfficialVerifierCases.def"
};

struct VerifierCaseDefinition {
  llvm::StringLiteral Name;
  VerifierCaseBuilder Builder;
  VersionMask Versions;
  AcceptanceExpectation Expected;
};

constexpr std::array VerifierCaseDefinitions = {
#define SBF_OFFICIAL_VERIFIER_CASE(ID, NAME, BUILDER, VERSION_MASK,            \
                                   EXPECTATION)                                \
  VerifierCaseDefinition{NAME, VerifierCaseBuilder::BUILDER,                   \
                         VersionMask::VERSION_MASK,                            \
                         AcceptanceExpectation::EXPECTATION},
#include "SBFOfficialVerifierCases.def"
};

constexpr size_t kVerifierDomainCaseCount = [] {
  size_t Count = 0;
  for (Version TheVersion : kOracleVersions)
    for (const VerifierCaseDefinition &Definition : VerifierCaseDefinitions)
      Count += versionInMask(TheVersion, Definition.Versions);
  return Count;
}();

struct UpstreamOpcodeDefinition {
  Opcode ID;
  uint8_t Encoding;
  OperandForm Form;
  uint8_t Width;
  VersionMask Versions;
};

constexpr std::array UpstreamOpcodeDefinitions = {
#define SBF_UPSTREAM_OPCODE(ID, ENCODING, MNEMONIC, FORM, WIDTH, VERSION_MASK) \
  UpstreamOpcodeDefinition{Opcode::ID, ENCODING, OperandForm::FORM, WIDTH,     \
                           VersionMask::VERSION_MASK},
#include "SBFUpstreamOpcodes.def"
};

struct VerificationCase {
  std::string Name;
  Version TheVersion;
  std::vector<uint8_t> Text;
  AcceptanceExpectation Expected;
};

enum class ELFMutationBuilder : uint8_t {
#define SBF_OFFICIAL_ELF_MUTATION(ID, NAME, BUILDER, EXPECTATION) BUILDER,
#include "SBFOfficialELFMutations.def"
};

struct ELFMutationDefinition {
  llvm::StringLiteral Name;
  ELFMutationBuilder Builder;
  AcceptanceExpectation Expected;
};

constexpr std::array ELFMutationDefinitions = {
#define SBF_OFFICIAL_ELF_MUTATION(ID, NAME, BUILDER, EXPECTATION)              \
  ELFMutationDefinition{NAME, ELFMutationBuilder::BUILDER,                     \
                        AcceptanceExpectation::EXPECTATION},
#include "SBFOfficialELFMutations.def"
};

struct ELFMutationCase {
  std::string Name;
  std::vector<uint8_t> Bytes;
  AcceptanceExpectation Expected;
};

enum class LegacyELFCaseBuilder : uint8_t {
#define SBF_OFFICIAL_LEGACY_ELF_CASE(ID, NAME, BUILDER, EXPECTATION) BUILDER,
#include "SBFOfficialLegacyELFCases.def"
};

struct LegacyELFCaseDefinition {
  llvm::StringLiteral Name;
  LegacyELFCaseBuilder Builder;
  AcceptanceExpectation Expected;
};

constexpr std::array LegacyELFCaseDefinitions = {
#define SBF_OFFICIAL_LEGACY_ELF_CASE(ID, NAME, BUILDER, EXPECTATION)           \
  LegacyELFCaseDefinition{NAME, LegacyELFCaseBuilder::BUILDER,                 \
                          AcceptanceExpectation::EXPECTATION},
#include "SBFOfficialLegacyELFCases.def"
};

using OracleELFT = llvm::object::ELF64LE;
using OracleELFHeader = OracleELFT::Ehdr;
using OracleProgramHeader = OracleELFT::Phdr;

constexpr uint8_t kInvalidELFMagicByte = 0;
constexpr uint8_t kNonZeroELFIdentificationByte = 1;
constexpr size_t kRodataProgramHeaderIndex = 0;
constexpr size_t kTextProgramHeaderIndex = 1;
constexpr uint16_t kBaselineProgramHeaderCount = 2;
constexpr uint16_t kOverlappingProgramHeaderCount = 3;
constexpr size_t kSingleByteTruncation = 1;
constexpr uint64_t kMinimalSegmentAlignment = 1;
constexpr size_t kBaselineTextInstructionCount = 2;

template <typename Mutator>
void mutateELFHeader(std::vector<uint8_t> &Bytes, Mutator Mutate) {
  ASSERT_GE(Bytes.size(), sizeof(OracleELFHeader));
  OracleELFHeader Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  Mutate(Header);
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
}

template <typename Mutator>
void mutateProgramHeader(std::vector<uint8_t> &Bytes, size_t Index,
                         Mutator Mutate) {
  ASSERT_GE(Bytes.size(), sizeof(OracleELFHeader));
  OracleELFHeader Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  const uint64_t Offset = Header.e_phoff + Index * sizeof(OracleProgramHeader);
  ASSERT_LE(Offset, Bytes.size());
  ASSERT_LE(sizeof(OracleProgramHeader), Bytes.size() - Offset);
  OracleProgramHeader ProgramHeader;
  std::memcpy(&ProgramHeader, Bytes.data() + Offset, sizeof(ProgramHeader));
  Mutate(ProgramHeader);
  std::memcpy(Bytes.data() + Offset, &ProgramHeader, sizeof(ProgramHeader));
}

std::optional<OracleProgramHeader>
readProgramHeader(llvm::ArrayRef<uint8_t> Bytes, size_t Index) {
  if (Bytes.size() < sizeof(OracleELFHeader)) {
    ADD_FAILURE() << "ELF mutation baseline is missing its file header";
    return std::nullopt;
  }
  OracleELFHeader Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  const uint64_t Offset = Header.e_phoff + Index * sizeof(OracleProgramHeader);
  if (Offset > Bytes.size() ||
      sizeof(OracleProgramHeader) > Bytes.size() - Offset) {
    ADD_FAILURE() << "ELF mutation baseline is missing program header "
                  << Index;
    return std::nullopt;
  }
  OracleProgramHeader ProgramHeader;
  std::memcpy(&ProgramHeader, Bytes.data() + Offset, sizeof(ProgramHeader));
  return ProgramHeader;
}

std::vector<uint8_t> strictELFBaseline();

void applyELFMutation(std::vector<uint8_t> &Bytes, ELFMutationBuilder Builder) {
  switch (Builder) {
  case ELFMutationBuilder::Baseline:
    return;
  case ELFMutationBuilder::InvalidMagic:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_MAG0] = kInvalidELFMagicByte;
    });
    return;
  case ELFMutationBuilder::InvalidClass:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS32;
    });
    return;
  case ELFMutationBuilder::InvalidData:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2MSB;
    });
    return;
  case ELFMutationBuilder::InvalidIdentVersion:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_VERSION] = llvm::ELF::EV_NONE;
    });
    return;
  case ELFMutationBuilder::InvalidOSABI:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_OSABI] = llvm::ELF::ELFOSABI_GNU;
    });
    return;
  case ELFMutationBuilder::InvalidABIVersion:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_ABIVERSION] = kNonZeroELFIdentificationByte;
    });
    return;
  case ELFMutationBuilder::InvalidIdentPadding:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ident[llvm::ELF::EI_PAD] = kNonZeroELFIdentificationByte;
    });
    return;
  case ELFMutationBuilder::ExecutableType:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_type = llvm::ELF::ET_EXEC;
    });
    return;
  case ELFMutationBuilder::InvalidMachine:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_machine = kELFMachineSBPF;
    });
    return;
  case ELFMutationBuilder::InvalidHeaderVersion:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_version = llvm::ELF::EV_NONE;
    });
    return;
  case ELFMutationBuilder::ReservedSBPFVersion:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_flags = static_cast<uint32_t>(Version::Reserved);
    });
    return;
  case ELFMutationBuilder::InvalidELFHeaderSize:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_ehsize = sizeof(OracleELFHeader) - kSingleByteTruncation;
    });
    return;
  case ELFMutationBuilder::InvalidProgramHeaderEntrySize:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_phentsize = sizeof(OracleProgramHeader) - kSingleByteTruncation;
    });
    return;
  case ELFMutationBuilder::NoProgramHeaders:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) { Header.e_phnum = 0; });
    return;
  case ELFMutationBuilder::ProgramHeaderOffsetOverlapsFileHeader:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) { Header.e_phoff = 0; });
    return;
  case ELFMutationBuilder::ProgramHeaderOffsetOverflow:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_phoff = std::numeric_limits<decltype(Header.e_phoff)>::max();
    });
    return;
  case ELFMutationBuilder::ProgramHeaderCountMaximum:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_phnum = std::numeric_limits<decltype(Header.e_phnum)>::max();
    });
    return;
  case ELFMutationBuilder::TruncatedProgramHeaderTable:
    Bytes.resize(sizeof(OracleELFHeader) +
                 kBaselineProgramHeaderCount * sizeof(OracleProgramHeader) -
                 kSingleByteTruncation);
    return;
  case ELFMutationBuilder::ProgramHeaderTableOverlapsSegments:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_phnum = kOverlappingProgramHeaderCount;
    });
    Bytes.resize(sizeof(OracleELFHeader) + kOverlappingProgramHeaderCount *
                                               sizeof(OracleProgramHeader),
                 0);
    return;
  case ELFMutationBuilder::InvalidRodataSegmentType:
    mutateProgramHeader(Bytes, kRodataProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_type = llvm::ELF::PT_NULL;
                        });
    return;
  case ELFMutationBuilder::InvalidTextSegmentType:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_type = llvm::ELF::PT_NULL;
                        });
    return;
  case ELFMutationBuilder::InvalidRodataSegmentFlags:
    mutateProgramHeader(
        Bytes, kRodataProgramHeaderIndex,
        [](OracleProgramHeader &Header) { Header.p_flags = llvm::ELF::PF_X; });
    return;
  case ELFMutationBuilder::InvalidTextSegmentFlags:
    mutateProgramHeader(
        Bytes, kTextProgramHeaderIndex,
        [](OracleProgramHeader &Header) { Header.p_flags = llvm::ELF::PF_R; });
    return;
  case ELFMutationBuilder::RodataOffsetUnaligned:
    mutateProgramHeader(Bytes, kRodataProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_offset += kSingleByteTruncation;
                        });
    return;
  case ELFMutationBuilder::TextOffsetUnaligned:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_offset += kSingleByteTruncation;
                        });
    return;
  case ELFMutationBuilder::TextOffsetAtEndOfFile:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [&Bytes](OracleProgramHeader &Header) {
                          Header.p_offset = Bytes.size();
                        });
    return;
  case ELFMutationBuilder::TextFileRangePastEnd:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_filesz += kInstructionSize;
                          Header.p_memsz += kInstructionSize;
                        });
    return;
  case ELFMutationBuilder::TextFileSizeUnaligned:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_filesz -= kSingleByteTruncation;
                          Header.p_memsz = Header.p_filesz;
                        });
    return;
  case ELFMutationBuilder::TextFileMemorySizeMismatch:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_memsz += kInstructionSize;
                        });
    return;
  case ELFMutationBuilder::InvalidTextVirtualAddress:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_vaddr += kInstructionSize;
                        });
    return;
  case ELFMutationBuilder::InvalidTextPhysicalAddress:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_paddr += kInstructionSize;
                        });
    return;
  case ELFMutationBuilder::RodataTextFileOverlap: {
    const std::optional<OracleProgramHeader> Rodata =
        readProgramHeader(Bytes, kRodataProgramHeaderIndex);
    ASSERT_TRUE(Rodata.has_value());
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [&Rodata](OracleProgramHeader &Header) {
                          Header.p_offset = Rodata->p_offset;
                        });
    return;
  }
  case ELFMutationBuilder::RodataTextVMOverlap: {
    const std::optional<OracleProgramHeader> Rodata =
        readProgramHeader(Bytes, kRodataProgramHeaderIndex);
    ASSERT_TRUE(Rodata.has_value());
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [&Rodata](OracleProgramHeader &Header) {
                          Header.p_vaddr = Rodata->p_vaddr;
                          Header.p_paddr = Rodata->p_paddr;
                        });
    return;
  }
  case ELFMutationBuilder::TextAlignmentValueIgnored:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_align = kMinimalSegmentAlignment;
                        });
    return;
  case ELFMutationBuilder::TruncatedTextBytes:
    ASSERT_GE(Bytes.size(), kSingleByteTruncation);
    Bytes.resize(Bytes.size() - kSingleByteTruncation);
    return;
  case ELFMutationBuilder::EmptyTextSegment:
    mutateProgramHeader(Bytes, kTextProgramHeaderIndex,
                        [](OracleProgramHeader &Header) {
                          Header.p_filesz = 0;
                          Header.p_memsz = 0;
                        });
    return;
  case ELFMutationBuilder::EntryAtSecondInstruction:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_entry = kBytecodeStart + kInstructionSize;
    });
    return;
  case ELFMutationBuilder::EntryUnaligned:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_entry = kBytecodeStart + kSingleByteTruncation;
    });
    return;
  case ELFMutationBuilder::EntryBeforeText:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_entry = kBytecodeStart - kInstructionSize;
    });
    return;
  case ELFMutationBuilder::EntryAtTextEnd:
    mutateELFHeader(Bytes, [](OracleELFHeader &Header) {
      Header.e_entry =
          kBytecodeStart + kBaselineTextInstructionCount * kInstructionSize;
    });
    return;
  }
  llvm_unreachable("unknown official ELF mutation builder");
}

std::vector<ELFMutationCase> elfMutationCases() {
  const std::vector<uint8_t> Baseline = strictELFBaseline();
  EXPECT_FALSE(Baseline.empty());
  std::vector<ELFMutationCase> Cases;
  Cases.reserve(ELFMutationDefinitions.size());
  for (const ELFMutationDefinition &Definition : ELFMutationDefinitions) {
    std::vector<uint8_t> Bytes = Baseline;
    applyELFMutation(Bytes, Definition.Builder);
    Cases.push_back(
        {Definition.Name.str(), std::move(Bytes), Definition.Expected});
  }
  return Cases;
}

std::vector<uint8_t> legacyELFCaseBytes(LegacyELFCaseBuilder Builder) {
  constexpr llvm::StringLiteral UnknownSectionName(".mystery");
  test::LegacyELFOptions Options;
  switch (Builder) {
  case LegacyELFCaseBuilder::Baseline:
    return test::buildLegacyELF(Options);
  case LegacyELFCaseBuilder::TextWithoutAlloc:
    Options.TextIsAllocatable = false;
    return test::buildLegacyELF(Options);
  case LegacyELFCaseBuilder::RodataWithoutAlloc:
    Options.AddReadOnlyData = true;
    Options.ReadOnlyDataIsAllocatable = false;
    Options.DataSectionName = kRodataSectionName.str();
    return test::buildLegacyELF(Options);
  case LegacyELFCaseBuilder::UnknownAllocProgbits:
    Options.AddReadOnlyData = true;
    Options.DataSectionName = UnknownSectionName.str();
    return test::buildLegacyELF(Options);
  case LegacyELFCaseBuilder::UnknownAllocNobits:
    Options.AddReadOnlyData = true;
    Options.DataSectionName = UnknownSectionName.str();
    Options.DataIsNoBits = true;
    return test::buildLegacyELF(Options);
  case LegacyELFCaseBuilder::PrebasedRodata:
    Options.AddReadOnlyData = true;
    Options.DataSectionName = kRodataSectionName.str();
    Options.DataVirtualAddress = kBytecodeStart + kInstructionSize;
    return test::buildLegacyELF(Options);
  case LegacyELFCaseBuilder::RelocationBeforeEntryAlignment: {
    constexpr uint64_t EntryMisalignment = 1;
    test::LegacyDynamicELFOptions DynamicOptions;
    DynamicOptions.MisalignDynamicRel = true;
    DynamicOptions.Relocations.push_back(
        {0, Relocation::Call32, test::kLegacyFixtureDynamicSymbolIndex});
    std::vector<uint8_t> Bytes = test::buildLegacyDynamicELF(DynamicOptions);
    OracleELFHeader Header;
    EXPECT_GE(Bytes.size(), sizeof(Header));
    if (Bytes.size() < sizeof(Header))
      return {};
    std::memcpy(&Header, Bytes.data(), sizeof(Header));
    Header.e_entry = static_cast<uint64_t>(Header.e_entry) + EntryMisalignment;
    std::memcpy(Bytes.data(), &Header, sizeof(Header));
    return Bytes;
  }
  }
  llvm_unreachable("unknown official legacy ELF case builder");
}

std::vector<ELFMutationCase> legacyELFCases() {
  std::vector<ELFMutationCase> Cases;
  Cases.reserve(LegacyELFCaseDefinitions.size());
  for (const LegacyELFCaseDefinition &Definition : LegacyELFCaseDefinitions)
    Cases.push_back({Definition.Name.str(),
                     legacyELFCaseBytes(Definition.Builder),
                     Definition.Expected});
  return Cases;
}

constexpr uint8_t kMaximumWritableRegister = kFramePointerRegister - 1;
constexpr uint8_t kInvalidRegister = kRegisterCount;
constexpr int16_t kBranchInRangeOffset = 0;
constexpr int16_t kBranchForwardOutOfRangeOffset = 1;
constexpr int16_t kBranchBackwardOutOfRangeOffset = -2;
constexpr int16_t kBranchToContinuationOffset = 1;
constexpr int32_t kValidImmediate = 1;
constexpr int32_t kInvalidImmediate = 0;
constexpr int32_t kInvalidEndianByteWidth = kBitsPerByte;
constexpr int32_t kInvalidEndianBelowDoubleWord = kDoubleWordBitWidth - 1;
constexpr int32_t kInvalidEndianAboveDoubleWord = kDoubleWordBitWidth + 1;

const UpstreamOpcodeDefinition *upstreamOpcode(Opcode ID) {
  const auto It = std::find_if(
      UpstreamOpcodeDefinitions.begin(), UpstreamOpcodeDefinitions.end(),
      [ID](const UpstreamOpcodeDefinition &Definition) {
        return Definition.ID == ID;
      });
  return It == UpstreamOpcodeDefinitions.end() ? nullptr : &*It;
}

test::EncodedInstruction encodeUpstream(Opcode ID, uint8_t Dst = 0,
                                        uint8_t Src = 0, int16_t Offset = 0,
                                        int32_t Immediate = 0) {
  test::EncodedInstruction Instruction{};
  const UpstreamOpcodeDefinition *Definition = upstreamOpcode(ID);
  EXPECT_NE(Definition, nullptr);
  if (!Definition)
    return Instruction;
  Instruction[kOpcodeOffset] = Definition->Encoding;
  Instruction[kRegisterByteOffset] =
      static_cast<uint8_t>((Src << kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Instruction.data() + kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Instruction.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Instruction;
}

std::vector<uint8_t>
instructionBytes(llvm::ArrayRef<test::EncodedInstruction> Instructions) {
  std::vector<uint8_t> Text;
  Text.reserve(Instructions.size() * kInstructionSize);
  for (const test::EncodedInstruction &Instruction : Instructions)
    Text.insert(Text.end(), Instruction.begin(), Instruction.end());
  return Text;
}

enum class ExecutionFaultClass : uint8_t {
#define SBF_OFFICIAL_EXECUTION_FAULT(ID, RUST_ID, SPELLING) ID,
#include "SBFOfficialExecutionFaults.def"
};

enum class ExecutionOutcomeKind : uint8_t { Rejected, Returned, Faulted };
enum class ExecutionSyscallProfile : uint8_t { None, Probe, CollisionProbe };

template <typename... VersionTs>
constexpr VersionMask executionVersionMask(VersionTs... Versions) {
  const uint64_t Mask =
      (uint64_t{0} | ... | (uint64_t{1} << static_cast<uint8_t>(Versions)));
  return static_cast<VersionMask>(Mask);
}

enum class ExecutionCaseBuilder : uint8_t {
#define SBF_OFFICIAL_EXECUTION_CASE(ID, NAME, BUILDER, VERSION_MASK, BUDGET,   \
                                    OUTCOME, FAULT, TARGET_HITS)               \
  BUILDER,
#include "SBFOfficialExecutionCases.def"
};

struct ExecutionCaseDefinition {
  llvm::StringLiteral Name;
  ExecutionCaseBuilder Builder;
  VersionMask Versions;
  uint64_t Budget;
  ExecutionOutcomeKind Outcome;
  ExecutionFaultClass Fault;
  size_t TargetHits;
};

constexpr std::array ExecutionCaseDefinitions = {
#define SBF_OFFICIAL_EXECUTION_CASE(ID, NAME, BUILDER, VERSION_MASK, BUDGET,   \
                                    OUTCOME, FAULT, TARGET_HITS)               \
  ExecutionCaseDefinition{                                                     \
      NAME,       ExecutionCaseBuilder::BUILDER, VERSION_MASK,                 \
      BUDGET,     ExecutionOutcomeKind::OUTCOME, ExecutionFaultClass::FAULT,   \
      TARGET_HITS},
#include "SBFOfficialExecutionCases.def"
};

constexpr std::array ExecutionFaultSpellings = {
#define SBF_OFFICIAL_EXECUTION_FAULT(ID, RUST_ID, SPELLING)                    \
  llvm::StringLiteral(SPELLING),
#include "SBFOfficialExecutionFaults.def"
};

llvm::StringRef executionFaultName(ExecutionFaultClass Fault) {
  const size_t Index = static_cast<size_t>(Fault);
  assert(Index < ExecutionFaultSpellings.size() &&
         "execution fault class must be tabulated");
  return ExecutionFaultSpellings[Index];
}

std::optional<ExecutionFaultClass> parseExecutionFault(llvm::StringRef Name) {
  for (size_t Index = 0; Index < ExecutionFaultSpellings.size(); ++Index)
    if (ExecutionFaultSpellings[Index] == Name)
      return static_cast<ExecutionFaultClass>(Index);
  return std::nullopt;
}

struct ExecutionCase {
  std::string Name;
  Version TheVersion = Version::Reserved;
  std::vector<uint8_t> Text;
  /// Optional pre-relocation loader input. The official from_text_bytes()
  /// boundary consumes Text; legacy NeverD loading consumes relative CALLs.
  std::vector<uint8_t> LoaderText;
  std::vector<RawFunction> Functions;
  std::vector<uint8_t> Input;
  ExecutionSyscallProfile Syscalls = ExecutionSyscallProfile::None;
  uint64_t Budget = 0;
  size_t TargetSlot = 0;
  size_t ExpectedTargetHits = 1;
  std::optional<ExecutionOutcomeKind> ExpectedOutcome;
  std::optional<ExecutionFaultClass> ExpectedFault;
  std::optional<uint64_t> ExpectedResult;
};

struct ExecutionObservation {
  ExecutionOutcomeKind Outcome = ExecutionOutcomeKind::Faulted;
  ExecutionFaultClass Fault = ExecutionFaultClass::InvalidInstruction;
  uint64_t Result = 0;
  uint64_t InstructionCount = 0;
  size_t TargetHits = 0;
  std::vector<uint8_t> Input;
  uint64_t SyscallCount = 0;
  uint64_t SyscallDigest = 0;

  friend bool operator==(const ExecutionObservation &,
                         const ExecutionObservation &) = default;
};

#define SBF_OFFICIAL_EXECUTION_CONSTANT(ID, RUST_ID, RUST_TYPE, CXX_TYPE,      \
                                        VALUE)                                 \
  [[maybe_unused]] constexpr CXX_TYPE k##ID = VALUE;
#include "SBFOfficialExecutionConstants.def"
constexpr std::array<uint8_t, 16> kExecutionInputPattern = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

constexpr size_t kComputedActiveOpcodeVersionPairCount = [] {
  size_t Count = 0;
  for (Version TheVersion : kOracleVersions)
    for (const UpstreamOpcodeDefinition &Definition : UpstreamOpcodeDefinitions)
      Count += versionInMask(TheVersion, Definition.Versions);
  return Count;
}();

static_assert(kComputedActiveOpcodeVersionPairCount ==
                  kPinnedActiveOpcodeVersionPairCount,
              "the pinned execution matrix cardinality changed");

constexpr size_t kComputedBoundaryExecutionCaseCount = [] {
  size_t Count = 0;
  for (Version TheVersion : kOracleVersions)
    for (const ExecutionCaseDefinition &Definition : ExecutionCaseDefinitions)
      Count += versionInMask(TheVersion, Definition.Versions);
  return Count;
}();

static_assert(kComputedBoundaryExecutionCaseCount ==
                  kPinnedBoundaryExecutionCaseCount,
              "the pinned boundary execution matrix cardinality changed");
static_assert(std::size(Fixtures) == kPinnedManifestELFCount,
              "the pinned ELF manifest cardinality changed");
static_assert(kExhaustiveEncodingCaseCount + kVerifierDomainCaseCount ==
                  kPinnedVerificationCaseCount,
              "the pinned verification matrix cardinality changed");
static_assert(ELFMutationDefinitions.size() == kPinnedStrictELFMutationCount,
              "the pinned strict ELF mutation cardinality changed");
static_assert(kPinnedActiveOpcodeVersionPairCount +
                      kPinnedBoundaryExecutionCaseCount ==
                  kPinnedExecutionCaseCount,
              "the pinned execution matrix total changed");

llvm::Expected<std::vector<uint8_t>> decodeOracleHex(llvm::StringRef Value) {
  if (Value == kOracleEmptyField)
    return std::vector<uint8_t>{};
  if ((Value.size() & 1u) != 0)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "odd-length oracle hex field '%s'",
                                   Value.str().c_str());
  std::vector<uint8_t> Bytes;
  Bytes.reserve(Value.size() / 2);
  for (size_t Offset = 0; Offset < Value.size(); Offset += 2) {
    uint8_t Byte = 0;
    if (!llvm::tryGetHexFromNibbles(Value[Offset], Value[Offset + 1], Byte))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "invalid oracle hex field '%s'",
                                     Value.str().c_str());
    Bytes.push_back(Byte);
  }
  return Bytes;
}

llvm::Expected<ExecutionOutcomeKind>
parseExecutionOutcome(llvm::StringRef Name) {
  if (Name == kOracleRejected)
    return ExecutionOutcomeKind::Rejected;
  if (Name == kOracleReturned)
    return ExecutionOutcomeKind::Returned;
  if (Name == kOracleFaulted)
    return ExecutionOutcomeKind::Faulted;
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "unknown execution outcome '%s'",
                                 Name.str().c_str());
}

llvm::Expected<std::vector<ExecutionObservation>>
parseExecutionBatchOutput(llvm::StringRef Output, size_t ExpectedCount) {
  std::vector<ExecutionObservation> Observations;
  bool SawSummary = false;
  while (!Output.empty()) {
    auto [Line, Rest] = Output.split('\n');
    Line = Line.trim();
    Output = Rest;
    if (Line.empty())
      continue;
    if (!Line.starts_with(kOracleProtocol))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "oracle emitted non-protocol execution output '%s'",
          Line.str().c_str());

    llvm::SmallVector<llvm::StringRef, 12> Fields;
    Line.split(Fields, ' ', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    if (Fields.size() == 3 && Fields[0] == kOracleProtocol &&
        Fields[1] == kOracleExecutionSummaryRecord) {
      if (SawSummary)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "oracle emitted two execution summaries");
      auto Count = parseUnsigned(Fields[2]);
      if (!Count)
        return Count.takeError();
      if (*Count != ExpectedCount)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "oracle execution summary reported %llu records, expected %zu",
            static_cast<unsigned long long>(*Count), ExpectedCount);
      SawSummary = true;
      continue;
    }
    if (SawSummary)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "oracle emitted a protocol record after the execution summary");
    if (Fields.size() != 11 || Fields[0] != kOracleProtocol ||
        Fields[1] != kOracleExecutionRecord)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "malformed execution record '%s'",
                                     Line.str().c_str());

    auto Index = parseUnsigned(Fields[2]);
    if (!Index)
      return Index.takeError();
    if (*Index != Observations.size())
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "oracle execution record index %llu is not the expected %zu",
          static_cast<unsigned long long>(*Index), Observations.size());
    auto Outcome = parseExecutionOutcome(Fields[3]);
    if (!Outcome)
      return Outcome.takeError();
    const std::optional<ExecutionFaultClass> Fault =
        parseExecutionFault(Fields[4]);
    if (!Fault)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unknown execution fault '%s'",
                                     Fields[4].str().c_str());
    auto Result = parseUnsigned(Fields[5]);
    if (!Result)
      return Result.takeError();
    auto Count = parseUnsigned(Fields[6]);
    if (!Count)
      return Count.takeError();
    auto TargetHits = parseUnsigned(Fields[7]);
    if (!TargetHits)
      return TargetHits.takeError();
    auto Input = decodeOracleHex(Fields[8]);
    if (!Input)
      return Input.takeError();
    auto SyscallCount = parseUnsigned(Fields[9]);
    if (!SyscallCount)
      return SyscallCount.takeError();
    auto SyscallDigest = parseUnsigned(Fields[10]);
    if (!SyscallDigest)
      return SyscallDigest.takeError();
    if ((*Outcome == ExecutionOutcomeKind::Returned &&
         *Fault != ExecutionFaultClass::None) ||
        (*Outcome == ExecutionOutcomeKind::Faulted &&
         *Fault == ExecutionFaultClass::None) ||
        (*Outcome == ExecutionOutcomeKind::Rejected &&
         *Fault != ExecutionFaultClass::None))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "inconsistent execution outcome/fault in record %zu",
          Observations.size());

    Observations.push_back({*Outcome, *Fault, *Result, *Count,
                            static_cast<size_t>(*TargetHits), std::move(*Input),
                            *SyscallCount, *SyscallDigest});
  }
  if (!SawSummary)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "oracle emitted no execution summary");
  if (Observations.size() != ExpectedCount)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "oracle emitted %zu execution records, expected %zu",
        Observations.size(), ExpectedCount);
  return Observations;
}

std::string executionSyscallProfileName(ExecutionSyscallProfile Profile) {
  switch (Profile) {
  case ExecutionSyscallProfile::None:
    return kOracleNoneProfile.str();
  case ExecutionSyscallProfile::Probe:
    return kOracleProbeProfile.str();
  case ExecutionSyscallProfile::CollisionProbe:
    return kOracleCollisionProbeProfile.str();
  }
  llvm_unreachable("unknown execution syscall profile");
}

std::string executionFunctionsField(const ExecutionCase &Case) {
  if (Case.Functions.empty())
    return kOracleEmptyField.str();
  std::string Field;
  for (const RawFunction &Function : Case.Functions) {
    if (!Field.empty())
      Field.push_back(',');
    Field += llvm::utostr(Function.Key);
    Field.push_back(':');
    Field += llvm::utostr(Function.TargetSlot);
  }
  return Field;
}

std::string executionBytesField(llvm::ArrayRef<uint8_t> Bytes) {
  return Bytes.empty() ? kOracleEmptyField.str() : llvm::toHex(Bytes, true);
}

llvm::Expected<std::vector<ExecutionObservation>>
runOfficialExecutionBatch(llvm::StringRef Oracle,
                          llvm::ArrayRef<ExecutionCase> Cases,
                          OracleProcessResult &Process) {
  test::TemporaryFile BatchFile("execution-batch");
  if (BatchFile.error())
    return llvm::createStringError(BatchFile.error(),
                                   "cannot create execution batch input");
  {
    std::ofstream Output(BatchFile.str().str());
    if (!Output)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "cannot open execution batch input");
    Output << kOracleExecutionBatchProtocol.str() << '\n';
    for (size_t Index = 0; Index < Cases.size(); ++Index) {
      const ExecutionCase &Case = Cases[Index];
      Output << Index << ' ' << versionName(Case.TheVersion).str() << ' '
             << Case.Budget << ' ' << executionSyscallProfileName(Case.Syscalls)
             << ' ' << Case.TargetSlot << ' ' << executionFunctionsField(Case)
             << ' ' << executionBytesField(Case.Input) << ' '
             << executionBytesField(Case.Text) << '\n';
    }
  }

  const std::string Path = BatchFile.str().str();
  const llvm::SmallVector<llvm::StringRef, 3> Arguments{
      kOracleExecuteBatchCommand, kOracleBatchInputArgument, Path};
  Process = runOracle(Oracle, Arguments);
  if (Process.ExitCode != 0)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "official oracle exited with %d: %s%s",
        Process.ExitCode, Process.ExecutionError.c_str(),
        Process.StandardError.c_str());
  return parseExecutionBatchOutput(Process.StandardOutput, Cases.size());
}

Opcode memoryLoadOpcode(Version TheVersion, uint8_t Width) {
  if (TheVersion == Version::V2) {
    switch (Width) {
    case 8:
      return Opcode::LD_1B_REG;
    case 16:
      return Opcode::LD_2B_REG;
    case 32:
      return Opcode::LD_4B_REG;
    case 64:
      return Opcode::LD_8B_REG;
    }
  } else {
    switch (Width) {
    case 8:
      return Opcode::LD_B_REG;
    case 16:
      return Opcode::LD_H_REG;
    case 32:
      return Opcode::LD_W_REG;
    case 64:
      return Opcode::LD_DW_REG;
    }
  }
  llvm_unreachable("unsupported memory width in execution probe");
}

std::pair<int32_t, int32_t> branchOperands(Operation Operation) {
  switch (Operation) {
  case Operation::ULt:
  case Operation::SLt:
    return {3, 7};
  case Operation::Ne:
  case Operation::UGt:
  case Operation::SGt:
    return {7, 3};
  case Operation::Set:
    return {3, 1};
  default:
    return {7, 7};
  }
}

ExecutionCase
makeOpcodeExecutionCase(Version TheVersion,
                        const UpstreamOpcodeDefinition &Definition) {
  const OpcodeInfo *Info = getOpcodeInfo(Definition.ID);
  EXPECT_NE(Info, nullptr);
  ExecutionCase Case;
  Case.Name = (versionName(TheVersion) + "-" + opcodeName(Definition.ID)).str();
  Case.TheVersion = TheVersion;
  Case.Input.assign(kExecutionInputPattern.begin(),
                    kExecutionInputPattern.end());
  Case.Budget = kExecutionBudget;

  std::vector<test::EncodedInstruction> Instructions;
  std::optional<size_t> LegacyCallSlot;
  const auto Emit = [&Instructions](Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                                    int16_t Offset = 0, int32_t Immediate = 0) {
    const size_t Slot = Instructions.size();
    Instructions.push_back(encodeUpstream(ID, Dst, Src, Offset, Immediate));
    return Slot;
  };
  if (!Info) {
    Case.Text = instructionBytes({encodeUpstream(Opcode::EXIT)});
    return Case;
  }

  switch (Definition.Form) {
  case OperandForm::LDDW: {
    Case.TargetSlot = Emit(Definition.ID, 0, 0, 0, 0x1234'5678);
    test::EncodedInstruction Continuation{};
    llvm::support::endian::write32le(Continuation.data() + kImmediateOffset,
                                     0x1122'3344);
    Instructions.push_back(Continuation);
    Emit(Opcode::EXIT);
    break;
  }
  case OperandForm::Load:
    Case.TargetSlot =
        Emit(Definition.ID, /*Dst=*/0, /*Src=*/kFirstArgumentRegister);
    Emit(Opcode::EXIT);
    break;
  case OperandForm::StoreImm:
  case OperandForm::StoreReg:
    if (Definition.Form == OperandForm::StoreReg)
      Emit(Opcode::MOV64_IMM, 2, 0, 0, kExecutionMemoryValue);
    Case.TargetSlot = Emit(
        Definition.ID, /*Dst=*/kFirstArgumentRegister,
        Definition.Form == OperandForm::StoreReg ? 2 : 0,
        /*Offset=*/0,
        Definition.Form == OperandForm::StoreImm ? kExecutionMemoryValue : 0);
    Emit(memoryLoadOpcode(TheVersion, Definition.Width), /*Dst=*/0,
         /*Src=*/kFirstArgumentRegister);
    Emit(Opcode::EXIT);
    break;
  case OperandForm::Dst:
  case OperandForm::DstImm:
  case OperandForm::DstSrc:
  case OperandForm::Endian: {
    int32_t Left = 21;
    int32_t Right = 3;
    int32_t Immediate = 3;
    if (Info->Op == Operation::SDiv || Info->Op == Operation::SRem) {
      Left = -21;
      Right = -3;
      Immediate = -3;
    }
    if (Info->Op == Operation::UHighMul) {
      Emit(Opcode::MOV32_IMM, 0, 0, 0, -1);
      Emit(Opcode::HOR64_IMM, 0, 0, 0, -1);
      if (Definition.Form == OperandForm::DstSrc) {
        Emit(Opcode::MOV32_IMM, 2, 0, 0, -1);
        Emit(Opcode::HOR64_IMM, 2, 0, 0, -1);
      }
      Immediate = -1;
    } else if (Info->Op == Operation::SHighMul) {
      Emit(Opcode::MOV32_IMM, 0);
      Emit(Opcode::HOR64_IMM, 0, 0, 0, std::numeric_limits<int32_t>::min());
      if (Definition.Form == OperandForm::DstSrc)
        Emit(Opcode::MOV64_IMM, 2, 0, 0, -2);
      Immediate = -2;
    } else {
      Emit(Opcode::MOV64_IMM, 0, 0, 0, Left);
      if (Definition.Form == OperandForm::DstSrc)
        Emit(Opcode::MOV64_IMM, 2, 0, 0, Right);
    }
    if (Definition.Form == OperandForm::Endian)
      Immediate = kWordBitWidth;
    else if (Info->Op == Operation::HighOr)
      Immediate = 0x1234'5678;
    Case.TargetSlot =
        Emit(Definition.ID, 0, Definition.Form == OperandForm::DstSrc ? 2 : 0,
             0, Immediate);
    Emit(Opcode::EXIT);
    break;
  }
  case OperandForm::Branch:
    Case.TargetSlot = Emit(Definition.ID, 0, 0, 2);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPoisonMarker));
    Emit(Opcode::JA, 0, 0, 1);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    break;
  case OperandForm::BranchImm:
  case OperandForm::BranchReg: {
    const auto [Left, Right] = branchOperands(Info->Op);
    Emit(Opcode::MOV64_IMM, 0, 0, 0, Left);
    if (Definition.Form == OperandForm::BranchReg)
      Emit(Opcode::MOV64_IMM, 2, 0, 0, Right);
    Case.TargetSlot = Emit(
        Definition.ID, 0, Definition.Form == OperandForm::BranchReg ? 2 : 0, 2,
        Definition.Form == OperandForm::BranchImm ? Right : 0);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPoisonMarker));
    Emit(Opcode::JA, 0, 0, 1);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    break;
  }
  case OperandForm::CallImm:
    Case.TargetSlot = Instructions.size();
    if (versionHasFeature(TheVersion, VersionFeature::StaticSyscalls)) {
      Emit(Definition.ID, 0, /*Src=*/1, 0, /*Immediate=*/1);
    } else {
      LegacyCallSlot = Emit(Definition.ID, 0, 0, 0,
                            static_cast<int32_t>(kExecutionFunctionKey));
      Case.TargetSlot = *LegacyCallSlot;
      Case.Functions.push_back(
          {kExecutionFunctionKey, 2, "execution_opcode_target"});
    }
    Emit(Opcode::EXIT);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    break;
  case OperandForm::CallReg: {
    constexpr size_t TargetBodySlot = 5;
    Emit(Opcode::MOV64_IMM, 2, 0, 0,
         static_cast<int32_t>(kBytecodeStart >>
                              std::numeric_limits<uint32_t>::digits));
    Emit(Opcode::LSH64_IMM, 2, 0, 0, std::numeric_limits<uint32_t>::digits);
    Emit(Opcode::ADD64_IMM, 2, 0, 0,
         static_cast<int32_t>(TargetBodySlot * kInstructionSize));
    Case.TargetSlot = Instructions.size();
    if (versionHasFeature(TheVersion, VersionFeature::CallXSource))
      Emit(Definition.ID, 0, 2);
    else if (versionHasFeature(TheVersion, VersionFeature::CallXDestination))
      Emit(Definition.ID, 2);
    else
      Emit(Definition.ID, 0, 0, 0, 2);
    Emit(Opcode::EXIT);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    break;
  }
  case OperandForm::None:
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Case.TargetSlot = Emit(Definition.ID);
    break;
  }

  Case.Text = instructionBytes(Instructions);
  if (LegacyCallSlot) {
    std::vector<test::EncodedInstruction> LoaderInstructions = Instructions;
    LoaderInstructions[*LegacyCallSlot] =
        encodeUpstream(Opcode::CALL_IMM, 0, 0, 0, 1);
    Case.LoaderText = instructionBytes(LoaderInstructions);
  }
  return Case;
}

std::vector<ExecutionCase> opcodeExecutionCases() {
  std::vector<ExecutionCase> Cases;
  Cases.reserve(kPinnedActiveOpcodeVersionPairCount);
  for (Version TheVersion : kOracleVersions)
    for (const UpstreamOpcodeDefinition &Definition : UpstreamOpcodeDefinitions)
      if (versionInMask(TheVersion, Definition.Versions))
        Cases.push_back(makeOpcodeExecutionCase(TheVersion, Definition));
  return Cases;
}

uint64_t executionProbeResult(const SyscallArguments &Arguments) {
  uint64_t Result = kProbeReturnBias;
  for (uint64_t Argument : Arguments)
    Result += Argument;
  return Result;
}

ExecutionCase
makeBoundaryExecutionCase(Version TheVersion,
                          const ExecutionCaseDefinition &Definition) {
  ExecutionCase Case;
  Case.Name = (versionName(TheVersion) + "-" + Definition.Name).str();
  Case.TheVersion = TheVersion;
  Case.Input.assign(kExecutionInputPattern.begin(),
                    kExecutionInputPattern.end());
  Case.Budget = Definition.Budget;
  Case.ExpectedTargetHits = Definition.TargetHits;
  Case.ExpectedOutcome = Definition.Outcome;
  Case.ExpectedFault = Definition.Fault;

  std::vector<test::EncodedInstruction> Instructions;
  const auto Emit = [&Instructions](Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                                    int16_t Offset = 0, int32_t Immediate = 0) {
    const size_t Slot = Instructions.size();
    Instructions.push_back(encodeUpstream(ID, Dst, Src, Offset, Immediate));
    return Slot;
  };
  const auto EmitSelectedCallX = [&Emit, TheVersion](uint8_t Register) {
    if (versionHasFeature(TheVersion, VersionFeature::CallXSource))
      return Emit(Opcode::CALL_REG, 0, Register);
    if (versionHasFeature(TheVersion, VersionFeature::CallXDestination))
      return Emit(Opcode::CALL_REG, Register);
    return Emit(Opcode::CALL_REG, 0, 0, 0, Register);
  };
  const auto EmitProgramAddress = [&Emit](uint8_t Register,
                                          int32_t ByteOffset) {
    Emit(Opcode::MOV64_IMM, Register, 0, 0,
         static_cast<int32_t>(kBytecodeStart >>
                              std::numeric_limits<uint32_t>::digits));
    Emit(Opcode::LSH64_IMM, Register, 0, 0,
         std::numeric_limits<uint32_t>::digits);
    Emit(Opcode::ADD64_IMM, Register, 0, 0, ByteOffset);
  };
  const auto EmitBranchObservation = [&Emit](Opcode Branch, uint8_t Dst,
                                             uint8_t Src, int32_t Immediate) {
    const size_t Target = Emit(Branch, Dst, Src, 2, Immediate);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPoisonMarker));
    Emit(Opcode::JA, 0, 0, 1);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    return Target;
  };

  switch (Definition.Builder) {
  case ExecutionCaseBuilder::Alu32NegativeResult:
    Emit(Opcode::MOV32_IMM, 0, 0, 0, -1);
    Case.TargetSlot = Emit(Opcode::ADD32_IMM, 0);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::Mov32NegativeRegister:
    Emit(Opcode::MOV64_IMM, 2, 0, 0, -1);
    Case.TargetSlot = Emit(Opcode::MOV32_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::Sub32ImmediateOrder:
    Emit(Opcode::MOV64_IMM, 0, 0, 0, 7);
    Case.TargetSlot = Emit(Opcode::SUB32_IMM, 0, 0, 0, 3);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::Sub64ImmediateOrder:
    Emit(Opcode::MOV64_IMM, 0, 0, 0, 7);
    Case.TargetSlot = Emit(Opcode::SUB64_IMM, 0, 0, 0, 3);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::CallXUnalignedFloor: {
    constexpr size_t BodySlot = 5;
    EmitProgramAddress(2, static_cast<int32_t>(BodySlot * kInstructionSize +
                                               (kInstructionSize - 1)));
    Case.TargetSlot = EmitSelectedCallX(2);
    Emit(Opcode::EXIT);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    Case.ExpectedResult = kExecutionPassMarker;
    break;
  }
  case ExecutionCaseBuilder::CallXOutsideText:
    EmitProgramAddress(2, 0x1000);
    Case.TargetSlot = EmitSelectedCallX(2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::UnknownImmediateCall:
    Case.TargetSlot = Emit(
        Opcode::CALL_IMM, 0,
        versionHasFeature(TheVersion, VersionFeature::StaticSyscalls) ? 0 : 0,
        0, -1);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::RecursiveCallDepth: {
    if (versionHasFeature(TheVersion, VersionFeature::StaticSyscalls)) {
      Case.TargetSlot = Emit(Opcode::CALL_IMM, 0, 1, 0, -1);
    } else {
      const uint32_t Key = legacyFunctionKey(0, {});
      Case.TargetSlot =
          Emit(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Key));
      Case.Functions.push_back({Key, 0, "recursive_execution_target"});
      // -1 is the legacy unresolved-call sentinel, so a self-recursive
      // relative CALL cannot cross the loader boundary. Feed the analyzer a
      // harmless in-range relative call, then install the exact explicit
      // from_text_bytes() registry below, just as the official oracle does.
      Case.LoaderText = instructionBytes(
          {encodeUpstream(Opcode::CALL_IMM), encodeUpstream(Opcode::EXIT)});
    }
    Emit(Opcode::EXIT);
    break;
  }
  case ExecutionCaseBuilder::MemoryLoadLastByte:
    Case.TargetSlot =
        Emit(Opcode::LD_B_REG, 0, kFirstArgumentRegister,
             static_cast<int16_t>(kExecutionInputPattern.size() - 1));
    Emit(Opcode::EXIT);
    Case.ExpectedResult = kExecutionInputPattern.back();
    break;
  case ExecutionCaseBuilder::MemoryLoadPastEnd:
    Case.TargetSlot =
        Emit(Opcode::LD_H_REG, 0, kFirstArgumentRegister,
             static_cast<int16_t>(kExecutionInputPattern.size() - 1));
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::DivisionZero32Legacy:
    Emit(Opcode::MOV64_IMM, 2);
    Case.TargetSlot = Emit(Opcode::DIV32_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::DivisionZero64Legacy:
    Emit(Opcode::MOV64_IMM, 2);
    Case.TargetSlot = Emit(Opcode::DIV64_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::UDivisionZero32:
    Emit(Opcode::MOV64_IMM, 2);
    Case.TargetSlot = Emit(Opcode::UDIV32_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::UDivisionZero64:
    Emit(Opcode::MOV64_IMM, 2);
    Case.TargetSlot = Emit(Opcode::UDIV64_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::SDivisionZero32:
    Emit(Opcode::MOV64_IMM, 2);
    Case.TargetSlot = Emit(Opcode::SDIV32_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::SDivisionZero64:
    Emit(Opcode::MOV64_IMM, 2);
    Case.TargetSlot = Emit(Opcode::SDIV64_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::SDivisionOverflow32Immediate:
    Emit(Opcode::MOV32_IMM, 0, 0, 0, std::numeric_limits<int32_t>::min());
    Case.TargetSlot = Emit(Opcode::SDIV32_IMM, 0, 0, 0, -1);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::SDivisionOverflow32Register:
    Emit(Opcode::MOV32_IMM, 0, 0, 0, std::numeric_limits<int32_t>::min());
    Emit(Opcode::MOV64_IMM, 2, 0, 0, -1);
    Case.TargetSlot = Emit(Opcode::SDIV32_REG, 0, 2);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::SDivisionOverflow64Immediate:
  case ExecutionCaseBuilder::SDivisionOverflow64Register:
    Emit(Opcode::MOV32_IMM, 0);
    Emit(Opcode::HOR64_IMM, 0, 0, 0, std::numeric_limits<int32_t>::min());
    if (Definition.Builder == ExecutionCaseBuilder::SDivisionOverflow64Register)
      Emit(Opcode::MOV64_IMM, 2, 0, 0, -1);
    Case.TargetSlot = Emit(
        Definition.Builder == ExecutionCaseBuilder::SDivisionOverflow64Register
            ? Opcode::SDIV64_REG
            : Opcode::SDIV64_IMM,
        0,
        Definition.Builder == ExecutionCaseBuilder::SDivisionOverflow64Register
            ? 2
            : 0,
        0, -1);
    Emit(Opcode::EXIT);
    break;
  case ExecutionCaseBuilder::LDDWWideSignBoundary: {
    Case.TargetSlot = Emit(Opcode::LDDW, 0, 0, 0, -1);
    test::EncodedInstruction Continuation{};
    llvm::support::endian::write32le(
        Continuation.data() + kImmediateOffset,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::min()));
    Instructions.push_back(Continuation);
    Emit(Opcode::EXIT);
    Case.ExpectedResult = 0x8000'0000'ffff'ffff;
    break;
  }
  case ExecutionCaseBuilder::Jmp32UnsignedLowHalf: {
    Emit(Opcode::LDDW, 0, 0, 0, 1);
    test::EncodedInstruction Continuation{};
    llvm::support::endian::write32le(Continuation.data() + kImmediateOffset, 1);
    Instructions.push_back(Continuation);
    Case.TargetSlot = EmitBranchObservation(Opcode::JLT32_IMM, 0, 0, 2);
    Case.ExpectedResult = kExecutionPassMarker;
    break;
  }
  case ExecutionCaseBuilder::Jmp32SignedLowHalf: {
    Emit(Opcode::LDDW, 0, 0, 0, -1);
    Instructions.emplace_back();
    Case.TargetSlot = EmitBranchObservation(Opcode::JSLT32_IMM, 0, 0, 0);
    Case.ExpectedResult = kExecutionPassMarker;
    break;
  }
  case ExecutionCaseBuilder::LegacySyscallFunctionCollision: {
    for (uint8_t Register = kFirstArgumentRegister;
         Register < kFirstArgumentRegister + kArgumentRegisterCount; ++Register)
      Emit(Opcode::MOV64_IMM, Register, 0, 0,
           Register - kFirstArgumentRegister + 1);
    const uint32_t Key = legacyFunctionKey(kCollisionFunctionSlot, {});
    Case.TargetSlot =
        Emit(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Key));
    Emit(Opcode::EXIT);
    EXPECT_EQ(Instructions.size(), kCollisionFunctionSlot);
    Emit(Opcode::MOV64_IMM, 0, 0, 0,
         static_cast<int32_t>(kExecutionPassMarker));
    Emit(Opcode::EXIT);
    Case.Functions.push_back(
        {Key, kCollisionFunctionSlot, "collision_execution_target"});
    std::vector<test::EncodedInstruction> LoaderInstructions = Instructions;
    LoaderInstructions[Case.TargetSlot] =
        encodeUpstream(Opcode::CALL_IMM, 0, 0, 0, 1);
    Case.LoaderText = instructionBytes(LoaderInstructions);
    Case.Syscalls = ExecutionSyscallProfile::CollisionProbe;
    Case.ExpectedResult = kExecutionPassMarker;
    break;
  }
  case ExecutionCaseBuilder::StaticProbeSyscall: {
    SyscallArguments Arguments{};
    for (uint8_t Register = kFirstArgumentRegister;
         Register < kFirstArgumentRegister + kArgumentRegisterCount;
         ++Register) {
      const uint64_t Value = Register - kFirstArgumentRegister + 1;
      Arguments[Register - kFirstArgumentRegister] = Value;
      Emit(Opcode::MOV64_IMM, Register, 0, 0, static_cast<int32_t>(Value));
    }
    Case.TargetSlot =
        Emit(Opcode::CALL_IMM, 0, 0, 0,
             static_cast<int32_t>(hashSymbolName(kOracleProbeSyscallName)));
    Emit(Opcode::EXIT);
    Case.Syscalls = ExecutionSyscallProfile::Probe;
    Case.ExpectedResult = executionProbeResult(Arguments);
    break;
  }
  case ExecutionCaseBuilder::CallXContinuationInvalidFetch:
  case ExecutionCaseBuilder::CallXContinuationMeterPrecedesFetch:
  case ExecutionCaseBuilder::CallXContinuationFetchAtBudget: {
    Emit(Opcode::LDDW, 2, 0, 0, static_cast<int32_t>(kInstructionSize));
    test::EncodedInstruction Continuation{};
    llvm::support::endian::write32le(
        Continuation.data() + kImmediateOffset,
        static_cast<uint32_t>(kBytecodeStart >>
                              std::numeric_limits<uint32_t>::digits));
    Instructions.push_back(Continuation);
    EmitSelectedCallX(2);
    Emit(Opcode::EXIT);
    Case.TargetSlot = 1;
    break;
  }
  }

  if (Case.Text.empty())
    Case.Text = instructionBytes(Instructions);
  return Case;
}

std::vector<ExecutionCase> boundaryExecutionCases() {
  std::vector<ExecutionCase> Cases;
  Cases.reserve(kPinnedBoundaryExecutionCaseCount);
  for (Version TheVersion : kOracleVersions)
    for (const ExecutionCaseDefinition &Definition : ExecutionCaseDefinitions)
      if (versionInMask(TheVersion, Definition.Versions))
        Cases.push_back(makeBoundaryExecutionCase(TheVersion, Definition));
  return Cases;
}

uint64_t executionSyscallDigest(llvm::ArrayRef<SyscallTraceEntry> Syscalls) {
  uint64_t Digest = kSyscallDigestOffsetBasis;
  for (const SyscallTraceEntry &Syscall : Syscalls)
    for (uint64_t Argument : Syscall.Arguments) {
      Digest ^= Argument;
      Digest *= kSyscallDigestPrime;
    }
  return Digest;
}

llvm::Expected<ExecutionFaultClass>
executionFaultClass(const ExecutionResult &Result) {
  if (Result.Status == ExecutionStatus::Returned)
    return ExecutionFaultClass::None;
  if (Result.Status == ExecutionStatus::StepLimit)
    return ExecutionFaultClass::InstructionMeter;
  if (Result.Status != ExecutionStatus::Faulted)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "NeverD execution stopped in a non-terminal status");
  switch (Result.Fault) {
  case FaultCode::InvalidInstruction:
  case FaultCode::InvalidRegister:
  case FaultCode::InvalidBranch:
    return ExecutionFaultClass::InvalidInstruction;
  case FaultCode::DivideByZero:
    return ExecutionFaultClass::DivideByZero;
  case FaultCode::DivideOverflow:
    return ExecutionFaultClass::DivideOverflow;
  case FaultCode::MemoryAccess:
    return ExecutionFaultClass::MemoryAccess;
  case FaultCode::CallDepth:
    return ExecutionFaultClass::CallDepth;
  case FaultCode::UnknownSyscall:
    return ExecutionFaultClass::UnresolvedCall;
  case FaultCode::UnknownIndirectCall:
    return ExecutionFaultClass::UnknownIndirectCall;
  case FaultCode::ExecutionOverrun:
    return ExecutionFaultClass::ExecutionOverrun;
  case FaultCode::None:
    break;
  }
  return llvm::createStringError(
      llvm::inconvertibleErrorCode(),
      "NeverD fault has no canonical execution-oracle class");
}

BinaryImage makeExecutionImage(const ExecutionCase &Case) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  const llvm::ArrayRef<uint8_t> LoaderText =
      Case.LoaderText.empty() ? llvm::ArrayRef<uint8_t>(Case.Text)
                              : llvm::ArrayRef<uint8_t>(Case.LoaderText);
  Image.Raw.assign(LoaderText.begin(), LoaderText.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = LoaderText.size();
  Text.FileSz = LoaderText.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  for (const RawFunction &Function : Case.Functions) {
    Symbol Entry;
    Entry.Name = Function.Name;
    Entry.Addr = kBytecodeStart + Function.TargetSlot * kInstructionSize;
    Entry.IsFunc = true;
    Image.Symbols.push_back(std::move(Entry));
  }

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(Case.TheVersion);
  Meta.Version = Case.TheVersion;
  Meta.StrictLayout =
      versionHasFeature(Case.TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, LoaderText.size()};
  Meta.TextVM = {kBytecodeStart, LoaderText.size()};
  Image.SBF = Meta;
  return Image;
}

llvm::Expected<ExecutionObservation>
runNeverDExecution(const ExecutionCase &Case) {
  AnalyzeOptions Analyze;
  Analyze.RecoverHighIR = false;
  Analyze.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
      kMinimumOracleVersion, kMaximumOracleVersion, SBFVMConfig{}};
  auto Program = analyze(makeExecutionImage(Case), Analyze);
  if (!Program) {
    llvm::consumeError(Program.takeError());
    return ExecutionObservation{ExecutionOutcomeKind::Rejected,
                                ExecutionFaultClass::None,
                                0,
                                0,
                                0,
                                Case.Input,
                                0,
                                kSyscallDigestOffsetBasis};
  }

  // The official side deliberately exercises from_text_bytes(), whose
  // explicit function registry is independent of its loader syscall registry.
  // Install the equivalent executable image after NeverD's loader/verifier
  // admission step so legacy CALL relocation cannot collapse that distinction.
  std::vector<ProgramFunctionEntry> Functions;
  Functions.reserve(Case.Functions.size());
  for (const RawFunction &Function : Case.Functions)
    Functions.push_back({Function.Key, Function.TargetSlot, Function.Name});
  auto ExecutableImage = createProgramImage(
      Case.Text, kBytecodeStart, {}, 0,
      !versionHasFeature(Case.TheVersion, VersionFeature::StrictELF),
      Case.TheVersion, 0, Functions);
  if (!ExecutableImage)
    return ExecutableImage.takeError();
  Program->ExecutableImage = std::move(*ExecutableImage);

  ExecutionEnvironment Environment;
  if (!Case.Input.empty())
    Environment.Memory.push_back(
        {kInputStart, Case.Input, true, "oracle.execution.input"});
  if (Case.Syscalls != ExecutionSyscallProfile::None) {
    const uint32_t ProbeHash =
        Case.Syscalls == ExecutionSyscallProfile::CollisionProbe
            ? legacyFunctionKey(kCollisionFunctionSlot, {})
            : hashSymbolName(kOracleProbeSyscallName);
    Environment.HostSyscall =
        [ProbeHash](uint32_t Hash,
                    const SyscallArguments &Arguments) -> SyscallOutcome {
      if (Hash != ProbeHash)
        return SyscallOutcome::unregistered();
      return SyscallOutcome::returned(executionProbeResult(Arguments));
    };
  }

  InterpreterOptions Options;
  Options.MaxSteps = Case.Budget;
  Options.RecordTrace = true;
  auto Result = executeRaw(*Program, std::move(Environment), Options);
  if (!Result)
    return Result.takeError();
  auto Fault = executionFaultClass(*Result);
  if (!Fault)
    return Fault.takeError();

  ExecutionObservation Observation;
  Observation.Outcome = Result->Status == ExecutionStatus::Returned
                            ? ExecutionOutcomeKind::Returned
                            : ExecutionOutcomeKind::Faulted;
  Observation.Fault = *Fault;
  Observation.Result =
      Result->Status == ExecutionStatus::Returned ? Result->ReturnValue : 0;
  Observation.InstructionCount = Result->Steps;
  Observation.TargetHits = static_cast<size_t>(
      std::count_if(Result->Trace.begin(), Result->Trace.end(),
                    [&Case](const InterpreterTraceEntry &Entry) {
                      return Entry.Slot == Case.TargetSlot;
                    }));
  Observation.Input = Case.Input;
  for (const MemoryRegion &Region : Result->Memory)
    if (Region.Address == kInputStart) {
      Observation.Input = Region.Bytes;
      break;
    }
  Observation.SyscallCount = Result->Syscalls.size();
  Observation.SyscallDigest = executionSyscallDigest(Result->Syscalls);
  return Observation;
}

std::vector<uint8_t> strictELFBaseline() {
  const test::EncodedInstruction Exit = encodeUpstream(Opcode::EXIT);
  test::StrictELFOptions Options;
  Options.TheVersion = Version::V3;
  Options.AddRodata = true;
  Options.Text = instructionBytes({Exit, Exit});
  return test::buildStrictELF(Options);
}

bool upstreamAcceptsEncoding(Version TheVersion, uint8_t Encoding) {
  return std::any_of(
      UpstreamOpcodeDefinitions.begin(), UpstreamOpcodeDefinitions.end(),
      [TheVersion, Encoding](const UpstreamOpcodeDefinition &Definition) {
        return Definition.Encoding == Encoding &&
               versionInMask(TheVersion, Definition.Versions);
      });
}

std::vector<uint8_t> makeEncodingProbe(Version TheVersion, uint8_t Encoding) {
  test::EncodedInstruction Probe{};
  Probe[kOpcodeOffset] = Encoding;
  llvm::support::endian::write32le(Probe.data() + kImmediateOffset,
                                   static_cast<uint32_t>(kValidImmediate));

  const UpstreamOpcodeDefinition *LittleEndian = upstreamOpcode(Opcode::LE);
  const UpstreamOpcodeDefinition *BigEndian = upstreamOpcode(Opcode::BE);
  const UpstreamOpcodeDefinition *WideLoad = upstreamOpcode(Opcode::LDDW);
  const UpstreamOpcodeDefinition *Call = upstreamOpcode(Opcode::CALL_IMM);
  EXPECT_NE(LittleEndian, nullptr);
  EXPECT_NE(BigEndian, nullptr);
  EXPECT_NE(WideLoad, nullptr);
  EXPECT_NE(Call, nullptr);
  if ((LittleEndian && Encoding == LittleEndian->Encoding) ||
      (BigEndian && Encoding == BigEndian->Encoding))
    llvm::support::endian::write32le(Probe.data() + kImmediateOffset,
                                     static_cast<uint32_t>(kHalfWordBitWidth));
  if (Call && Encoding == Call->Encoding)
    llvm::support::endian::write32le(Probe.data() + kImmediateOffset, 0);

  std::vector<test::EncodedInstruction> Instructions{Probe};
  if (WideLoad && Encoding == WideLoad->Encoding &&
      versionInMask(TheVersion, WideLoad->Versions))
    Instructions.emplace_back();
  Instructions.push_back(encodeUpstream(Opcode::EXIT));
  return instructionBytes(Instructions);
}

test::EncodedInstruction makeCallX(Version TheVersion, uint8_t Register) {
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int32_t Immediate = 0;
  if (versionHasFeature(TheVersion, VersionFeature::CallXSource))
    Src = Register;
  else if (versionHasFeature(TheVersion, VersionFeature::CallXDestination))
    Dst = Register;
  else
    Immediate = Register;
  return encodeUpstream(Opcode::CALL_REG, Dst, Src, 0, Immediate);
}

std::vector<uint8_t> makeVerifierDomainProbe(VerifierCaseBuilder Builder,
                                             Version TheVersion) {
  const test::EncodedInstruction Exit = encodeUpstream(Opcode::EXIT);
  const auto WithExit = [&Exit](test::EncodedInstruction Instruction) {
    return instructionBytes({Instruction, Exit});
  };
  switch (Builder) {
  case VerifierCaseBuilder::LDDWComplete:
    return instructionBytes({encodeUpstream(Opcode::LDDW), {}, Exit});
  case VerifierCaseBuilder::LDDWLast:
    return instructionBytes({encodeUpstream(Opcode::LDDW)});
  case VerifierCaseBuilder::LDDWNonZeroContinuation:
    return instructionBytes({encodeUpstream(Opcode::LDDW), Exit, Exit});
  case VerifierCaseBuilder::BranchToLDDWContinuation:
    return instructionBytes(
        {encodeUpstream(Opcode::JA, 0, 0, kBranchToContinuationOffset),
         encodeUpstream(Opcode::LDDW),
         {},
         Exit});
  case VerifierCaseBuilder::RegisterBoundary:
    return WithExit(encodeUpstream(Opcode::MOV64_REG, kMaximumWritableRegister,
                                   kFramePointerRegister));
  case VerifierCaseBuilder::FramePointerWrite:
    return WithExit(encodeUpstream(Opcode::MOV64_REG, kFramePointerRegister));
  case VerifierCaseBuilder::InvalidDestinationNibble:
    return WithExit(encodeUpstream(Opcode::MOV64_REG, kInvalidRegister));
  case VerifierCaseBuilder::InvalidSourceNibble:
    return WithExit(encodeUpstream(Opcode::MOV64_REG, 0, kInvalidRegister));
  case VerifierCaseBuilder::StoreThroughFramePointer:
    return WithExit(encodeUpstream(
        TheVersion == Version::V2 ? Opcode::ST_8B_IMM : Opcode::ST_DW_IMM,
        kFramePointerRegister));
  case VerifierCaseBuilder::DivisionOne:
    return WithExit(encodeUpstream(
        TheVersion == Version::V2 ? Opcode::UDIV64_IMM : Opcode::DIV64_IMM, 0,
        0, 0, kValidImmediate));
  case VerifierCaseBuilder::DivisionZero:
    return WithExit(encodeUpstream(
        TheVersion == Version::V2 ? Opcode::UDIV64_IMM : Opcode::DIV64_IMM, 0,
        0, 0, kInvalidImmediate));
  case VerifierCaseBuilder::ShiftMaximum:
    return WithExit(
        encodeUpstream(Opcode::LSH64_IMM, 0, 0, 0, kDoubleWordBitWidth - 1));
  case VerifierCaseBuilder::ShiftWidth:
    return WithExit(
        encodeUpstream(Opcode::LSH64_IMM, 0, 0, 0, kDoubleWordBitWidth));
  case VerifierCaseBuilder::ShiftNegative:
    return WithExit(encodeUpstream(Opcode::LSH64_IMM, 0, 0, 0, -1));
  case VerifierCaseBuilder::Endian16:
    return WithExit(encodeUpstream(Opcode::BE, 0, 0, 0, kHalfWordBitWidth));
  case VerifierCaseBuilder::Endian32:
    return WithExit(encodeUpstream(Opcode::BE, 0, 0, 0, kWordBitWidth));
  case VerifierCaseBuilder::Endian64:
    return WithExit(encodeUpstream(Opcode::BE, 0, 0, 0, kDoubleWordBitWidth));
  case VerifierCaseBuilder::EndianZero:
    return WithExit(encodeUpstream(Opcode::BE, 0, 0, 0, 0));
  case VerifierCaseBuilder::EndianByte:
    return WithExit(
        encodeUpstream(Opcode::BE, 0, 0, 0, kInvalidEndianByteWidth));
  case VerifierCaseBuilder::EndianBelowDoubleWord:
    return WithExit(
        encodeUpstream(Opcode::BE, 0, 0, 0, kInvalidEndianBelowDoubleWord));
  case VerifierCaseBuilder::EndianAboveDoubleWord:
    return WithExit(
        encodeUpstream(Opcode::BE, 0, 0, 0, kInvalidEndianAboveDoubleWord));
  case VerifierCaseBuilder::BranchInRange:
    return WithExit(encodeUpstream(Opcode::JA, 0, 0, kBranchInRangeOffset));
  case VerifierCaseBuilder::BranchForwardOutOfRange:
    return WithExit(
        encodeUpstream(Opcode::JA, 0, 0, kBranchForwardOutOfRangeOffset));
  case VerifierCaseBuilder::BranchBackwardOutOfRange:
    return WithExit(
        encodeUpstream(Opcode::JA, 0, 0, kBranchBackwardOutOfRangeOffset));
  case VerifierCaseBuilder::CallXSelectedMaximum:
    return WithExit(makeCallX(TheVersion, kMaximumWritableRegister));
  case VerifierCaseBuilder::CallXSelectedFramePointer:
    return WithExit(makeCallX(TheVersion, kFramePointerRegister));
  case VerifierCaseBuilder::CallXUnselectedOperands: {
    if (versionHasFeature(TheVersion, VersionFeature::CallXSource))
      return WithExit(encodeUpstream(Opcode::CALL_REG, kMaximumWritableRegister,
                                     kMaximumWritableRegister, 0,
                                     kFramePointerRegister));
    if (versionHasFeature(TheVersion, VersionFeature::CallXDestination))
      return WithExit(encodeUpstream(Opcode::CALL_REG, kMaximumWritableRegister,
                                     kFramePointerRegister, 0,
                                     kFramePointerRegister));
    return WithExit(encodeUpstream(Opcode::CALL_REG, kMaximumWritableRegister,
                                   kFramePointerRegister, 0,
                                   kMaximumWritableRegister));
  }
  }
  llvm_unreachable("unknown official verifier case builder");
}

std::vector<VerificationCase> verificationCases() {
  std::vector<VerificationCase> Cases;
  Cases.reserve(kExhaustiveEncodingCaseCount + kVerifierDomainCaseCount);
  for (Version TheVersion : kOracleVersions) {
    for (unsigned Encoding = 0; Encoding < kOpcodeEncodingCount; ++Encoding) {
      Cases.push_back(
          {(llvm::Twine("encoding-") + llvm::utohexstr(Encoding)).str(),
           TheVersion,
           makeEncodingProbe(TheVersion, static_cast<uint8_t>(Encoding)),
           upstreamAcceptsEncoding(TheVersion, static_cast<uint8_t>(Encoding))
               ? AcceptanceExpectation::Accept
               : AcceptanceExpectation::Reject});
    }
    for (const VerifierCaseDefinition &Definition : VerifierCaseDefinitions) {
      if (!versionInMask(TheVersion, Definition.Versions))
        continue;
      Cases.push_back({Definition.Name.str(), TheVersion,
                       makeVerifierDomainProbe(Definition.Builder, TheVersion),
                       Definition.Expected});
    }
  }
  return Cases;
}

bool officialOracleRequired() {
  const char *Required = std::getenv(kOracleRequiredEnvironment.data());
  return Required && llvm::StringRef(Required) == kOracleRequiredValue;
}

std::optional<std::string> oracleExecutable() {
  const char *Executable = std::getenv(kOracleExecutableEnvironment.data());
  return Executable && *Executable ? std::optional<std::string>(Executable)
                                   : std::nullopt;
}

std::optional<OracleConfiguration> oracleConfiguration() {
  const std::optional<std::string> Executable = oracleExecutable();
  const char *Corpus = std::getenv(kOracleCorpusEnvironment.data());
  if (!Executable || !Corpus || !*Corpus)
    return std::nullopt;
  return OracleConfiguration{*Executable, Corpus};
}

BinaryImage makeVerificationImage(Version TheVersion,
                                  llvm::ArrayRef<uint8_t> TextBytes) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  Image.Raw.assign(TextBytes.begin(), TextBytes.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = TextBytes.size();
  Text.FileSz = TextBytes.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(TheVersion);
  Meta.Version = TheVersion;
  Meta.StrictLayout = versionHasFeature(TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, TextBytes.size()};
  Meta.TextVM = {kBytecodeStart, TextBytes.size()};
  Image.SBF = Meta;
  return Image;
}

AcceptanceExpectation runNeverDVerification(const VerificationCase &Case) {
  AnalyzeOptions Options;
  Options.RecoverHighIR = false;
  Options.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
      kMinimumOracleVersion, kMaximumOracleVersion, SBFVMConfig{}};
  auto Program =
      analyze(makeVerificationImage(Case.TheVersion, Case.Text), Options);
  if (Program)
    return AcceptanceExpectation::Accept;
  llvm::consumeError(Program.takeError());
  return AcceptanceExpectation::Reject;
}

AcceptanceExpectation runNeverDELFVerification(const ELFMutationCase &Case) {
  test::TemporaryFile File("elf-mutation");
  EXPECT_FALSE(File.error()) << File.error().message();
  if (File.error())
    return AcceptanceExpectation::Reject;
  {
    std::ofstream Output(File.str().str(), std::ios::binary);
    EXPECT_TRUE(Output);
    if (!Output)
      return AcceptanceExpectation::Reject;
    Output.write(reinterpret_cast<const char *>(Case.Bytes.data()),
                 static_cast<std::streamsize>(Case.Bytes.size()));
    EXPECT_TRUE(Output);
    if (!Output)
      return AcceptanceExpectation::Reject;
  }

  auto Image = loadBinary(File.str().str());
  if (!Image) {
    llvm::consumeError(Image.takeError());
    return AcceptanceExpectation::Reject;
  }
  AnalyzeOptions Options;
  Options.RecoverHighIR = false;
  Options.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
      kMinimumOracleVersion, kMaximumOracleVersion, SBFVMConfig{}};
  auto Program = analyze(*Image, Options);
  if (Program)
    return AcceptanceExpectation::Accept;
  llvm::consumeError(Program.takeError());
  return AcceptanceExpectation::Reject;
}

llvm::Expected<std::vector<AcceptanceExpectation>>
parseVerificationOutcomes(llvm::StringRef Output, size_t ExpectedCount,
                          llvm::StringRef RecordKind) {
  std::vector<std::optional<AcceptanceExpectation>> Records(ExpectedCount);
  while (!Output.empty()) {
    auto [Line, Rest] = Output.split('\n');
    Output = Rest;
    Line = Line.trim();
    if (Line.empty())
      continue;
    if (!Line.starts_with(kOracleProtocol))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unexpected oracle output '%s'",
                                     Line.str().c_str());

    llvm::SmallVector<llvm::StringRef, 4> Fields;
    Line.split(Fields, ' ', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
    if (Fields.size() != 4 || Fields[0] != kOracleProtocol ||
        Fields[1] != RecordKind)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "malformed verification record '%s'",
                                     Line.str().c_str());
    auto Index = parseUnsigned(Fields[2]);
    if (!Index)
      return Index.takeError();
    if (*Index >= ExpectedCount)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "verification record index %llu is outside batch size %zu",
          static_cast<unsigned long long>(*Index), ExpectedCount);
    if (Records[static_cast<size_t>(*Index)])
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "duplicate verification record %llu",
                                     static_cast<unsigned long long>(*Index));
    if (Fields[3] == kOracleAccepted)
      Records[static_cast<size_t>(*Index)] = AcceptanceExpectation::Accept;
    else if (Fields[3] == kOracleRejected)
      Records[static_cast<size_t>(*Index)] = AcceptanceExpectation::Reject;
    else
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unknown verification status '%s'",
                                     Fields[3].str().c_str());
  }

  std::vector<AcceptanceExpectation> Result;
  Result.reserve(ExpectedCount);
  for (size_t Index = 0; Index < Records.size(); ++Index) {
    if (!Records[Index])
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "missing verification record %zu", Index);
    Result.push_back(*Records[Index]);
  }
  return Result;
}

std::string verificationRecord(size_t Index, llvm::StringRef Status) {
  return (llvm::Twine(kOracleProtocol) + " " + kOracleELFVerificationRecord +
          " " + llvm::Twine(static_cast<unsigned long long>(Index)) + " " +
          Status + "\n")
      .str();
}

void expectStrictVerificationRecordParsing() {
  constexpr size_t kRecordCount = 2;
  constexpr size_t kFirstRecordIndex = 0;
  constexpr size_t kSecondRecordIndex = 1;
  const std::string First =
      verificationRecord(kFirstRecordIndex, kOracleAccepted);
  const std::string Second =
      verificationRecord(kSecondRecordIndex, kOracleRejected);

  auto SingleProtocol = protocolLine(First);
  ASSERT_TRUE(static_cast<bool>(SingleProtocol));
  auto DuplicateProtocol = protocolLine(First + Second);
  ASSERT_FALSE(static_cast<bool>(DuplicateProtocol));
  llvm::consumeError(DuplicateProtocol.takeError());
  auto MissingProtocol = protocolLine("");
  ASSERT_FALSE(static_cast<bool>(MissingProtocol));
  llvm::consumeError(MissingProtocol.takeError());
  auto WithProgramOutput = protocolLine("log: foo\n" + First);
  ASSERT_FALSE(static_cast<bool>(WithProgramOutput));
  llvm::consumeError(WithProgramOutput.takeError());

  auto Valid = parseVerificationOutcomes(First + Second, kRecordCount,
                                         kOracleELFVerificationRecord);
  ASSERT_TRUE(static_cast<bool>(Valid))
      << (Valid ? std::string() : llvm::toString(Valid.takeError()));
  ASSERT_EQ(Valid->size(), kRecordCount);
  EXPECT_EQ((*Valid)[kFirstRecordIndex], AcceptanceExpectation::Accept);
  EXPECT_EQ((*Valid)[kSecondRecordIndex], AcceptanceExpectation::Reject);

  const auto ExpectInfrastructureFailure = [](auto Result) {
    ASSERT_FALSE(static_cast<bool>(Result));
    llvm::consumeError(Result.takeError());
  };
  ExpectInfrastructureFailure(parseVerificationOutcomes(
      First, kRecordCount, kOracleELFVerificationRecord));
  ExpectInfrastructureFailure(parseVerificationOutcomes(
      First + First, 1, kOracleELFVerificationRecord));
  ExpectInfrastructureFailure(
      parseVerificationOutcomes(Second, 1, kOracleELFVerificationRecord));
  ExpectInfrastructureFailure(parseVerificationOutcomes(
      verificationRecord(kFirstRecordIndex, kOracleReturned), 1,
      kOracleELFVerificationRecord));
  ExpectInfrastructureFailure(parseVerificationOutcomes(
      (llvm::Twine(kOracleVersionRecord) + "\n").str(), 1,
      kOracleELFVerificationRecord));
  ExpectInfrastructureFailure(parseVerificationOutcomes(
      "junk\n" + First, 1, kOracleELFVerificationRecord));
}

llvm::Expected<std::vector<AcceptanceExpectation>>
runOfficialVerificationBatch(llvm::StringRef Oracle,
                             llvm::ArrayRef<VerificationCase> Cases,
                             OracleProcessResult &Process) {
  test::TemporaryFile InputFile("verification-batch");
  if (InputFile.error())
    return llvm::errorCodeToError(InputFile.error());
  {
    std::ofstream Output(InputFile.str().str(), std::ios::binary);
    if (!Output)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "cannot open verification batch input");
    Output << kOracleBatchProtocol.str() << '\n';
    for (size_t Index = 0; Index < Cases.size(); ++Index)
      Output << Index << ' ' << oracleVersionName(Cases[Index].TheVersion).str()
             << ' ' << llvm::toHex(Cases[Index].Text, /*LowerCase=*/true)
             << '\n';
    if (!Output)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "cannot write verification batch input");
  }

  Process = runOracle(Oracle, {kOracleVerifyBatchCommand,
                               kOracleBatchInputArgument, InputFile.str()});
  if (Process.ExitCode != 0)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "official oracle exited with %d: %s%s",
        Process.ExitCode, Process.ExecutionError.c_str(),
        Process.StandardError.c_str());
  return parseVerificationOutcomes(Process.StandardOutput, Cases.size(),
                                   kOracleVerificationRecord);
}

llvm::Expected<std::vector<AcceptanceExpectation>>
runOfficialELFVerificationBatch(llvm::StringRef Oracle,
                                llvm::ArrayRef<ELFMutationCase> Cases,
                                OracleProcessResult &Process) {
  test::TemporaryFile InputFile("elf-verification-batch");
  if (InputFile.error())
    return llvm::errorCodeToError(InputFile.error());
  {
    std::ofstream Output(InputFile.str().str(), std::ios::binary);
    if (!Output)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "cannot open ELF verification batch");
    Output << kOracleELFBatchProtocol.str() << '\n';
    for (size_t Index = 0; Index < Cases.size(); ++Index)
      Output << Index << ' '
             << llvm::toHex(Cases[Index].Bytes, /*LowerCase=*/true) << '\n';
    if (!Output)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "cannot write ELF verification batch");
  }

  Process = runOracle(Oracle, {kOracleVerifyELFBatchCommand,
                               kOracleBatchInputArgument, InputFile.str()});
  if (Process.ExitCode != 0)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "official oracle exited with %d: %s%s",
        Process.ExitCode, Process.ExecutionError.c_str(),
        Process.StandardError.c_str());
  return parseVerificationOutcomes(Process.StandardOutput, Cases.size(),
                                   kOracleELFVerificationRecord);
}

void expectPinnedOracle(llvm::StringRef Oracle) {
  OracleProcessResult Process = runOracle(Oracle, {kOracleVersionCommand});
  ASSERT_EQ(Process.ExitCode, 0)
      << Process.ExecutionError << Process.StandardError;
  auto Line = protocolLine(Process.StandardOutput);
  ASSERT_TRUE(static_cast<bool>(Line))
      << (Line ? std::string() : llvm::toString(Line.takeError()))
      << Process.StandardOutput;
  llvm::SmallVector<llvm::StringRef, 3> Fields;
  Line->split(Fields, ' ', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  ASSERT_EQ(Fields.size(), 3u) << Line->str();
  EXPECT_EQ(Fields[0], kOracleProtocol);
  EXPECT_EQ(Fields[1], kOracleVersionRecord);
  EXPECT_EQ(Fields[2], kSBPFMainRevision)
      << "the external oracle is not built from the audited sbpf revision";
}

BinaryImage makeRawImage(const RawCase &Case) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  for (const test::EncodedInstruction &Instruction : Case.Instructions)
    Image.Raw.insert(Image.Raw.end(), Instruction.begin(), Instruction.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = Image.Raw.size();
  Text.FileSz = Image.Raw.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  for (const RawFunction &Function : Case.Functions) {
    Symbol Entry;
    Entry.Name = Function.Name;
    Entry.Addr = kBytecodeStart + Function.TargetSlot * kInstructionSize;
    Entry.IsFunc = true;
    Image.Symbols.push_back(std::move(Entry));
  }

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(Case.TheVersion);
  Meta.Version = Case.TheVersion;
  Meta.StrictLayout =
      versionHasFeature(Case.TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, Image.Raw.size()};
  Meta.TextVM = {kBytecodeStart, Image.Raw.size()};
  Image.SBF = Meta;
  return Image;
}

OracleOutcome runNeverDText(const RawCase &Case) {
  BinaryImage Image = makeRawImage(Case);
  AnalyzeOptions Analyze;
  Analyze.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
      kMinimumOracleVersion, kMaximumOracleVersion, SBFVMConfig{}};
  auto Program = analyze(Image, Analyze);
  if (!Program) {
    llvm::consumeError(Program.takeError());
    return {OracleOutcomeKind::Rejected};
  }

  // The official from_text_bytes() boundary accepts an explicit function
  // registry. Rebuild the synthetic canonical image with that same registry
  // so this test exercises runtime dispatch rather than NeverD's ELF-derived
  // key construction.
  if (!Case.Functions.empty() || !Case.OfficialInstructions.empty()) {
    const std::vector<uint8_t> CanonicalText =
        Case.OfficialInstructions.empty()
            ? std::vector<uint8_t>(Program->text().begin(),
                                   Program->text().end())
            : instructionBytes(Case.OfficialInstructions);
    std::vector<ProgramFunctionEntry> Functions;
    Functions.reserve(Case.Functions.size());
    for (const RawFunction &Function : Case.Functions)
      Functions.push_back({Function.Key, Function.TargetSlot, Function.Name});
    auto ExecutableImage = createProgramImage(
        CanonicalText, kBytecodeStart, {}, 0,
        !versionHasFeature(Case.TheVersion, VersionFeature::StrictELF),
        Case.TheVersion, 0, Functions);
    if (!ExecutableImage) {
      llvm::consumeError(ExecutableImage.takeError());
      return {OracleOutcomeKind::Rejected};
    }
    Program->ExecutableImage = std::move(*ExecutableImage);
  }

  InterpreterOptions Options;
  Options.MaxSteps = Case.Budget;
  Options.RecordTrace = false;
  auto Result = executeRaw(
      *Program, makeEnvironment(InputProfile::None, Case.Syscalls), Options);
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return {OracleOutcomeKind::OtherFault};
  }
  if (Result->Status == ExecutionStatus::Returned)
    return {OracleOutcomeKind::Returned, Result->ReturnValue, Result->Steps};
  if (Result->Status == ExecutionStatus::Faulted &&
      Result->Fault == FaultCode::CallDepth)
    return {OracleOutcomeKind::CallDepthFault, 0, Result->Steps};
  return {OracleOutcomeKind::OtherFault, 0, Result->Steps};
}

llvm::Expected<OracleOutcome> runOfficialText(llvm::StringRef Oracle,
                                              const RawCase &Case,
                                              llvm::StringRef TextPath,
                                              OracleProcessResult &Process) {
  std::vector<std::string> ArgumentStorage{
      kOracleTextCommand.str(),
      kOracleTextArgument.str(),
      TextPath.str(),
      kOracleSBPFVersionArgument.str(),
      versionName(Case.TheVersion).str(),
      kOracleInputArgument.str(),
      kOracleNoneProfile.str(),
      kOracleSyscallsArgument.str(),
      syscallProfileName(Case.Syscalls),
      kOracleBudgetArgument.str(),
      llvm::utostr(Case.Budget),
  };
  for (const RawFunction &Function : Case.Functions) {
    ArgumentStorage.push_back(kOracleFunctionArgument.str());
    ArgumentStorage.push_back(llvm::utostr(Function.Key) + ":" +
                              llvm::utostr(Function.TargetSlot));
  }
  llvm::SmallVector<llvm::StringRef, 18> Arguments;
  Arguments.reserve(ArgumentStorage.size());
  for (const std::string &Argument : ArgumentStorage)
    Arguments.push_back(Argument);

  Process = runOracle(Oracle, Arguments);
  if (Process.ExitCode != 0)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "official oracle exited with %d: %s%s",
        Process.ExitCode, Process.ExecutionError.c_str(),
        Process.StandardError.c_str());
  return parseOutcome(Process.StandardOutput);
}

std::vector<RawCase> rawCases() {
  constexpr uint64_t PQRExpected =
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 1;
  constexpr size_t StaticTargetSlot = 2;
  constexpr size_t CallXTargetSlot = 5;
  constexpr int32_t UnalignedCallXTarget =
      static_cast<int32_t>(CallXTargetSlot * kInstructionSize + 7);
  constexpr size_t LegacyTargetSlot = 2;
  const uint32_t LegacyKey = legacyFunctionKey(LegacyTargetSlot, {});
  const uint32_t LogKey = hashSymbolName("log");
  test::EncodedInstruction LDDWContinuation{};
  llvm::support::endian::write32le(
      LDDWContinuation.data() + kImmediateOffset,
      static_cast<uint32_t>(kBytecodeStart >>
                            std::numeric_limits<uint32_t>::digits));
  const std::vector<test::EncodedInstruction> CallXContinuation{
      test::encode(Opcode::LDDW, 2, 0, 0,
                   static_cast<int32_t>(kInstructionSize)),
      LDDWContinuation, test::encode(Opcode::CALL_REG, 2),
      test::encode(Opcode::EXIT)};

  return {
      {"v2-pqr-uhmul-extreme",
       Version::V2,
       {test::encode(Opcode::ADD64_IMM, kFramePointerRegister),
        test::encode(Opcode::MOV32_IMM, 0, 0, 0, -1),
        test::encode(Opcode::HOR64_IMM, 0, 0, 0, -1),
        test::encode(Opcode::MOV32_IMM, 1, 0, 0, -1),
        test::encode(Opcode::HOR64_IMM, 1),
        test::encode(Opcode::UHMUL64_REG, 0, 1), test::encode(Opcode::EXIT)},
       {},
       {},
       SyscallProfile::None,
       kRawExecutionBudget,
       OracleOutcomeKind::Returned,
       PQRExpected,
       7},
      {"v3-static-call",
       Version::V3,
       {test::encode(Opcode::CALL_IMM, 0, 1, 0,
                     static_cast<int32_t>(StaticTargetSlot - 1)),
        test::encode(Opcode::EXIT),
        test::encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
        test::encode(Opcode::EXIT)},
       {},
       {},
       SyscallProfile::None,
       kRawExecutionBudget,
       OracleOutcomeKind::Returned,
       42,
       4},
      {"v3-unaligned-callx-floor",
       Version::V3,
       {test::encode(Opcode::MOV64_IMM, 2, 0, 0, 1),
        test::encode(Opcode::LSH64_IMM, 2, 0, 0, 32),
        test::encode(Opcode::ADD64_IMM, 2, 0, 0, UnalignedCallXTarget),
        test::encode(Opcode::CALL_REG, 2), test::encode(Opcode::EXIT),
        test::encode(Opcode::MOV64_IMM, 0, 0, 0, 77),
        test::encode(Opcode::EXIT)},
       {},
       {},
       SyscallProfile::None,
       kRawExecutionBudget,
       OracleOutcomeKind::Returned,
       77,
       7},
      {"v0-legacy-call",
       Version::V0,
       {test::encode(Opcode::CALL_IMM, 0, 0, 0, 1), test::encode(Opcode::EXIT),
        test::encode(Opcode::MOV64_IMM, 0, 0, 0, 33),
        test::encode(Opcode::EXIT)},
       {test::encode(Opcode::CALL_IMM, 0, 0, 0,
                     static_cast<int32_t>(LegacyKey)),
        test::encode(Opcode::EXIT),
        test::encode(Opcode::MOV64_IMM, 0, 0, 0, 33),
        test::encode(Opcode::EXIT)},
       {{LegacyKey, LegacyTargetSlot, "legacy_target"}},
       SyscallProfile::None,
       kRawExecutionBudget,
       OracleOutcomeKind::Returned,
       33,
       4},
      {"v0-legacy-syscall-then-colliding-function",
       Version::V0,
       {test::encode(Opcode::CALL_IMM, 0, 0, 0, 1), test::encode(Opcode::EXIT),
        test::encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
        test::encode(Opcode::EXIT)},
       {test::encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(LogKey)),
        test::encode(Opcode::EXIT),
        test::encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
        test::encode(Opcode::EXIT)},
       {{LogKey, LegacyTargetSlot, "legacy_collision_target"}},
       SyscallProfile::Log,
       kRawExecutionBudget,
       OracleOutcomeKind::Returned,
       42,
       4},
      {"v3-callx-continuation-invalid-fetch",
       Version::V3,
       CallXContinuation,
       {},
       {},
       SyscallProfile::None,
       kRawExecutionBudget,
       OracleOutcomeKind::OtherFault,
       0,
       3},
      {"v3-callx-continuation-meter-precedes-fetch",
       Version::V3,
       CallXContinuation,
       {},
       {},
       SyscallProfile::None,
       2,
       OracleOutcomeKind::OtherFault,
       0,
       2},
      {"v3-callx-continuation-fetch-at-budget-boundary",
       Version::V3,
       CallXContinuation,
       {},
       {},
       SyscallProfile::None,
       3,
       OracleOutcomeKind::OtherFault,
       0,
       3},
  };
}

TEST(SBFExternalOracle, PinnedCorpusMatchesIndependentOfficialProcess) {
  ASSERT_EQ(std::size(Fixtures), kPinnedManifestELFCount);
  const std::optional<OracleConfiguration> Configuration =
      oracleConfiguration();
  if (!Configuration) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle configuration is missing: "
                    << kOracleExecutableEnvironment.str() << " and "
                    << kOracleCorpusEnvironment.str() << " must both be set";
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str() << " and "
                 << kOracleCorpusEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(Configuration->Executable));
  ASSERT_TRUE(std::filesystem::is_directory(Configuration->Corpus));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(Configuration->Executable));

  for (const FixtureExpectation &Fixture : Fixtures) {
    SCOPED_TRACE(Fixture.File);
    const std::filesystem::path Path =
        Configuration->Corpus / kOracleCorpusTestsDirectory.str() /
        kOracleCorpusELFDirectory.str() / Fixture.File;
    ASSERT_TRUE(std::filesystem::is_regular_file(Path));

    OracleProcessResult Process;
    auto Official =
        runOfficialELF(Configuration->Executable, Fixture, Path, Process);
    ASSERT_TRUE(static_cast<bool>(Official))
        << (Official ? std::string() : llvm::toString(Official.takeError()))
        << "\nstdout:\n"
        << Process.StandardOutput << "\nstderr:\n"
        << Process.StandardError;
    const OracleOutcome Published = expectedELFOutcome(Fixture);
    EXPECT_EQ(Official->Kind, Published.Kind)
        << "official=" << outcomeName(Official->Kind).str()
        << " published=" << outcomeName(Published.Kind).str();
    if (Published.Kind == OracleOutcomeKind::Returned)
      EXPECT_EQ(Official->Result, Published.Result);
    const OracleOutcome NeverD = runNeverDELF(Fixture, Path);
    EXPECT_EQ(NeverD.Kind, Official->Kind)
        << "NeverD=" << outcomeName(NeverD.Kind).str()
        << " official=" << outcomeName(Official->Kind).str();
    if (Official->Kind == OracleOutcomeKind::Returned)
      EXPECT_EQ(NeverD.Result, Official->Result);
  }
}

TEST(SBFExternalOracle, ProtocolRecordsAreStrictlyValidated) {
  ASSERT_NO_FATAL_FAILURE(expectStrictVerificationRecordParsing());

  const std::string ValidRecord =
      (kOracleProtocol + " " + kOracleExecutionRecord +
       " 0 returned none 42 2 1 0011 0 " +
       llvm::utostr(kSyscallDigestOffsetBasis) + "\n" + kOracleProtocol + " " +
       kOracleExecutionSummaryRecord + " 1\n")
          .str();
  auto Junk = parseExecutionBatchOutput("junk\n" + ValidRecord, 1);
  EXPECT_FALSE(static_cast<bool>(Junk));
  if (!Junk)
    llvm::consumeError(Junk.takeError());

  auto Valid = parseExecutionBatchOutput(ValidRecord, 1);
  ASSERT_TRUE(static_cast<bool>(Valid))
      << (Valid ? std::string() : llvm::toString(Valid.takeError()));
  ASSERT_EQ(Valid->size(), 1u);
  EXPECT_EQ((*Valid)[0].Result, 42u);
}

TEST(SBFExternalOracle,
     GeneratedV1V2V4ELFHeadersMatchIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  const test::EncodedInstruction Exit = encodeUpstream(Opcode::EXIT);
  const std::vector<uint8_t> ExitText(Exit.begin(), Exit.end());
  test::LegacyELFOptions V1Options;
  V1Options.TheVersion = Version::V1;
  V1Options.Text = ExitText;
  test::LegacyELFOptions V2Options;
  V2Options.TheVersion = Version::V2;
  V2Options.Text = ExitText;
  test::StrictELFOptions V4Options;
  V4Options.TheVersion = Version::V4;
  V4Options.Text = ExitText;

  struct GeneratedELF {
    llvm::StringLiteral Name;
    Version TheVersion;
    std::vector<uint8_t> Bytes;
  };
  std::array Generated = {
      GeneratedELF{"generated-v1.so", Version::V1,
                   test::buildLegacyELF(V1Options)},
      GeneratedELF{"generated-v2.so", Version::V2,
                   test::buildLegacyELF(V2Options)},
      GeneratedELF{"generated-v4.so", Version::V4,
                   test::buildStrictELF(V4Options)},
  };

  for (const GeneratedELF &ELF : Generated) {
    SCOPED_TRACE(ELF.Name.str());
    ASSERT_FALSE(ELF.Bytes.empty());
    test::TemporaryFile File("so");
    ASSERT_FALSE(File.error()) << File.error().message();
    {
      std::ofstream Output(File.str().str(), std::ios::binary);
      ASSERT_TRUE(Output);
      Output.write(reinterpret_cast<const char *>(ELF.Bytes.data()),
                   static_cast<std::streamsize>(ELF.Bytes.size()));
      ASSERT_TRUE(Output);
    }

    const FixtureExpectation Fixture{ELF.Name.data(),
                                     ELF.TheVersion,
                                     LoadExpectation::Accept,
                                     VMProfile::Default,
                                     InputProfile::None,
                                     SyscallProfile::None,
                                     0,
                                     ExecutionExpectation::NotRun,
                                     0};
    OracleProcessResult Process;
    auto Official =
        runOfficialELF(*Executable, Fixture, File.str().str(), Process);
    ASSERT_TRUE(static_cast<bool>(Official))
        << (Official ? std::string() : llvm::toString(Official.takeError()))
        << "\nstdout:\n"
        << Process.StandardOutput << "\nstderr:\n"
        << Process.StandardError;
    EXPECT_EQ(Official->Kind, OracleOutcomeKind::Accepted)
        << "official=" << outcomeName(Official->Kind).str();
    const OracleOutcome NeverD = runNeverDELF(Fixture, File.str().str());
    EXPECT_EQ(NeverD.Kind, Official->Kind)
        << "NeverD=" << outcomeName(NeverD.Kind).str()
        << " official=" << outcomeName(Official->Kind).str();
  }
}

TEST(SBFExternalOracle, RawTextSemanticsMatchIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  for (const RawCase &Case : rawCases()) {
    SCOPED_TRACE(Case.Name);
    test::TemporaryFile TextFile("text");
    ASSERT_FALSE(TextFile.error()) << TextFile.error().message();
    {
      std::ofstream Output(TextFile.str().str(), std::ios::binary);
      ASSERT_TRUE(Output);
      const llvm::ArrayRef<test::EncodedInstruction> OfficialInstructions =
          Case.OfficialInstructions.empty()
              ? llvm::ArrayRef<test::EncodedInstruction>(Case.Instructions)
              : llvm::ArrayRef<test::EncodedInstruction>(
                    Case.OfficialInstructions);
      for (const test::EncodedInstruction &Instruction : OfficialInstructions)
        Output.write(reinterpret_cast<const char *>(Instruction.data()),
                     Instruction.size());
    }

    OracleProcessResult Process;
    auto Official = runOfficialText(*Executable, Case, TextFile.str(), Process);
    ASSERT_TRUE(static_cast<bool>(Official))
        << (Official ? std::string() : llvm::toString(Official.takeError()))
        << "\nstdout:\n"
        << Process.StandardOutput << "\nstderr:\n"
        << Process.StandardError;
    ASSERT_EQ(Official->Kind, Case.ExpectedKind);
    if (Case.ExpectedKind == OracleOutcomeKind::Returned)
      EXPECT_EQ(Official->Result, Case.ExpectedResult);
    EXPECT_EQ(Official->InstructionCount, Case.ExpectedInstructionCount);

    const OracleOutcome NeverD = runNeverDText(Case);
    EXPECT_EQ(NeverD.Kind, Official->Kind)
        << "NeverD=" << outcomeName(NeverD.Kind).str()
        << " official=" << outcomeName(Official->Kind).str();
    if (Official->Kind == OracleOutcomeKind::Returned)
      EXPECT_EQ(NeverD.Result, Official->Result);
    EXPECT_EQ(NeverD.InstructionCount, Official->InstructionCount);
  }
}

TEST(SBFExternalOracle,
     EveryActiveVersionOpcodeExecutesAgainstIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  const std::vector<ExecutionCase> Cases = opcodeExecutionCases();
  ASSERT_EQ(Cases.size(), kPinnedActiveOpcodeVersionPairCount);
  ASSERT_EQ(Cases.size() + kPinnedBoundaryExecutionCaseCount,
            kPinnedExecutionCaseCount);
  std::array<std::array<bool, static_cast<size_t>(Opcode::Unknown)>,
             kConcreteVersionCount>
      Seen{};
  size_t ExpectedIndex = 0;
  for (size_t VersionIndex = 0; VersionIndex < kOracleVersions.size();
       ++VersionIndex) {
    const Version TheVersion = kOracleVersions[VersionIndex];
    for (const UpstreamOpcodeDefinition &Definition :
         UpstreamOpcodeDefinitions) {
      if (!versionInMask(TheVersion, Definition.Versions))
        continue;
      ASSERT_LT(ExpectedIndex, Cases.size());
      EXPECT_EQ(Cases[ExpectedIndex].TheVersion, TheVersion);
      const size_t OpcodeIndex = static_cast<size_t>(Definition.ID);
      ASSERT_LT(OpcodeIndex, Seen[VersionIndex].size());
      EXPECT_FALSE(Seen[VersionIndex][OpcodeIndex])
          << "duplicate execution pair " << Cases[ExpectedIndex].Name;
      Seen[VersionIndex][OpcodeIndex] = true;
      ++ExpectedIndex;
    }
  }
  ASSERT_EQ(ExpectedIndex, Cases.size());

  OracleProcessResult Process;
  auto Official = runOfficialExecutionBatch(*Executable, Cases, Process);
  ASSERT_TRUE(static_cast<bool>(Official))
      << (Official ? std::string() : llvm::toString(Official.takeError()))
      << "\nstdout:\n"
      << Process.StandardOutput << "\nstderr:\n"
      << Process.StandardError;
  ASSERT_EQ(Official->size(), Cases.size());

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    const ExecutionCase &Case = Cases[Index];
    const ExecutionObservation &Reference = (*Official)[Index];
    SCOPED_TRACE(::testing::Message()
                 << "index=" << Index << " case=" << Case.Name
                 << " version=" << versionName(Case.TheVersion).str()
                 << " target=" << Case.TargetSlot
                 << " text=" << llvm::toHex(Case.Text, true));
    ASSERT_EQ(Reference.Outcome, ExecutionOutcomeKind::Returned)
        << "official fault=" << executionFaultName(Reference.Fault).str();
    EXPECT_EQ(Reference.Fault, ExecutionFaultClass::None);
    EXPECT_EQ(Reference.TargetHits, Case.ExpectedTargetHits)
        << "the target opcode was not executed exactly once";
    EXPECT_EQ(Reference.SyscallCount, 0u);
    EXPECT_EQ(Reference.SyscallDigest, kSyscallDigestOffsetBasis);

    auto NeverD = runNeverDExecution(Case);
    ASSERT_TRUE(static_cast<bool>(NeverD))
        << (NeverD ? std::string() : llvm::toString(NeverD.takeError()));
    EXPECT_EQ(NeverD->Outcome, Reference.Outcome);
    EXPECT_EQ(NeverD->Fault, Reference.Fault)
        << "NeverD=" << executionFaultName(NeverD->Fault).str()
        << " official=" << executionFaultName(Reference.Fault).str();
    EXPECT_EQ(NeverD->Result, Reference.Result);
    EXPECT_EQ(NeverD->InstructionCount, Reference.InstructionCount);
    EXPECT_EQ(NeverD->TargetHits, Reference.TargetHits);
    EXPECT_EQ(NeverD->Input, Reference.Input);
    EXPECT_EQ(NeverD->SyscallCount, Reference.SyscallCount);
    EXPECT_EQ(NeverD->SyscallDigest, Reference.SyscallDigest);
  }
}

TEST(SBFExternalOracle,
     HighValueExecutionBoundariesMatchIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  const std::vector<ExecutionCase> Cases = boundaryExecutionCases();
  ASSERT_EQ(Cases.size(), kPinnedBoundaryExecutionCaseCount);
  ASSERT_EQ(kPinnedActiveOpcodeVersionPairCount + Cases.size(),
            kPinnedExecutionCaseCount);
  OracleProcessResult Process;
  auto Official = runOfficialExecutionBatch(*Executable, Cases, Process);
  ASSERT_TRUE(static_cast<bool>(Official))
      << (Official ? std::string() : llvm::toString(Official.takeError()))
      << "\nstdout:\n"
      << Process.StandardOutput << "\nstderr:\n"
      << Process.StandardError;
  ASSERT_EQ(Official->size(), Cases.size());

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    const ExecutionCase &Case = Cases[Index];
    const ExecutionObservation &Reference = (*Official)[Index];
    SCOPED_TRACE(::testing::Message()
                 << "index=" << Index << " case=" << Case.Name
                 << " version=" << versionName(Case.TheVersion).str()
                 << " target=" << Case.TargetSlot
                 << " text=" << llvm::toHex(Case.Text, true));
    ASSERT_TRUE(Case.ExpectedOutcome.has_value());
    ASSERT_TRUE(Case.ExpectedFault.has_value());
    EXPECT_EQ(Reference.Outcome, *Case.ExpectedOutcome);
    EXPECT_EQ(Reference.Fault, *Case.ExpectedFault)
        << "official=" << executionFaultName(Reference.Fault).str()
        << " expected=" << executionFaultName(*Case.ExpectedFault).str();
    EXPECT_EQ(Reference.TargetHits, Case.ExpectedTargetHits);
    if (Case.ExpectedResult)
      EXPECT_EQ(Reference.Result, *Case.ExpectedResult);
    const bool ExpectsSyscall = Case.Syscalls != ExecutionSyscallProfile::None;
    EXPECT_EQ(Reference.SyscallCount, ExpectsSyscall ? 1u : 0u);

    auto NeverD = runNeverDExecution(Case);
    ASSERT_TRUE(static_cast<bool>(NeverD))
        << (NeverD ? std::string() : llvm::toString(NeverD.takeError()));
    EXPECT_EQ(NeverD->Outcome, Reference.Outcome);
    EXPECT_EQ(NeverD->Fault, Reference.Fault)
        << "NeverD=" << executionFaultName(NeverD->Fault).str()
        << " official=" << executionFaultName(Reference.Fault).str();
    EXPECT_EQ(NeverD->Result, Reference.Result);
    EXPECT_EQ(NeverD->InstructionCount, Reference.InstructionCount);
    EXPECT_EQ(NeverD->TargetHits, Reference.TargetHits);
    EXPECT_EQ(NeverD->Input, Reference.Input);
    EXPECT_EQ(NeverD->SyscallCount, Reference.SyscallCount);
    EXPECT_EQ(NeverD->SyscallDigest, Reference.SyscallDigest);
  }
}

TEST(SBFExternalOracle,
     OpcodeAndVerifierAcceptanceMatchIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  for (Version TheVersion : kOracleVersions) {
    for (unsigned Encoding = 0; Encoding < kOpcodeEncodingCount; ++Encoding) {
      const size_t Matches = static_cast<size_t>(std::count_if(
          UpstreamOpcodeDefinitions.begin(), UpstreamOpcodeDefinitions.end(),
          [TheVersion, Encoding](const UpstreamOpcodeDefinition &Definition) {
            return Definition.Encoding == Encoding &&
                   versionInMask(TheVersion, Definition.Versions);
          }));
      ASSERT_LE(Matches, 1u)
          << versionName(TheVersion).str() << " encoding=0x" << std::hex
          << Encoding << " has an ambiguous upstream manifest";
    }
  }

  const std::vector<VerificationCase> Cases = verificationCases();
  ASSERT_EQ(Cases.size(), kPinnedVerificationCaseCount);

  OracleProcessResult Process;
  auto Official = runOfficialVerificationBatch(*Executable, Cases, Process);
  ASSERT_TRUE(static_cast<bool>(Official))
      << (Official ? std::string() : llvm::toString(Official.takeError()))
      << "\nstdout:\n"
      << Process.StandardOutput << "\nstderr:\n"
      << Process.StandardError;
  ASSERT_EQ(Official->size(), Cases.size());

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    const VerificationCase &Case = Cases[Index];
    SCOPED_TRACE(::testing::Message()
                 << "index=" << Index << " case=" << Case.Name
                 << " version=" << versionName(Case.TheVersion).str()
                 << " text=" << llvm::toHex(Case.Text, true));
    EXPECT_EQ((*Official)[Index], Case.Expected)
        << "official=" << acceptanceName((*Official)[Index]).str()
        << " expected=" << acceptanceName(Case.Expected).str();
    const AcceptanceExpectation NeverD = runNeverDVerification(Case);
    EXPECT_EQ(NeverD, (*Official)[Index])
        << "NeverD=" << acceptanceName(NeverD).str()
        << " official=" << acceptanceName((*Official)[Index]).str();
  }
}

TEST(SBFExternalOracle, StrictELFMutationsMatchIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  const std::vector<ELFMutationCase> Cases = elfMutationCases();
  ASSERT_EQ(Cases.size(), kPinnedStrictELFMutationCount);
  ASSERT_FALSE(Cases.empty());

  OracleProcessResult Process;
  auto Official = runOfficialELFVerificationBatch(*Executable, Cases, Process);
  ASSERT_TRUE(static_cast<bool>(Official))
      << (Official ? std::string() : llvm::toString(Official.takeError()))
      << "\nstdout:\n"
      << Process.StandardOutput << "\nstderr:\n"
      << Process.StandardError;
  ASSERT_EQ(Official->size(), Cases.size());

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    const ELFMutationCase &Case = Cases[Index];
    SCOPED_TRACE(::testing::Message()
                 << "index=" << Index << " case=" << Case.Name
                 << " bytes=" << Case.Bytes.size()
                 << " hex=" << llvm::toHex(Case.Bytes, true));
    EXPECT_EQ((*Official)[Index], Case.Expected)
        << "official=" << acceptanceName((*Official)[Index]).str()
        << " expected=" << acceptanceName(Case.Expected).str();
    const AcceptanceExpectation NeverD = runNeverDELFVerification(Case);
    EXPECT_EQ(NeverD, (*Official)[Index])
        << "NeverD=" << acceptanceName(NeverD).str()
        << " official=" << acceptanceName((*Official)[Index]).str();
  }
}

TEST(SBFExternalOracle, LegacyELFCasesMatchIndependentOfficialProcess) {
  const std::optional<std::string> Executable = oracleExecutable();
  if (!Executable) {
    if (officialOracleRequired()) {
      ADD_FAILURE() << "required official oracle executable is missing: set "
                    << kOracleExecutableEnvironment.str();
      return;
    }
    GTEST_SKIP() << "set " << kOracleExecutableEnvironment.str()
                 << " to enable the official out-of-process release gate";
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(*Executable));
  ASSERT_NO_FATAL_FAILURE(expectPinnedOracle(*Executable));

  const std::vector<ELFMutationCase> Cases = legacyELFCases();
  ASSERT_EQ(Cases.size(), LegacyELFCaseDefinitions.size());
  ASSERT_FALSE(Cases.empty());

  OracleProcessResult Process;
  auto Official = runOfficialELFVerificationBatch(*Executable, Cases, Process);
  ASSERT_TRUE(static_cast<bool>(Official))
      << (Official ? std::string() : llvm::toString(Official.takeError()))
      << "\nstdout:\n"
      << Process.StandardOutput << "\nstderr:\n"
      << Process.StandardError;
  ASSERT_EQ(Official->size(), Cases.size());

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    const ELFMutationCase &Case = Cases[Index];
    SCOPED_TRACE(::testing::Message()
                 << "index=" << Index << " case=" << Case.Name
                 << " bytes=" << Case.Bytes.size()
                 << " hex=" << llvm::toHex(Case.Bytes, true));
    EXPECT_EQ((*Official)[Index], Case.Expected)
        << "official=" << acceptanceName((*Official)[Index]).str()
        << " expected=" << acceptanceName(Case.Expected).str();
    const AcceptanceExpectation NeverD = runNeverDELFVerification(Case);
    EXPECT_EQ(NeverD, (*Official)[Index])
        << "NeverD=" << acceptanceName(NeverD).str()
        << " official=" << acceptanceName((*Official)[Index]).str();
  }
}

} // namespace
} // namespace neverd::sbf
