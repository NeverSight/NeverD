//===- SBFEmitterTests.cpp - Solana SBF source backend tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"
#include "neverd/sbf/emit/SBFSourceStatus.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

namespace neverd::sbf {
namespace {

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                          int16_t Offset = 0, int32_t Immediate = 0) {
  EncodedInstruction Bytes{};
  const OpcodeInfo *Info = getOpcodeInfo(ID);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[kOpcodeOffset] = Info->Encoding;
  Bytes[kRegisterByteOffset] =
      static_cast<uint8_t>((Src << kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Bytes.data() + kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Bytes.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Bytes;
}

BinaryImage makeImage(Version TheVersion,
                      std::initializer_list<EncodedInstruction> Instructions) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  for (const EncodedInstruction &Instruction : Instructions)
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

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(TheVersion);
  Meta.Version = TheVersion;
  Meta.StrictLayout = versionHasFeature(TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, Image.Raw.size()};
  Meta.TextVM = {kBytecodeStart, Image.Raw.size()};
  Image.SBF = Meta;
  return Image;
}

BinaryImage makeReducibleImage() {
  return makeImage(Version::V3,
                   {encode(Opcode::MOV64_IMM, 0, 0, 0, 0),
                    encode(Opcode::JEQ64_IMM, 1, 0, 1, 0),
                    encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
                    encode(Opcode::MOV64_IMM, 2, 0, 0, 0),
                    encode(Opcode::JGE64_IMM, 2, 0, 2, 3),
                    encode(Opcode::ADD64_IMM, 2, 0, 0, 1),
                    encode(Opcode::JA, 0, 0, -3),
                    encode(Opcode::ADD64_REG, 0, 2), encode(Opcode::EXIT)});
}

SBFProgram makeReturnProgram() {
  SBFProgram Program;
  Program.Low.TheVersion = Version::V3;
  Program.Low.TextAddress = kBytecodeStart;
  Program.Low.EntrySlot = 0;
  Program.Low.Instructions.resize(2);

  MedInstruction Move;
  Move.Slot = 0;
  Move.Address = kBytecodeStart;
  Move.SourceOpcode = Opcode::MOV64_IMM;
  Move.Op = Operation::Mov;
  Move.Form = OperandForm::DstImm;
  Move.Width = 64;
  Move.Dst = kReturnRegister;
  Move.Immediate = 7;
  Program.Med.Instructions.push_back(Move);

  MedInstruction Exit;
  Exit.Slot = 1;
  Exit.Address = kBytecodeStart + kInstructionSize;
  Exit.SourceOpcode = Opcode::EXIT;
  Exit.Op = Operation::Exit;
  Exit.Form = OperandForm::None;
  Program.Med.Instructions.push_back(Exit);
  return Program;
}

SBFProgram makeIndirectCallProgram(Version TheVersion) {
  SBFProgram Program;
  Program.Low.TheVersion = TheVersion;
  Program.Low.TextAddress = kBytecodeStart;
  Program.Low.EntrySlot = 0;
  Program.Low.Instructions.resize(3);

  MedInstruction Call;
  Call.Slot = 0;
  Call.Address = kBytecodeStart;
  Call.SourceOpcode = Opcode::CALL_REG;
  Call.Op = Operation::CallX;
  Call.Form = OperandForm::CallReg;
  Call.Width = 64;
  Call.Call = CallKind::Indirect;
  Call.CallRegister = 1;
  Program.Med.Instructions.push_back(Call);

  for (size_t Slot = 1; Slot < 3; ++Slot) {
    MedInstruction Exit;
    Exit.Slot = Slot;
    Exit.Address = kBytecodeStart + Slot * kInstructionSize;
    Exit.SourceOpcode = Opcode::EXIT;
    Exit.Op = Operation::Exit;
    Exit.Form = OperandForm::None;
    Program.Med.Instructions.push_back(Exit);
  }
  return Program;
}

class TemporaryFile {
public:
  explicit TemporaryFile(llvm::StringRef Extension,
                         llvm::StringRef Prefix = "neverd-sbf-emitter") {
    std::error_code Error =
        llvm::sys::fs::createTemporaryFile(Prefix, Extension, Path);
    EXPECT_FALSE(Error) << Error.message();
  }

  ~TemporaryFile() {
    if (!Path.empty())
      llvm::sys::fs::remove(Path);
  }

  llvm::StringRef str() const { return Path; }

private:
  llvm::SmallString<128> Path;
};

TEST(SBFSourceStatus, PreservesStableFaultMappings) {
#define SBF_FAULT_CODE(NAME, VALUE)                                            \
  EXPECT_EQ(static_cast<uint32_t>(FaultCode::NAME), uint32_t{VALUE});          \
  EXPECT_TRUE(isKnownFaultCodeValue(uint32_t{VALUE}));
#include "neverd/sbf/SBFFaultCodes.def"

#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  EXPECT_EQ(sourceStatusForFault(FaultCode::FAULT_CODE), SourceStatus::NAME);  \
  EXPECT_EQ(sourceStatusCode(SourceStatus::NAME), uint32_t{VALUE});            \
  EXPECT_EQ(cSourceStatusName(SourceStatus::NAME), #C_NAME);                   \
  EXPECT_TRUE(isKnownSourceStatusCode(uint32_t{VALUE}));
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  EXPECT_EQ(sourceStatusForFault(FaultCode::FAULT_CODE), SourceStatus::NAME);  \
  EXPECT_EQ(sourceStatusCode(SourceStatus::NAME), uint32_t{C_VALUE});          \
  EXPECT_EQ(cSourceStatusName(SourceStatus::NAME), #C_NAME);                   \
  EXPECT_EQ(rustSourceErrorName(SourceStatus::NAME), "SbfErrorV2::" #NAME);    \
  EXPECT_EQ(rustSourceErrorCode(SourceStatus::NAME), uint32_t{RUST_VALUE});    \
  EXPECT_TRUE(isKnownSourceStatusCode(uint32_t{C_VALUE}));
#include "neverd/sbf/emit/SBFSourceStatuses.def"

#define SBF_SOURCE_RUST_V1_ERROR(NAME)                                         \
  EXPECT_EQ(rustLegacySourceErrorName(SourceStatus::NAME), "SbfError:"         \
                                                           ":" #NAME);
#define SBF_SOURCE_RUST_V1_FALLBACK(NAME, LEGACY_NAME)                         \
  EXPECT_EQ(rustLegacySourceErrorName(SourceStatus::NAME),                     \
            "SbfError::" #LEGACY_NAME);
#include "neverd/sbf/emit/SBFSourceStatuses.def"

  EXPECT_FALSE(isKnownFaultCodeValue(std::numeric_limits<uint32_t>::max()));
  EXPECT_FALSE(isKnownSourceStatusCode(std::numeric_limits<uint32_t>::max()));
}

TEST(SBFCEmitter, EmitsExplicitStableStatusAssignments) {
  auto Source = emitC(makeReturnProgram());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("typedef enum neverd_sbf_status {"),
            std::string::npos);
  const size_t LegacyStatusEnd = Source->find("} neverd_sbf_status;");
  ASSERT_NE(LegacyStatusEnd, std::string::npos);
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)                    \
  EXPECT_NE(Source->find(std::string("  " #C_NAME " = ") +                     \
                         std::to_string(VALUE) + ","),                         \
            std::string::npos);
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  EXPECT_NE(Source->find(std::string("  " #C_NAME " = ") +                     \
                         std::to_string(C_VALUE) + ","),                       \
            std::string::npos);
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  EXPECT_NE(Source->find("typedef uint32_t neverd_sbf_status_v2;"),
            std::string::npos);
#define SBF_SOURCE_C_V1_ERROR(NAME)                                            \
  EXPECT_LT(Source->find(cSourceStatusName(SourceStatus::NAME).str()),         \
            LegacyStatusEnd);
#define SBF_SOURCE_C_V1_FALLBACK(NAME, LEGACY_NAME)                            \
  EXPECT_GT(Source->find(cSourceStatusName(SourceStatus::NAME).str()),         \
            LegacyStatusEnd);
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  EXPECT_EQ(Source->find("nd_runtime_status"), std::string::npos);

  auto RuntimeProgram = analyze(makeImage(
      Version::V3, {encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(RuntimeProgram))
      << llvm::toString(RuntimeProgram.takeError());
  auto RuntimeSource = emitC(*RuntimeProgram);
  ASSERT_TRUE(static_cast<bool>(RuntimeSource))
      << llvm::toString(RuntimeSource.takeError());
  EXPECT_NE(RuntimeSource->find("default: return invalid_status;"),
            std::string::npos);
}

TEST(SBFRustEmitter, AppendsErrorVariants) {
  auto Source = emitRust(makeReturnProgram());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("pub enum SbfError { InvalidInstruction, "
                         "MemoryAccess, DivideByZero, DivideOverflow, "
                         "CallDepth, UnknownSyscall, UnknownFunction, "
                         "ExecutionOverrun }"),
            std::string::npos);
  EXPECT_NE(Source->find("#[repr(u32)]\n#[non_exhaustive]\n"
                         "#[derive(Clone, Copy, Debug, Eq, PartialEq)]\n"
                         "pub enum SbfErrorV2"),
            std::string::npos);
#define SBF_SOURCE_SUCCESS(NAME, FAULT_CODE, C_NAME, VALUE)
#define SBF_SOURCE_ERROR(NAME, FAULT_CODE, C_NAME, C_VALUE, RUST_VALUE)        \
  EXPECT_NE(                                                                   \
      Source->find(std::string(#NAME " = ") + std::to_string(RUST_VALUE)),     \
      std::string::npos);
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  EXPECT_NE(Source->find("Variants and discriminants are append-only"),
            std::string::npos);
}

TEST(SBFCEmitter, PreservesTheOriginalEnvironmentAcrossTranslationUnits) {
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    GTEST_SKIP() << "clang is not available";

  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  auto Program =
      analyze(makeImage(Version::V3, {encode(Opcode::CALL_IMM, 0, 0, 0,
                                             static_cast<int32_t>(Log64->Hash)),
                                      encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Generated = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Generated))
      << llvm::toString(Generated.takeError());

  std::string Harness = R"(
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION,
  NEVERD_SBF_MEMORY_ACCESS,
  NEVERD_SBF_DIVIDE_BY_ZERO,
  NEVERD_SBF_DIVIDE_OVERFLOW,
  NEVERD_SBF_CALL_DEPTH,
  NEVERD_SBF_UNKNOWN_SYSCALL,
  NEVERD_SBF_UNKNOWN_FUNCTION,
  NEVERD_SBF_EXECUTION_OVERRUN
} neverd_sbf_status;
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t, uint32_t, uint64_t *);
  int (*store)(void *, uint64_t, uint32_t, uint64_t);
  int (*syscall)(void *, uint32_t)";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    Harness += ", uint64_t";
  Harness += R"(, uint64_t *);
} neverd_sbf_environment;

extern neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *, uint64_t, uint64_t, uint64_t *);

struct old_environment_with_canary {
  neverd_sbf_environment environment;
  uintptr_t forbidden_extension[2];
};

static int legacy_syscall(void *context, uint32_t hash)";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    Harness += ", uint64_t a" + std::to_string(Index);
  Harness += R"(, uint64_t *value) {
  (void)context;
  (void)hash;
)";
  for (unsigned Index = 1; Index < kArgumentRegisterCount; ++Index)
    Harness += "  (void)a" + std::to_string(Index) + ";\n";
  Harness += R"(  *value = a0 + UINT64_C(1);
  return 0;
}

int main(void) {
  struct old_environment_with_canary wrapper;
  uint64_t result = 0;
  wrapper.environment.context = 0;
  wrapper.environment.load = 0;
  wrapper.environment.store = 0;
  wrapper.environment.syscall = legacy_syscall;
  wrapper.forbidden_extension[0] = UINTPTR_MAX;
  wrapper.forbidden_extension[1] = UINTPTR_MAX;
  if (neverd_sbf_program(&wrapper.environment, UINT64_C(41), 0, &result) != 0)
    return 1;
  return result == UINT64_C(42) ? 0 : 2;
}
)";

  TemporaryFile GeneratedFile("c");
  TemporaryFile HarnessFile("c");
  TemporaryFile GeneratedObject("o");
  TemporaryFile HarnessObject("o");
  TemporaryFile Executable("out");
  {
    std::ofstream Output(GeneratedFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << *Generated;
  }
  {
    std::ofstream Output(HarnessFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << Harness;
  }

  const auto Compile = [&](llvm::StringRef Source, llvm::StringRef Object) {
    llvm::SmallVector<llvm::StringRef, 12> Arguments{
        *Clang, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-c",   Source,     "-o",    Object};
    std::string Error;
    const int Result = llvm::sys::ExecuteAndWait(
        *Clang, Arguments, std::nullopt, {}, 0, 0, &Error);
    EXPECT_EQ(Result, 0) << Error;
    return Result == 0;
  };
  ASSERT_TRUE(Compile(GeneratedFile.str(), GeneratedObject.str()));
  ASSERT_TRUE(Compile(HarnessFile.str(), HarnessObject.str()));

  llvm::SmallVector<llvm::StringRef, 8> LinkArguments{
      *Clang, GeneratedObject.str(), HarnessObject.str(), "-o",
      Executable.str()};
  std::string LinkError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Clang, LinkArguments, std::nullopt, {},
                                      0, 0, &LinkError),
            0)
      << LinkError;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

TEST(SBFCEmitter, SeparatesLegacyAndExactCallbackStatusDomains) {
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    GTEST_SKIP() << "clang is not available";

  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  auto CallbackProgram = analyze(makeImage(
      Version::V3,
      {encode(Opcode::LD_DW_REG, 2, 1), encode(Opcode::ST_DW_REG, 1, 2),
       encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(CallbackProgram))
      << llvm::toString(CallbackProgram.takeError());
  auto CallbackSource = emitC(*CallbackProgram);
  ASSERT_TRUE(static_cast<bool>(CallbackSource))
      << llvm::toString(CallbackSource.takeError());

  SBFProgram InvalidBranchProgram = makeReturnProgram();
  MedInstruction &InvalidBranch = InvalidBranchProgram.Med.Instructions.front();
  InvalidBranch.SourceOpcode = Opcode::JA;
  InvalidBranch.Op = Operation::Jump;
  InvalidBranch.Form = OperandForm::Branch;
  InvalidBranch.BranchTarget.reset();
  CEmitterOptions InvalidBranchOptions;
  InvalidBranchOptions.FunctionName = "neverd_sbf_invalid_branch";
  InvalidBranchOptions.PreferStructuredControlFlow = false;
  auto InvalidBranchSource = emitC(InvalidBranchProgram, InvalidBranchOptions);
  ASSERT_TRUE(static_cast<bool>(InvalidBranchSource))
      << llvm::toString(InvalidBranchSource.takeError());

  SBFProgram CollisionProgram;
  CollisionProgram.Low.TheVersion = Version::V0;
  CollisionProgram.Low.TextAddress = kBytecodeStart;
  CollisionProgram.Low.EntrySlot = 0;
  CollisionProgram.Low.Instructions.resize(4);
  MedInstruction CollisionCall;
  CollisionCall.Slot = 0;
  CollisionCall.Address = kBytecodeStart;
  CollisionCall.SourceOpcode = Opcode::CALL_IMM;
  CollisionCall.Op = Operation::Call;
  CollisionCall.Form = OperandForm::CallImm;
  CollisionCall.Call = CallKind::Internal;
  CollisionCall.CallTarget = 2;
  CollisionCall.SyscallHash = Log64->Hash;
  CollisionCall.Dispatch = CallDispatchPolicy::LegacyRuntimeThenFunction;
  CollisionProgram.Med.Instructions.push_back(CollisionCall);
  MedInstruction CallerExit;
  CallerExit.Slot = 1;
  CallerExit.Address = kBytecodeStart + kInstructionSize;
  CallerExit.SourceOpcode = Opcode::EXIT;
  CallerExit.Op = Operation::Exit;
  CallerExit.Form = OperandForm::None;
  CollisionProgram.Med.Instructions.push_back(CallerExit);
  MedInstruction CollisionResult;
  CollisionResult.Slot = 2;
  CollisionResult.Address = kBytecodeStart + 2 * kInstructionSize;
  CollisionResult.SourceOpcode = Opcode::MOV64_IMM;
  CollisionResult.Op = Operation::Mov;
  CollisionResult.Form = OperandForm::DstImm;
  CollisionResult.Width = kDoubleWordBitWidth;
  CollisionResult.Dst = kReturnRegister;
  CollisionResult.Immediate = 7;
  CollisionProgram.Med.Instructions.push_back(CollisionResult);
  MedInstruction CalleeExit = CallerExit;
  CalleeExit.Slot = 3;
  CalleeExit.Address = kBytecodeStart + 3 * kInstructionSize;
  CollisionProgram.Med.Instructions.push_back(CalleeExit);
  CEmitterOptions CollisionOptions;
  CollisionOptions.FunctionName = "neverd_sbf_collision";
  CollisionOptions.PreferStructuredControlFlow = false;
  auto CollisionSource = emitC(CollisionProgram, CollisionOptions);
  ASSERT_TRUE(static_cast<bool>(CollisionSource))
      << llvm::toString(CollisionSource.takeError());

  std::string Harness = R"(
#include <stdint.h>

typedef enum neverd_sbf_status {
  NEVERD_SBF_OK = 0,
  NEVERD_SBF_INVALID_INSTRUCTION,
  NEVERD_SBF_MEMORY_ACCESS,
  NEVERD_SBF_DIVIDE_BY_ZERO,
  NEVERD_SBF_DIVIDE_OVERFLOW,
  NEVERD_SBF_CALL_DEPTH,
  NEVERD_SBF_UNKNOWN_SYSCALL,
  NEVERD_SBF_UNKNOWN_FUNCTION,
  NEVERD_SBF_EXECUTION_OVERRUN
} neverd_sbf_status;
typedef uint32_t neverd_sbf_status_v2;
typedef struct neverd_sbf_syscall_invocation
    neverd_sbf_syscall_invocation;
typedef struct neverd_sbf_runtime_features neverd_sbf_runtime_features;
typedef struct neverd_sbf_environment {
  void *context;
  int (*load)(void *, uint64_t, uint32_t, uint64_t *);
  int (*store)(void *, uint64_t, uint32_t, uint64_t);
  int (*syscall)(void *, uint32_t)";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    Harness += ", uint64_t";
  Harness += R"(, uint64_t *);
} neverd_sbf_environment;
typedef struct neverd_sbf_environment_v2 {
  neverd_sbf_environment base;
  int (*syscall_with_features)(
      void *, const neverd_sbf_syscall_invocation *, uint64_t *);
  const neverd_sbf_runtime_features *runtime_features;
} neverd_sbf_environment_v2;

extern neverd_sbf_status neverd_sbf_program(
    neverd_sbf_environment *, uint64_t, uint64_t, uint64_t *);
extern neverd_sbf_status_v2 neverd_sbf_program_v2(
    neverd_sbf_environment_v2 *, uint64_t, uint64_t, uint64_t *);
extern neverd_sbf_status neverd_sbf_invalid_branch(
    neverd_sbf_environment *, uint64_t, uint64_t, uint64_t *);
extern neverd_sbf_status_v2 neverd_sbf_invalid_branch_v2(
    neverd_sbf_environment_v2 *, uint64_t, uint64_t, uint64_t *);
extern neverd_sbf_status neverd_sbf_collision(
    neverd_sbf_environment *, uint64_t, uint64_t, uint64_t *);
extern neverd_sbf_status_v2 neverd_sbf_collision_v2(
    neverd_sbf_environment_v2 *, uint64_t, uint64_t, uint64_t *);

static unsigned failure_stage;

static int load(void *context, uint64_t address, uint32_t width,
                uint64_t *value) {
  (void)context;
  (void)address;
  (void)width;
  *value = 0;
  return failure_stage == 1;
}

static int store(void *context, uint64_t address, uint32_t width,
                 uint64_t value) {
  (void)context;
  (void)address;
  (void)width;
  (void)value;
  return failure_stage == 2;
}

static int syscall(void *context, uint32_t hash)";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    Harness += ", uint64_t a" + std::to_string(Index);
  Harness += R"(, uint64_t *value) {
  (void)context;
  (void)hash;
)";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    Harness += "  (void)a" + std::to_string(Index) + ";\n";
  Harness += R"(  *value = 0;
  return failure_stage == 3;
}

int main(void) {
  neverd_sbf_environment legacy = {0, load, store, syscall};
  neverd_sbf_environment_v2 exact = {{0, load, store, syscall}, 0, 0};
  uint64_t result = 0;

  failure_stage = 1;
  if (neverd_sbf_program(&legacy, 0, 0, &result) !=
      NEVERD_SBF_MEMORY_ACCESS)
    return 1;
  failure_stage = 2;
  if (neverd_sbf_program(&legacy, 0, 0, &result) !=
      NEVERD_SBF_MEMORY_ACCESS)
    return 2;
  failure_stage = 3;
  if (neverd_sbf_program(&legacy, 0, 0, &result) !=
      NEVERD_SBF_UNKNOWN_SYSCALL)
    return 3;

  failure_stage = 1;
  if (neverd_sbf_program_v2(&exact, 0, 0, &result) !=
      NEVERD_SBF_INVALID_INSTRUCTION)
    return 4;
  failure_stage = 2;
  if (neverd_sbf_program_v2(&exact, 0, 0, &result) !=
      NEVERD_SBF_INVALID_INSTRUCTION)
    return 5;
  failure_stage = 3;
  if (neverd_sbf_program_v2(&exact, 0, 0, &result) !=
      NEVERD_SBF_INVALID_INSTRUCTION)
    return 6;

  failure_stage = 0;
  if (neverd_sbf_invalid_branch(&legacy, 0, 0, &result) !=
      NEVERD_SBF_INVALID_INSTRUCTION)
    return 7;
  if (neverd_sbf_invalid_branch_v2(&exact, 0, 0, &result) != UINT32_C(10))
    return 8;
  failure_stage = 3;
  result = 0;
  if (neverd_sbf_collision(&legacy, 0, 0, &result) != NEVERD_SBF_OK ||
      result != UINT64_C(7))
    return 9;
  if (neverd_sbf_collision_v2(&exact, 0, 0, &result) !=
      NEVERD_SBF_INVALID_INSTRUCTION)
    return 10;
  return 0;
}
)";

  TemporaryFile CallbackFile("c");
  TemporaryFile InvalidBranchFile("c");
  TemporaryFile CollisionFile("c");
  TemporaryFile HarnessFile("c");
  TemporaryFile CallbackObject("o");
  TemporaryFile InvalidBranchObject("o");
  TemporaryFile CollisionObject("o");
  TemporaryFile HarnessObject("o");
  TemporaryFile Executable("out");
  const auto Write = [](llvm::StringRef Path, llvm::StringRef Contents) {
    std::ofstream Output(Path.str(), std::ios::binary);
    EXPECT_TRUE(Output);
    Output << Contents.str();
    return static_cast<bool>(Output);
  };
  ASSERT_TRUE(Write(CallbackFile.str(), *CallbackSource));
  ASSERT_TRUE(Write(InvalidBranchFile.str(), *InvalidBranchSource));
  ASSERT_TRUE(Write(CollisionFile.str(), *CollisionSource));
  ASSERT_TRUE(Write(HarnessFile.str(), Harness));

  const auto Compile = [&](llvm::StringRef Source, llvm::StringRef Object) {
    llvm::SmallVector<llvm::StringRef, 12> Arguments{
        *Clang, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-c",   Source,     "-o",    Object};
    std::string Error;
    const int Result = llvm::sys::ExecuteAndWait(
        *Clang, Arguments, std::nullopt, {}, 0, 0, &Error);
    EXPECT_EQ(Result, 0) << Error;
    return Result == 0;
  };
  ASSERT_TRUE(Compile(CallbackFile.str(), CallbackObject.str()));
  ASSERT_TRUE(Compile(InvalidBranchFile.str(), InvalidBranchObject.str()));
  ASSERT_TRUE(Compile(CollisionFile.str(), CollisionObject.str()));
  ASSERT_TRUE(Compile(HarnessFile.str(), HarnessObject.str()));

  llvm::SmallVector<llvm::StringRef, 12> LinkArguments{
      *Clang,
      CallbackObject.str(),
      InvalidBranchObject.str(),
      CollisionObject.str(),
      HarnessObject.str(),
      "-o",
      Executable.str()};
  std::string LinkError;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Clang, LinkArguments, std::nullopt, {},
                                      0, 0, &LinkError),
            0)
      << LinkError;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

TEST(SBFSourceEmitters, DefaultOptionsFallBackForStaticCallFaults) {
  const EncodedInstruction Continuation{};
  auto ContinuationProgram = analyze(makeImage(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::LDDW),
                    Continuation, encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(ContinuationProgram))
      << llvm::toString(ContinuationProgram.takeError());

  auto C = emitC(*ContinuationProgram);
  ASSERT_TRUE(static_cast<bool>(C)) << llvm::toString(C.takeError());
  EXPECT_NE(C->find("switch (pc)"), std::string::npos);
  EXPECT_NE(C->find("case 2: return NEVERD_SBF_INVALID_INSTRUCTION;"),
            std::string::npos);
  const size_t CFrame = C->find("return_pc[depth] = 1;");
  ASSERT_NE(CFrame, std::string::npos);
  const size_t CTarget = C->find("pc = 2; continue;", CFrame);
  EXPECT_NE(CTarget, std::string::npos);

  auto Rust = emitRust(*ContinuationProgram);
  ASSERT_TRUE(static_cast<bool>(Rust)) << llvm::toString(Rust.takeError());
  EXPECT_NE(Rust->find("match pc"), std::string::npos);
  EXPECT_NE(Rust->find("2 => return Err(SbfErrorV2::InvalidInstruction),"),
            std::string::npos);
  const size_t RustFrame = Rust->find("return_pc[depth] = 1;");
  ASSERT_NE(RustFrame, std::string::npos);
  const size_t RustTarget = Rust->find("pc = 2;", RustFrame);
  EXPECT_NE(RustTarget, std::string::npos);

  auto UnsupportedProgram = analyze(makeImage(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 2), encode(Opcode::EXIT)}));
  ASSERT_TRUE(static_cast<bool>(UnsupportedProgram))
      << llvm::toString(UnsupportedProgram.takeError());
  C = emitC(*UnsupportedProgram);
  ASSERT_TRUE(static_cast<bool>(C)) << llvm::toString(C.takeError());
  const size_t CUnsupported = C->find("case 0:");
  const size_t CInvalid =
      C->find("return NEVERD_SBF_INVALID_INSTRUCTION;", CUnsupported);
  const size_t CNext = C->find("case 1:", CUnsupported);
  ASSERT_NE(CUnsupported, std::string::npos);
  ASSERT_NE(CInvalid, std::string::npos);
  ASSERT_NE(CNext, std::string::npos);
  EXPECT_LT(CInvalid, CNext);

  Rust = emitRust(*UnsupportedProgram);
  ASSERT_TRUE(static_cast<bool>(Rust)) << llvm::toString(Rust.takeError());
  const size_t RustUnsupported = Rust->find("0 => {");
  const size_t RustInvalid = Rust->find(
      "return Err(SbfErrorV2::InvalidInstruction);", RustUnsupported);
  const size_t RustNext = Rust->find("1 => {", RustUnsupported);
  ASSERT_NE(RustUnsupported, std::string::npos);
  ASSERT_NE(RustInvalid, std::string::npos);
  ASSERT_NE(RustNext, std::string::npos);
  EXPECT_LT(RustInvalid, RustNext);
}

TEST(SBFCEmitter, MinimalProgramCompilesWithoutWarnings) {
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    GTEST_SKIP() << "clang is not available";

  auto Source = emitC(makeReturnProgram());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("(void)result;"), std::string::npos);

  TemporaryFile SourceFile("c");
  TemporaryFile ObjectFile("o");
  ASSERT_FALSE(SourceFile.str().empty());
  ASSERT_FALSE(ObjectFile.str().empty());
  {
    std::ofstream Output(SourceFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << *Source;
  }

  llvm::SmallVector<llvm::StringRef, 10> Arguments{
      *Clang, "-std=c11",       "-Wall", "-Wextra",       "-Werror",
      "-c",   SourceFile.str(), "-o",    ObjectFile.str()};
  std::string Error;
  const int ExitCode = llvm::sys::ExecuteAndWait(
      *Clang, Arguments, std::nullopt, {}, 0, 0, &Error);
  EXPECT_EQ(ExitCode, 0) << Error;
}

TEST(SBFRustEmitter, MinimalProgramCompilesWithoutWarnings) {
  auto Rustc = llvm::sys::findProgramByName("rustc");
  if (!Rustc)
    GTEST_SKIP() << "rustc is not available";

  auto Source = emitRust(makeReturnProgram());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());

  TemporaryFile SourceFile("rs");
  TemporaryFile LibraryFile("rlib", "libneverd_generated");
  ASSERT_FALSE(SourceFile.str().empty());
  ASSERT_FALSE(LibraryFile.str().empty());
  {
    std::ofstream Output(SourceFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << *Source;
  }

  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      *Rustc, "--edition=2021", "--crate-type=lib",
      "-D",   "warnings",       SourceFile.str(),
      "-o",   LibraryFile.str()};
  std::string Error;
  const int ExitCode = llvm::sys::ExecuteAndWait(
      *Rustc, Arguments, std::nullopt, {}, 0, 0, &Error);
  EXPECT_EQ(ExitCode, 0) << Error;
}

TEST(SBFRustEmitter, PreservesTheOriginalRustAPIForDownstreamCrates) {
  auto Rustc = llvm::sys::findProgramByName("rustc");
  if (!Rustc)
    GTEST_SKIP() << "rustc is not available";

  auto Generated = emitRust(makeReturnProgram());
  ASSERT_TRUE(static_cast<bool>(Generated))
      << llvm::toString(Generated.takeError());

  std::string Host = R"(
extern crate neverd_generated;
use neverd_generated::{
    neverd_sbf_program, neverd_sbf_program_v2, SbfEnvironment,
    SbfEnvironmentV2, SbfError, SbfErrorV2, SbfSyscallInvocation,
    SbfSyscallOutcomeV2,
};

struct Env;
impl SbfEnvironment for Env {
    fn load(&mut self, _address: u64, _width: u8) -> Result<u64, SbfError> {
        Err(SbfError::MemoryAccess)
    }
    fn store(&mut self, _address: u64, _width: u8, _value: u64)
        -> Result<(), SbfError> {
        Err(SbfError::MemoryAccess)
    }
)";
  Host += "    fn syscall(&mut self, _hash: u32, _args: [u64; " +
          std::to_string(kArgumentRegisterCount);
  Host += R"(])
        -> Result<u64, SbfError> {
        Err(SbfError::UnknownSyscall)
    }
}

struct FeatureEnv;
impl SbfEnvironmentV2 for FeatureEnv {
    fn load(&mut self, _address: u64, _width: u8) -> Result<u64, SbfErrorV2> {
        Err(SbfErrorV2::MemoryAccess)
    }
    fn store(&mut self, _address: u64, _width: u8, _value: u64)
        -> Result<(), SbfErrorV2> {
        Err(SbfErrorV2::MemoryAccess)
    }
    fn syscall_with_features(
        &mut self, _invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        SbfSyscallOutcomeV2::Unregistered
    }
}

fn classify(error: SbfError) -> u32 {
    match error {
)";
#define SBF_SOURCE_RUST_V1_ERROR(NAME)                                         \
  Host += "        SbfError::" #NAME " => " +                                  \
          std::to_string(rustSourceErrorCode(SourceStatus::NAME)) + "u32,\n";
#include "neverd/sbf/emit/SBFSourceStatuses.def"
  Host += R"(    }
}

fn main() {
    let mut env = Env;
    assert_eq!(neverd_sbf_program(&mut env, 0, 0), Ok(7));
    let mut feature_env = FeatureEnv;
    assert_eq!(neverd_sbf_program_v2(&mut feature_env, 0, 0), Ok(7));
    assert_eq!(classify(SbfError::ExecutionOverrun), 7);
}
)";

