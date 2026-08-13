//===- SBFEmitterTests.cpp - Solana SBF source backend tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFCEmitter.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/sbf/emit/SBFRustEmitter.h"

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
  explicit TemporaryFile(llvm::StringRef Extension) {
    std::error_code Error = llvm::sys::fs::createTemporaryFile(
        "neverd-sbf-emitter", Extension, Path);
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
  TemporaryFile LibraryFile("rlib");
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

TEST(SBFCEmitter, MatchesCurrentCallFrameAndCallXSemantics) {
  auto Source = emitC(makeIndirectCallProgram(Version::V2));
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("depth + 1 >= NEVERD_SBF_MAX_CALL_DEPTH"),
            std::string::npos);
  EXPECT_NE(Source->find("NEVERD_SBF_STACK_START + NEVERD_SBF_STACK_SIZE"),
            std::string::npos);
  EXPECT_EQ(Source->find("& 7u"), std::string::npos);
  EXPECT_NE(Source->find("pc >= NEVERD_SBF_INSTRUCTION_COUNT"),
            std::string::npos);
}

TEST(SBFRustEmitter, MatchesCurrentCallFrameAndCallXSemantics) {
  auto Source = emitRust(makeIndirectCallProgram(Version::V2));
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("depth + 1 >= MAX_CALL_DEPTH"), std::string::npos);
  EXPECT_NE(Source->find("STACK_START + STACK_SIZE"), std::string::npos);
  EXPECT_EQ(Source->find("% INSTRUCTION_SIZE"), std::string::npos);
  EXPECT_NE(Source->find("pc >= INSTRUCTION_COUNT"), std::string::npos);
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
  neverd_sbf_environment env = {0, no_load, no_store, no_syscall};
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
