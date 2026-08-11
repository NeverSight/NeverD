//===- SBFLLVMDifferentialTests.cpp - SBF LLVM differential tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/Interpreter.h"
#include "neverd/sbf/LLVMEmitter.h"

#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

constexpr unsigned kBitsPerByte = 8;

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;
using JITFunction = uint64_t(void *, uint64_t);

struct JITSyscallTraceEntry {
  uint32_t Hash = 0;
  SyscallArguments Arguments{};
  uint64_t Result = 0;
};

struct JITEnvironment {
  uint64_t Input = kInputStart;
  std::vector<MemoryRegion> Memory;
  SyscallCallback Syscall;
  std::vector<JITSyscallTraceEntry> Syscalls;
  FaultCode Fault = FaultCode::None;
  uint64_t FaultAddress = 0;
};

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

std::array<EncodedInstruction, kLDDWSlotCount> encodeLDDW(uint8_t Dst,
                                                          uint64_t Immediate) {
  std::array<EncodedInstruction, 2> Result{
      encode(Opcode::LDDW, Dst, 0, 0, static_cast<int32_t>(Immediate)), {}};
  llvm::support::endian::write32le(
      Result[1].data() + kImmediateOffset,
      static_cast<uint32_t>(Immediate >>
                            std::numeric_limits<uint32_t>::digits));
  return Result;
}

llvm::Expected<SBFProgram>
analyzeProgram(Version TheVersion,
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
  return analyze(Image);
}

bool contains(const MemoryRegion &Region, uint64_t Address, size_t Size,
              size_t &Offset) {
  if (Address < Region.Address)
    return false;
  const uint64_t Delta = Address - Region.Address;
  if (Delta > Region.Bytes.size() || Size > Region.Bytes.size() - Delta)
    return false;
  Offset = static_cast<size_t>(Delta);
  return true;
}

void appendProgramMemory(const SBFProgram &Program,
                         JITEnvironment &Environment) {
  for (const ProgramRegion &Region : Program.ExecutableImage.regions())
    if (Region.DataVisible && !Region.Bytes.empty())
      Environment.Memory.push_back(
          {Region.Address, Region.Bytes, false, Region.Name});

  if (usesStackFrameGaps(Program.Low.TheVersion, Program.Config)) {
    for (size_t Frame = 0; Frame < Program.Config.MaxCallDepth; ++Frame) {
      MemoryRegion Stack;
      Stack.Address = kStackStart + Frame * Program.Config.StackFrameSize *
                                        kStackFrameGapMultiplier;
      Stack.Bytes.resize(Program.Config.StackFrameSize);
      Stack.Writable = true;
      Stack.Name = "stack." + std::to_string(Frame);
      Environment.Memory.push_back(std::move(Stack));
    }
    return;
  }
  Environment.Memory.push_back({kStackStart,
                                std::vector<uint8_t>(stackSize(Program.Config)),
                                true, "stack"});
}

extern "C" int32_t differentialLoad(void *Opaque, uint64_t Address,
                                    uint32_t Width, uint64_t *Result) {
  auto &Environment = *static_cast<JITEnvironment *>(Opaque);
  if (!Result || Width == 0 || Width % kBitsPerByte != 0)
    return 1;
  const size_t Size = Width / kBitsPerByte;
  if (Size > sizeof(uint64_t))
    return 1;
  for (const MemoryRegion &Region : Environment.Memory) {
    size_t Offset = 0;
    if (!contains(Region, Address, Size, Offset))
      continue;
    *Result = 0;
    for (size_t I = 0; I < Size; ++I)
      *Result |= static_cast<uint64_t>(Region.Bytes[Offset + I])
                 << (I * kBitsPerByte);
    return 0;
  }
  return 1;
}

extern "C" int32_t differentialStore(void *Opaque, uint64_t Address,
                                     uint32_t Width, uint64_t Value) {
  auto &Environment = *static_cast<JITEnvironment *>(Opaque);
  if (Width == 0 || Width % kBitsPerByte != 0)
    return 1;
  const size_t Size = Width / kBitsPerByte;
  if (Size > sizeof(uint64_t))
    return 1;
  for (MemoryRegion &Region : Environment.Memory) {
    size_t Offset = 0;
    if (!contains(Region, Address, Size, Offset))
      continue;
    if (!Region.Writable)
      return 1;
    for (size_t I = 0; I < Size; ++I)
      Region.Bytes[Offset + I] =
          static_cast<uint8_t>(Value >> (I * kBitsPerByte));
    return 0;
  }
  return 1;
}