  TemporaryFile GeneratedFile("rs");
  TemporaryFile LibraryFile("rlib", "libneverd_generated");
  TemporaryFile HostFile("rs");
  TemporaryFile Executable("out");
  {
    std::ofstream Output(GeneratedFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << *Generated;
  }
  {
    std::ofstream Output(HostFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << Host;
  }

  llvm::SmallVector<llvm::StringRef, 16> LibraryArguments{
      *Rustc,
      "--edition=2021",
      "--crate-type=lib",
      "--crate-name=neverd_generated",
      "-D",
      "warnings",
      GeneratedFile.str(),
      "-o",
      LibraryFile.str()};
  std::string Error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Rustc, LibraryArguments, std::nullopt,
                                      {}, 0, 0, &Error),
            0)
      << Error;

  const std::string External = "neverd_generated=" + LibraryFile.str().str();
  llvm::SmallVector<llvm::StringRef, 16> HostArguments{
      *Rustc,   "--edition=2021", "-D", "warnings",      "--extern",
      External, HostFile.str(),   "-o", Executable.str()};
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Rustc, HostArguments, std::nullopt, {},
                                      0, 0, &Error),
            0)
      << Error;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

TEST(SBFCEmitter, MatchesCurrentCallFrameAndCallXSemantics) {
  auto Source = emitC(makeIndirectCallProgram(Version::V2));
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("depth + 1 >= NEVERD_SBF_MAX_CALL_DEPTH"),
            std::string::npos);
  EXPECT_NE(Source->find("NEVERD_SBF_STACK_START + NEVERD_SBF_STACK_SIZE"),
            std::string::npos);
  EXPECT_EQ(Source->find("& 7u"), std::string::npos);
  const size_t BoundsCheck =
      Source->find("target_slot >= (uint64_t)NEVERD_SBF_INSTRUCTION_COUNT");
  const size_t Narrowing = Source->find("pc = (uint32_t)target_slot;");
  ASSERT_NE(BoundsCheck, std::string::npos);
  ASSERT_NE(Narrowing, std::string::npos);
  EXPECT_LT(BoundsCheck, Narrowing);
}

