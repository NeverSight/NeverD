//===- SBFSemanticTests.cpp - Solana SBF semantic oracle tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/Interpreter.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

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

SBFProgram
analyzeProgram(Version TheVersion,
               std::initializer_list<EncodedInstruction> Instructions,
               size_t EntrySlot = 0) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart + EntrySlot * kInstructionSize;
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

  auto Program = analyze(Image);
  EXPECT_TRUE(static_cast<bool>(Program))
      << (Program ? std::string() : llvm::toString(Program.takeError()));
  return Program ? std::move(*Program) : SBFProgram{};
}

TEST(SBFInterpreter, ExecutesRawBytesIndependentlyOfMedIR) {
  SBFProgram Program =
      analyzeProgram(Version::V3, {encode(Opcode::MOV64_IMM, 0, 0, 0, 5),
                                   encode(Opcode::ADD64_IMM, 0, 0, 0, 7),
                                   encode(Opcode::JEQ64_IMM, 0, 0, 1, 12),
                                   encode(Opcode::MOV64_IMM, 0, 0, 0, 99),
                                   encode(Opcode::EXIT)});
  ASSERT_FALSE(Program.Med.Instructions.empty());
  Program.Med.Instructions.front().Immediate = 0xffff;

  auto Result = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
  EXPECT_EQ(Result->ReturnValue, 12u);
  ASSERT_EQ(Result->Trace.size(), 4u);
  EXPECT_EQ(Result->Trace[2].Slot, 2u);
  EXPECT_EQ(Result->Trace[2].Op, Opcode::JEQ64_IMM);
}

TEST(SBFInterpreter, ImplementsNonMonotonicV2ArithmeticSemantics) {
  SBFProgram Program = analyzeProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, 10),
                    encode(Opcode::SUB64_IMM, 1, 0, 0, 3),
                    encode(Opcode::MOV32_REG, 0, 1), encode(Opcode::EXIT)});

  auto Result = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
  EXPECT_EQ(Result->ReturnValue, UINT64_C(0xfffffffffffffff9));
}

TEST(SBFInterpreter, ImplementsV2HighMultiplySemantics) {
  SBFProgram Unsigned = analyzeProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, -1),
                    encode(Opcode::UHMUL64_IMM, 1, 0, 0, 2),
                    encode(Opcode::MOV64_REG, 0, 1), encode(Opcode::EXIT)});
  auto UnsignedResult = executeRaw(Unsigned);
  ASSERT_TRUE(static_cast<bool>(UnsignedResult))
      << llvm::toString(UnsignedResult.takeError());
  EXPECT_EQ(UnsignedResult->Status, ExecutionStatus::Returned);
  EXPECT_EQ(UnsignedResult->ReturnValue, 1u);

  SBFProgram SignedPositive = analyzeProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, -1),
                    encode(Opcode::SHMUL64_IMM, 1, 0, 0, -1),
                    encode(Opcode::MOV64_REG, 0, 1), encode(Opcode::EXIT)});
  auto SignedPositiveResult = executeRaw(SignedPositive);
  ASSERT_TRUE(static_cast<bool>(SignedPositiveResult))
      << llvm::toString(SignedPositiveResult.takeError());
  EXPECT_EQ(SignedPositiveResult->Status, ExecutionStatus::Returned);
  EXPECT_EQ(SignedPositiveResult->ReturnValue, 0u);

  SBFProgram SignedNegative = analyzeProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                    encode(Opcode::LSH64_IMM, 1, 0, 0, 63),
                    encode(Opcode::SHMUL64_IMM, 1, 0, 0, 2),
                    encode(Opcode::MOV64_REG, 0, 1), encode(Opcode::EXIT)});
  auto SignedNegativeResult = executeRaw(SignedNegative);
  ASSERT_TRUE(static_cast<bool>(SignedNegativeResult))
      << llvm::toString(SignedNegativeResult.takeError());
  EXPECT_EQ(SignedNegativeResult->Status, ExecutionStatus::Returned);
  EXPECT_EQ(SignedNegativeResult->ReturnValue, UINT64_MAX);
}

TEST(SBFInterpreter, ModelsMemorySyscallsAndInternalCallFrames) {
  const SyscallInfo *Log64 = findSyscallByName("sol_log_64_");
  ASSERT_NE(Log64, nullptr);
  SBFProgram Program = analyzeProgram(
      Version::V3,
      {encode(Opcode::LD_DW_REG, 1, 1),
       encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::CALL_IMM, 0, 1, 0, 2),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 41), encode(Opcode::EXIT)});

  ExecutionEnvironment Environment;
  Environment.Input = kInputStart;
  Environment.Memory.push_back(
      {kInputStart, {9, 0, 0, 0, 0, 0, 0, 0}, false, "input"});
  Environment.Syscall = [](uint32_t, const SyscallArguments &Arguments)
      -> std::optional<uint64_t> { return Arguments[0] + 1; };

  auto Result = executeRaw(Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
  EXPECT_EQ(Result->ReturnValue, 42u);
  ASSERT_EQ(Result->Syscalls.size(), 1u);
  EXPECT_EQ(Result->Syscalls.front().Hash, Log64->Hash);
  EXPECT_EQ(Result->Syscalls.front().Arguments[0], 9u);
  EXPECT_EQ(Result->Syscalls.front().Result, 10u);
  EXPECT_EQ(Result->MaxCallDepth, 1u);
}

TEST(SBFInterpreter, ReportsRuntimeFaultsAndStepLimits) {
  SBFProgram DivideByZero = analyzeProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 1), encode(Opcode::MOV64_IMM, 2),
                    encode(Opcode::UDIV64_REG, 1, 2), encode(Opcode::EXIT)});
  auto DivideResult = executeRaw(DivideByZero);
  ASSERT_TRUE(static_cast<bool>(DivideResult))
      << llvm::toString(DivideResult.takeError());
  EXPECT_EQ(DivideResult->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(DivideResult->Fault, FaultCode::DivideByZero);

  SBFProgram BadMemory = analyzeProgram(
      Version::V3, {encode(Opcode::MOV64_IMM, 1),
                    encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)});
  auto MemoryResult = executeRaw(BadMemory);
  ASSERT_TRUE(static_cast<bool>(MemoryResult))
      << llvm::toString(MemoryResult.takeError());
  EXPECT_EQ(MemoryResult->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(MemoryResult->Fault, FaultCode::MemoryAccess);

  SBFProgram Loop = analyzeProgram(Version::V3, {encode(Opcode::JA, 0, 0, -1)});
  InterpreterOptions Options;
  Options.MaxSteps = 3;
  auto LoopResult = executeRaw(Loop, {}, Options);
  ASSERT_TRUE(static_cast<bool>(LoopResult))
      << llvm::toString(LoopResult.takeError());
  EXPECT_EQ(LoopResult->Status, ExecutionStatus::StepLimit);
  EXPECT_EQ(LoopResult->Steps, 3u);
}

TEST(SBFInterpreter, EnforcesTheV4AlignedMemoryMappingContract) {
  ExecutionEnvironment Environment;
  Environment.Memory.push_back({kInputStart, {1}, false, "input.first"});
  Environment.Memory.push_back(
      {kInputStart + kInstructionSize, {2}, false, "input.second"});

  SBFProgram V4 = analyzeProgram(Version::V4, {encode(Opcode::EXIT)});
  auto Rejected = executeRaw(V4, Environment);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("aligned index"),
            std::string::npos);

  SBFProgram V3 = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  auto Accepted = executeRaw(V3, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->Status, ExecutionStatus::Returned);
}

} // namespace
} // namespace neverd::sbf