extern "C" int32_t differentialSyscall(void *Opaque, uint32_t Hash,
#define SBF_ARGUMENT_REGISTER(ID, REGISTER) uint64_t ID,
#include "neverd/sbf/SBFArgumentRegisters.def"
                                       uint64_t *Result) {
  auto &Environment = *static_cast<JITEnvironment *>(Opaque);
  const SyscallArguments Arguments{
#define SBF_ARGUMENT_REGISTER(ID, REGISTER) ID,
#include "neverd/sbf/SBFArgumentRegisters.def"
  };
  std::optional<uint64_t> Value =
      Environment.Syscall ? Environment.Syscall(Hash, Arguments) : std::nullopt;
  if (!Result || !Value)
    return 1;
  *Result = *Value;
  Environment.Syscalls.push_back({Hash, Arguments, *Value});
  return 0;
}

extern "C" void differentialFault(void *Opaque, uint32_t Code,
                                  uint64_t Address) {
  auto &Environment = *static_cast<JITEnvironment *>(Opaque);
  if (Environment.Fault != FaultCode::None)
    return;
  Environment.Fault = static_cast<FaultCode>(Code);
  Environment.FaultAddress = Address;
}

llvm::Error defineRuntimeSymbols(llvm::orc::LLJIT &JIT) {
  llvm::orc::SymbolMap Symbols;
  auto Add = [&](llvm::StringRef Name, auto *Function) {
    Symbols[JIT.mangleAndIntern(Name)] = llvm::orc::ExecutorSymbolDef::fromPtr(
        Function, llvm::JITSymbolFlags::Exported);
  };
  Add(kRuntimeLoadName, &differentialLoad);
  Add(kRuntimeStoreName, &differentialStore);
  Add(kRuntimeSyscallName, &differentialSyscall);
  Add(kRuntimeFaultName, &differentialFault);
  return JIT.getMainJITDylib().define(
      llvm::orc::absoluteSymbols(std::move(Symbols)));
}

llvm::Expected<uint64_t> runJIT(const SBFProgram &Program,
                                JITEnvironment &Environment) {
  static const bool NativeTargetReady =
      !llvm::InitializeNativeTarget() &&
      !llvm::InitializeNativeTargetAsmPrinter();
  if (!NativeTargetReady)
    return llvm::make_error<llvm::StringError>(
        "sbf: LLVM native target initialization failed",
        llvm::inconvertibleErrorCode());

  auto JIT = llvm::orc::LLJITBuilder().create();
  if (!JIT)
    return JIT.takeError();
  if (llvm::Error Error = defineRuntimeSymbols(**JIT))
    return std::move(Error);

  auto Context = std::make_unique<llvm::LLVMContext>();
  auto Module = emitLLVM(Program, *Context);
  if (!Module)
    return Module.takeError();
  (*Module)->setDataLayout((*JIT)->getDataLayout());
  (*Module)->setTargetTriple((*JIT)->getTargetTriple());
  if (llvm::Error Error = (*JIT)->addIRModule(
          llvm::orc::ThreadSafeModule(std::move(*Module), std::move(Context))))
    return std::move(Error);

  auto Entry = (*JIT)->lookup(kEntryFunctionName);
  if (!Entry)
    return Entry.takeError();
  JITFunction *Function = Entry->toPtr<JITFunction *>();
  return Function(&Environment, Environment.Input);
}

void expectMemoryEqual(const std::vector<MemoryRegion> &Expected,
                       const std::vector<MemoryRegion> &Actual) {
  ASSERT_EQ(Actual.size(), Expected.size());
  for (size_t I = 0; I < Expected.size(); ++I) {
    SCOPED_TRACE("memory region " + std::to_string(I));
    EXPECT_EQ(Actual[I].Address, Expected[I].Address);
    EXPECT_EQ(Actual[I].Bytes, Expected[I].Bytes);
    EXPECT_EQ(Actual[I].Writable, Expected[I].Writable);
    EXPECT_EQ(Actual[I].Name, Expected[I].Name);
  }
}

void expectDifferential(const SBFProgram &Program,
                        ExecutionEnvironment Environment = {}) {
  JITEnvironment JITState;
  JITState.Input = Environment.Input;
  JITState.Memory = Environment.Memory;
  JITState.Syscall = Environment.Syscall;
  appendProgramMemory(Program, JITState);

  auto JITResult = runJIT(Program, JITState);
  ASSERT_TRUE(static_cast<bool>(JITResult))
      << llvm::toString(JITResult.takeError());
  auto Oracle = executeRaw(Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());

  if (Oracle->Status == ExecutionStatus::Faulted) {
    EXPECT_EQ(JITState.Fault, Oracle->Fault);
    EXPECT_EQ(JITState.FaultAddress,
              Program.Low.TextAddress + Oracle->FinalSlot * kInstructionSize);
  } else {
    EXPECT_EQ(Oracle->Status, ExecutionStatus::Returned);
    EXPECT_EQ(JITState.Fault, FaultCode::None);
    EXPECT_EQ(*JITResult, Oracle->ReturnValue);
  }
  expectMemoryEqual(Oracle->Memory, JITState.Memory);

  ASSERT_EQ(JITState.Syscalls.size(), Oracle->Syscalls.size());
  for (size_t I = 0; I < Oracle->Syscalls.size(); ++I) {
    EXPECT_EQ(JITState.Syscalls[I].Hash, Oracle->Syscalls[I].Hash);
    EXPECT_EQ(JITState.Syscalls[I].Arguments, Oracle->Syscalls[I].Arguments);
    EXPECT_EQ(JITState.Syscalls[I].Result, Oracle->Syscalls[I].Result);
  }
}