TEST(SBFRustEmitter, MatchesCurrentCallFrameAndCallXSemantics) {
  auto Source = emitRust(makeIndirectCallProgram(Version::V2));
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("depth + 1 >= MAX_CALL_DEPTH"), std::string::npos);
  EXPECT_NE(Source->find("STACK_START + STACK_SIZE"), std::string::npos);
  EXPECT_EQ(Source->find("% INSTRUCTION_SIZE"), std::string::npos);
  const size_t BoundsCheck =
      Source->find("target_slot >= INSTRUCTION_COUNT as u64");
  const size_t Narrowing = Source->find("pc = target_slot as usize;");
  ASSERT_NE(BoundsCheck, std::string::npos);
  ASSERT_NE(Narrowing, std::string::npos);
  EXPECT_LT(BoundsCheck, Narrowing);
}

TEST(SBFSourceEmitters, RejectInvalidVMConfiguration) {
  SBFProgram Program = makeReturnProgram();
  Program.Config.MaxCallDepth = 0;
  auto C = emitC(Program);
  ASSERT_FALSE(static_cast<bool>(C));
  EXPECT_NE(llvm::toString(C.takeError()).find("call depth"),
            std::string::npos);

  auto Rust = emitRust(Program);
  ASSERT_FALSE(static_cast<bool>(Rust));
  EXPECT_NE(llvm::toString(Rust.takeError()).find("call depth"),
            std::string::npos);
}

TEST(SBFSourceEmitters,
     RejectHostileVMResourcesBeforeMaterializingCallFrameArrays) {
  const auto ExpectBothReject = [](const SBFProgram &Program,
                                   llvm::StringRef ExpectedMessage) {
    auto C = emitC(Program);
    ASSERT_FALSE(static_cast<bool>(C));
    const std::string CError = llvm::toString(C.takeError());
    EXPECT_NE(CError.find(ExpectedMessage.str()), std::string::npos);

    auto Rust = emitRust(Program);
    ASSERT_FALSE(static_cast<bool>(Rust));
    const std::string RustError = llvm::toString(Rust.takeError());
    EXPECT_EQ(RustError, CError);
  };

  SBFProgram ExcessiveDepth = makeReturnProgram();
  ExcessiveDepth.Config.StackFrameSize = 1;
  ExcessiveDepth.Config.MaxCallDepth = std::numeric_limits<uint32_t>::max();
  ExpectBothReject(ExcessiveDepth, "host call-depth limit");

  SBFProgram MultiGiBStack = makeReturnProgram();
  MultiGiBStack.Config.StackFrameSize =
      static_cast<size_t>(kMemoryRegionSize - 1);
  MultiGiBStack.Config.MaxCallDepth = 1;
  ExpectBothReject(MultiGiBStack, "host stack-byte limit");
}