SBFProgram
checkedProgram(Version TheVersion,
               std::initializer_list<EncodedInstruction> Instructions) {
  auto Program = analyzeProgram(TheVersion, Instructions);
  if (!Program) {
    ADD_FAILURE() << llvm::toString(Program.takeError());
    return {};
  }
  return std::move(*Program);
}

TEST(SBFLLVMDifferential, MatchesVersionedArithmeticAndBranches) {
  for (Version TheVersion :
       {Version::V0, Version::V1, Version::V2, Version::V3, Version::V4}) {
    SCOPED_TRACE(versionName(TheVersion).str());
    SBFProgram Program =
        checkedProgram(TheVersion, {encode(Opcode::MOV64_IMM, 0, 0, 0,
                                           std::numeric_limits<int32_t>::max()),
                                    encode(Opcode::ADD32_IMM, 0, 0, 0, 1),
                                    encode(Opcode::JEQ64_REG, 0, 0, 1),
                                    encode(Opcode::MOV64_IMM, 0, 0, 0, 99),
                                    encode(Opcode::EXIT)});
    expectDifferential(Program);
  }

  SBFProgram PQR =
      checkedProgram(Version::V2, {encode(Opcode::MOV64_IMM, 0, 0, 0, -2),
                                   encode(Opcode::UDIV64_IMM, 0, 0, 0, -1),
                                   encode(Opcode::EXIT)});
  expectDifferential(PQR);
}

TEST(SBFLLVMDifferential, MatchesNestedStaticCallsAndReturns) {
  SBFProgram Program = checkedProgram(
      Version::V3,
      {encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
       encode(Opcode::CALL_IMM, 0, 1, 0, 3),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 0),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 40),
       encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT)});
  expectDifferential(Program);
}

TEST(SBFLLVMDifferential, MatchesMemoryAndSyscallEffects) {
  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  SBFProgram Program = checkedProgram(
      Version::V3,
      {encode(Opcode::LD_DW_REG, 2, 1),
       encode(Opcode::ST_DW_REG, kFramePointerRegister, 2, -8),
       encode(Opcode::LD_DW_REG, 1, kFramePointerRegister, -8),
       encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)});

  ExecutionEnvironment Environment;
  Environment.Memory.push_back(
      {kInputStart, {9, 0, 0, 0, 0, 0, 0, 0}, false, "input"});
  Environment.Syscall = [](uint32_t, const SyscallArguments &Arguments)
      -> std::optional<uint64_t> { return Arguments[0] + 1; };
  expectDifferential(Program, std::move(Environment));
}

TEST(SBFLLVMDifferential, MatchesRuntimeFaults) {
  SBFProgram DivideByZero = checkedProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 1, 0, 0, 7),
                    encode(Opcode::MOV64_IMM, 2, 0, 0, 0),
                    encode(Opcode::UDIV64_REG, 1, 2), encode(Opcode::EXIT)});
  expectDifferential(DivideByZero);

  SBFProgram BadMemory = checkedProgram(
      Version::V3, {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                    encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)});
  expectDifferential(BadMemory);
}

TEST(SBFLLVMDifferential, MatchesUnalignedV2CallXFlooring) {
  constexpr size_t TargetSlot = 4;
  constexpr int32_t UnalignedTarget =
      static_cast<int32_t>(TargetSlot * kInstructionSize + 3);
  SBFProgram Program = checkedProgram(
      Version::V2,
      {encode(Opcode::MOV64_IMM, 2, 0, 0, UnalignedTarget),
       encode(Opcode::HOR64_IMM, 2, 0, 0, 1), encode(Opcode::CALL_REG, 0, 2),
       encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 77),
       encode(Opcode::EXIT)});
  expectDifferential(Program);
}

TEST(SBFLLVMDifferential, MatchesCallXFaultOrderingAndContinuationTargets) {
  SBFProgram BadTarget = checkedProgram(
      Version::V2, {encode(Opcode::MOV64_IMM, 2, 0, 0, 0),
                    encode(Opcode::CALL_REG, 0, 2), encode(Opcode::EXIT)});
  BadTarget.Config.MaxCallDepth = 1;
  expectDifferential(BadTarget);

  const auto LoadTarget = encodeLDDW(2, kBytecodeStart + kInstructionSize);
  SBFProgram Continuation = checkedProgram(
      Version::V3, {LoadTarget[0], LoadTarget[1], encode(Opcode::CALL_REG, 2),
                    encode(Opcode::EXIT)});
  expectDifferential(Continuation);
}

} // namespace
} // namespace neverd::sbf