TEST(SBFCEmitter, StructuresReducibleConditionAndNaturalLoop) {
  auto Program = analyze(makeReducibleImage());
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_GE(Program->High.Regions.size(), 2u);

  auto Source = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("if ("), std::string::npos);
  EXPECT_NE(Source->find("while (1)"), std::string::npos);
  EXPECT_EQ(Source->find("switch (pc)"), std::string::npos);
}

TEST(SBFRustEmitter, StructuresReducibleConditionAndNaturalLoop) {
  auto Program = analyze(makeReducibleImage());
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_GE(Program->High.Regions.size(), 2u);

  auto Source = emitRust(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("if "), std::string::npos);
  EXPECT_NE(Source->find("loop {"), std::string::npos);
  EXPECT_EQ(Source->find("match pc"), std::string::npos);
}

TEST(SBFCEmitter, StructuredProgramMatchesTheRawBytecodeOracle) {
  auto Clang = llvm::sys::findProgramByName("clang");
  if (!Clang)
    GTEST_SKIP() << "clang is not available";

  auto Program = analyze(makeReducibleImage());
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}}) {
    ExecutionEnvironment Environment;
    Environment.Input = Input;
    auto Result = executeRaw(*Program, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Status, ExecutionStatus::Returned);
    EXPECT_EQ(Result->ReturnValue, Input == 0 ? 3u : 4u);
  }

  auto Source = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  *Source += R"(
static int no_load(void *context, uint64_t address, uint32_t width,
                   uint64_t *value) {
  (void)context; (void)address; (void)width; (void)value; return 1;
}
static int no_store(void *context, uint64_t address, uint32_t width,
                    uint64_t value) {
  (void)context; (void)address; (void)width; (void)value; return 1;
}
)";
  *Source += "static int no_syscall(void *context, uint32_t hash";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    *Source += ", uint64_t a" + std::to_string(kFirstArgumentRegister + Index);
  *Source += ", uint64_t *value) {\n  (void)context; (void)hash;";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    *Source +=
        " (void)a" + std::to_string(kFirstArgumentRegister + Index) + ";";
  *Source += R"( (void)value; return 1;
}
int main(void) {
  neverd_sbf_environment env = {
      .context = 0,
      .load = no_load,
      .store = no_store,
      .syscall = no_syscall,
  };
  uint64_t result = 0;
  if (neverd_sbf_program(&env, 0, 0, &result) != NEVERD_SBF_OK || result != 3)
    return 1;
  if (neverd_sbf_program(&env, 1, 0, &result) != NEVERD_SBF_OK || result != 4)
    return 2;
  return 0;
}
)";

  TemporaryFile SourceFile("c");
  TemporaryFile Executable("out");
  {
    std::ofstream Output(SourceFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << *Source;
  }
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      *Clang,    "-std=c11",       "-Wall", "-Wextra",
      "-Werror", SourceFile.str(), "-o",    Executable.str()};
  std::string Error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Clang, Arguments, std::nullopt, {}, 0, 0,
                                      &Error),
            0)
      << Error;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

TEST(SBFRustEmitter, StructuredProgramMatchesTheRawBytecodeOracle) {
  auto Rustc = llvm::sys::findProgramByName("rustc");
  if (!Rustc)
    GTEST_SKIP() << "rustc is not available";

#ifdef _WIN32
  // Git for Windows also ships a link.exe, but it is the POSIX hard-link
  // utility rather than an MSVC-compatible linker.  Git Bash places it ahead
  // of the Visual Studio linker when CTest launches rustc, so select the
  // unambiguous COFF linker explicitly.
  auto Linker = llvm::sys::findProgramByName("lld-link");
  ASSERT_TRUE(static_cast<bool>(Linker)) << "lld-link is not available";
  std::string LinkerArgument = "linker=" + *Linker;
#endif

  auto Program = analyze(makeReducibleImage());
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitRust(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  *Source += R"(
struct Env;
impl SbfEnvironment for Env {
    fn load(&mut self, _address: u64, _width: u8) -> Result<u64, SbfError> {
        Err(SbfError::MemoryAccess)
    }
    fn store(&mut self, _address: u64, _width: u8, _value: u64)
        -> Result<(), SbfError> {
        Err(SbfError::MemoryAccess)
    }
)";
  *Source += "    fn syscall(&mut self, _hash: u32, _args: [u64; " +
             std::to_string(kArgumentRegisterCount) + R"(])
        -> Result<u64, SbfError> {
        Err(SbfError::UnknownSyscall)
    }
}
fn main() {
    let mut env = Env;
    assert_eq!(neverd_sbf_program(&mut env, 0, 0), Ok(3));
    assert_eq!(neverd_sbf_program(&mut env, 1, 0), Ok(4));
}
)";

  TemporaryFile SourceFile("rs");
  TemporaryFile Executable("out");
  {
    std::ofstream Output(SourceFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << *Source;
  }
  llvm::SmallVector<llvm::StringRef, 16> Arguments{
      *Rustc, "--edition=2021", "--crate-name=neverd_sbf_generated", "-D",
      "warnings"};
#ifdef _WIN32
  Arguments.append({"-C", LinkerArgument});
#endif
  Arguments.append({SourceFile.str(), "-o", Executable.str()});
  std::string Error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Rustc, Arguments, std::nullopt, {}, 0, 0,
                                      &Error),
            0)
      << Error;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

} // namespace
} // namespace neverd::sbf
