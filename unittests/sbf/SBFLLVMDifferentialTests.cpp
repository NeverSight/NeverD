//===- SBFLLVMDifferentialTests.cpp - SBF LLVM differential tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/emit/SBFLLVMEmitter.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"

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
using JITFunction = uint64_t(void *, uint64_t, uint64_t);

struct JITSyscallTraceEntry {
  uint32_t Hash = 0;
  SyscallArguments Arguments{};
  uint64_t Result = 0;
};

struct JITEnvironment {
  uint64_t Input = kInputStart;
  uint64_t InstructionData = 0;
  std::vector<MemoryRegion> Memory;
  SyscallCallback Syscall;
  SyscallOutcomeCallback HostSyscall;
  FeatureAwareSyscallCallback FeatureAwareSyscall;
  RuntimeFeature ExpectedRuntimeFeatures = RuntimeFeature::None;
  bool RuntimeFeatureMismatch = false;
  std::optional<uint32_t> LoadStatus;
  std::optional<uint32_t> StoreStatus;
  std::vector<JITSyscallTraceEntry> Syscalls;
  FaultCode Fault = FaultCode::None;
  uint64_t FaultAddress = 0;
};

struct JITFaultInjection {
  std::optional<uint32_t> LoadStatus;
  std::optional<uint32_t> StoreStatus;
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
               std::initializer_list<EncodedInstruction> Instructions,
               const AnalyzeOptions &Options = {}, size_t EntrySlot = 0) {
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
  AnalyzeOptions EffectiveOptions = Options;
  if (TheVersion == Version::V4 && !EffectiveOptions.ExpertEnvironment)
    EffectiveOptions.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
        Version::V0, Version::V4, SBFVMConfig{}};
  return analyze(Image, EffectiveOptions);
}

bool contains(const MemoryRegion &Region, uint64_t Address, size_t Size,
              size_t &Offset) {
  if (Address < Region.Address)
    return false;
  const uint64_t Delta = Address - Region.Address;
  if (Region.VMGapSize == 0) {
    if (Delta > Region.Bytes.size() || Size > Region.Bytes.size() - Delta)
      return false;
    Offset = static_cast<size_t>(Delta);
    return true;
  }
  if (Region.Bytes.size() % Region.VMGapSize != 0 ||
      Region.Bytes.size() >
          std::numeric_limits<uint64_t>::max() / kStackFrameGapMultiplier ||
      Delta >= Region.Bytes.size() * kStackFrameGapMultiplier)
    return false;
  const uint64_t Period =
      static_cast<uint64_t>(Region.VMGapSize) * kStackFrameGapMultiplier;
  const uint64_t PeriodOffset = Delta % Period;
  if (PeriodOffset >= Region.VMGapSize ||
      Size > Region.VMGapSize - PeriodOffset)
    return false;
  const uint64_t BackingOffset =
      Delta / Period * Region.VMGapSize + PeriodOffset;
  if (BackingOffset > Region.Bytes.size() ||
      Size > Region.Bytes.size() - BackingOffset)
    return false;
  Offset = static_cast<size_t>(BackingOffset);
  return true;
}

void appendProgramMemory(const SBFProgram &Program,
                         JITEnvironment &Environment) {
  for (const ProgramRegion &Region : Program.ExecutableImage.regions())
    if (Region.DataVisible && !Region.Bytes.empty())
      Environment.Memory.push_back(
          {Region.Address, Region.Bytes, false, Region.Name});

  const bool HasStackFrameGaps =
      usesStackFrameGaps(Program.Low.TheVersion, Program.Config);
  const bool RequiresAlignedMapping =
      versionHasFeature(Program.Low.TheVersion,
                        VersionFeature::AlignedMemoryMapping) ||
      Program.Config.AlignedMemoryMapping;
  if (HasStackFrameGaps && RequiresAlignedMapping) {
    MemoryRegion Stack;
    Stack.Address = kStackStart;
    Stack.Bytes.resize(stackSize(Program.Config));
    Stack.Writable = true;
    Stack.Name = "stack";
    Stack.VMGapSize = Program.Config.StackFrameSize;
    Environment.Memory.push_back(std::move(Stack));
    return;
  } else if (HasStackFrameGaps) {
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
    return static_cast<int32_t>(FaultCode::MemoryAccess);
  if (Environment.LoadStatus)
    return static_cast<int32_t>(*Environment.LoadStatus);
  const size_t Size = Width / kBitsPerByte;
  if (Size > sizeof(uint64_t))
    return static_cast<int32_t>(FaultCode::MemoryAccess);
  for (const MemoryRegion &Region : Environment.Memory) {
    size_t Offset = 0;
    if (!contains(Region, Address, Size, Offset))
      continue;
    *Result = 0;
    for (size_t I = 0; I < Size; ++I)
      *Result |= static_cast<uint64_t>(Region.Bytes[Offset + I])
                 << (I * kBitsPerByte);
    return static_cast<int32_t>(FaultCode::None);
  }
  return static_cast<int32_t>(FaultCode::MemoryAccess);
}

extern "C" int32_t differentialStore(void *Opaque, uint64_t Address,
                                     uint32_t Width, uint64_t Value) {
  auto &Environment = *static_cast<JITEnvironment *>(Opaque);
  if (Width == 0 || Width % kBitsPerByte != 0)
    return static_cast<int32_t>(FaultCode::MemoryAccess);
  if (Environment.StoreStatus)
    return static_cast<int32_t>(*Environment.StoreStatus);
  const size_t Size = Width / kBitsPerByte;
  if (Size > sizeof(uint64_t))
    return static_cast<int32_t>(FaultCode::MemoryAccess);
  for (MemoryRegion &Region : Environment.Memory) {
    size_t Offset = 0;
    if (!contains(Region, Address, Size, Offset))
      continue;
    if (!Region.Writable)
      return static_cast<int32_t>(FaultCode::MemoryAccess);
    for (size_t I = 0; I < Size; ++I)
      Region.Bytes[Offset + I] =
          static_cast<uint8_t>(Value >> (I * kBitsPerByte));
    return static_cast<int32_t>(FaultCode::None);
  }
  return static_cast<int32_t>(FaultCode::MemoryAccess);
}

int32_t dispatchSyscall(JITEnvironment &Environment,
                        const SyscallInvocation &Invocation, uint64_t *Result) {
  if (!Result)
    return static_cast<int32_t>(FaultCode::InvalidInstruction);
  SyscallOutcome Outcome = SyscallOutcome::unregistered();
  if (Environment.FeatureAwareSyscall) {
    Outcome = Environment.FeatureAwareSyscall(Invocation);
  } else if (Environment.HostSyscall) {
    Outcome = Environment.HostSyscall(Invocation.Hash, Invocation.Arguments);
  } else if (Environment.Syscall) {
    std::optional<uint64_t> Value =
        Environment.Syscall(Invocation.Hash, Invocation.Arguments);
    Outcome = Value ? SyscallOutcome::returned(*Value)
                    : SyscallOutcome::unregistered();
  }
  if (Outcome.kind() != SyscallOutcome::Kind::Returned)
    return static_cast<int32_t>(Outcome.abiStatus());
  *Result = Outcome.value();
  Environment.Syscalls.push_back(
      {Invocation.Hash, Invocation.Arguments, Outcome.value()});
  return static_cast<int32_t>(FaultCode::None);
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
  return dispatchSyscall(
      Environment,
      SyscallInvocation{Hash, Arguments, Environment.ExpectedRuntimeFeatures},
      Result);
}

extern "C" int32_t differentialFeatureAwareSyscall(
    void *Opaque, RuntimeFeatureMask RuntimeFeatures, uint32_t Hash,
#define SBF_ARGUMENT_REGISTER(ID, REGISTER) uint64_t ID,
#include "neverd/sbf/SBFArgumentRegisters.def"
    uint64_t *Result) {
  auto &Environment = *static_cast<JITEnvironment *>(Opaque);
  const SyscallArguments Arguments{
#define SBF_ARGUMENT_REGISTER(ID, REGISTER) ID,
#include "neverd/sbf/SBFArgumentRegisters.def"
  };
  const RuntimeFeature Snapshot = static_cast<RuntimeFeature>(RuntimeFeatures);
  if (Snapshot != Environment.ExpectedRuntimeFeatures) {
    Environment.RuntimeFeatureMismatch = true;
    return static_cast<int32_t>(FaultCode::InvalidInstruction);
  }
  return dispatchSyscall(Environment,
                         SyscallInvocation{Hash, Arguments, Snapshot}, Result);
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
  Add(kRuntimeFeatureAwareSyscallName, &differentialFeatureAwareSyscall);
  Add(kRuntimeFaultName, &differentialFault);
  return JIT.getMainJITDylib().define(
      llvm::orc::absoluteSymbols(std::move(Symbols)));
}

llvm::Expected<uint64_t> runJIT(const SBFProgram &Program,
                                JITEnvironment &Environment,
                                const LLVMEmitterOptions &Options = {}) {
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
  auto Module = emitLLVM(Program, *Context, Options);
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
  return Function(&Environment, Environment.Input, Environment.InstructionData);
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
    EXPECT_EQ(Actual[I].VMGapSize, Expected[I].VMGapSize);
  }
}

void expectDifferential(const SBFProgram &Program,
                        ExecutionEnvironment Environment = {},
                        JITFaultInjection FaultInjection = {}) {
  JITEnvironment JITState;
  JITState.Input = Environment.Input;
  JITState.Memory = Environment.Memory;
  JITState.Syscall = Environment.Syscall;
  JITState.HostSyscall = Environment.HostSyscall;
  JITState.FeatureAwareSyscall = Environment.FeatureAwareSyscall;
  JITState.ExpectedRuntimeFeatures =
      Environment.RuntimeFeatures.value_or(Program.ActiveRuntimeFeatures);
  JITState.LoadStatus = FaultInjection.LoadStatus;
  JITState.StoreStatus = FaultInjection.StoreStatus;
  appendProgramMemory(Program, JITState);

  LLVMEmitterOptions EmitterOptions;
  EmitterOptions.SyscallABI = LLVMRuntimeSyscallABI::FeatureAware;
  EmitterOptions.RuntimeFeatures = Environment.RuntimeFeatures;
  auto JITResult = runJIT(Program, JITState, EmitterOptions);
  ASSERT_TRUE(static_cast<bool>(JITResult))
      << llvm::toString(JITResult.takeError());
  EXPECT_FALSE(JITState.RuntimeFeatureMismatch);
  auto Oracle = executeRaw(Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  if (FaultInjection.LoadStatus || FaultInjection.StoreStatus) {
    ASSERT_EQ(Oracle->Status, ExecutionStatus::Faulted);
    ASSERT_EQ(Oracle->Fault, FaultCode::MemoryAccess);
    const uint32_t Status = FaultInjection.LoadStatus
                                ? *FaultInjection.LoadStatus
                                : *FaultInjection.StoreStatus;
    if (isKnownFaultCodeValue(Status) &&
        Status != static_cast<uint32_t>(FaultCode::None))
      Oracle->Fault = static_cast<FaultCode>(Status);
  }

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

TEST(SBFLLVMDifferential, EntrypointContinuationMatchesTheRawFault) {
  const auto Load = encodeLDDW(0, 0);
  auto Program = analyzeProgram(
      Version::V3, {Load[0], Load[1], encode(Opcode::EXIT)}, {}, 1);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  expectDifferential(*Program);
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

TEST(SBFLLVMDifferential, DeliversExactResolvedFeaturesThroughTheTypedABI) {
  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  SBFProgram Program = checkedProgram(
      Version::V3,
      {encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)});
  Program.ActiveRuntimeFeatures =
      RuntimeFeature::SyscallParameterAddressRestrictions |
      RuntimeFeature::AccountDataDirectMapping |
      RuntimeFeature::DisableAllocFreeDeployment;

  size_t DefaultCalls = 0;
  ExecutionEnvironment DefaultEnvironment;
  DefaultEnvironment.FeatureAwareSyscall =
      [&](const SyscallInvocation &Invocation) -> SyscallOutcome {
    EXPECT_EQ(Invocation.RuntimeFeatures, Program.ActiveRuntimeFeatures);
    ++DefaultCalls;
    return SyscallOutcome::returned(42);
  };
  expectDifferential(Program, std::move(DefaultEnvironment));
  EXPECT_EQ(DefaultCalls, 2u);

  size_t EmptyCalls = 0;
  ExecutionEnvironment EmptyEnvironment;
  EmptyEnvironment.RuntimeFeatures = RuntimeFeature::None;
  EmptyEnvironment.FeatureAwareSyscall =
      [&](const SyscallInvocation &Invocation) -> SyscallOutcome {
    EXPECT_EQ(Invocation.RuntimeFeatures, RuntimeFeature::None);
    ++EmptyCalls;
    return SyscallOutcome::returned(7);
  };
  expectDifferential(Program, std::move(EmptyEnvironment));
  EXPECT_EQ(EmptyCalls, 2u);
}

TEST(SBFLLVMDifferential, MatchesRuntimeResolvedLegacySyscalls) {
  SBFProgram Program =
      checkedProgram(Version::V0, {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
                                   encode(Opcode::CALL_IMM, 0, 0, 0, -1),
                                   encode(Opcode::EXIT)});
  ASSERT_EQ(Program.Low.Instructions[1].Call, CallKind::Syscall);
  ASSERT_EQ(Program.Low.Instructions[1].Syscall, nullptr);

  ExecutionEnvironment Environment;
  Environment.Syscall =
      [](uint32_t Hash,
         const SyscallArguments &Arguments) -> std::optional<uint64_t> {
    if (Hash != std::numeric_limits<uint32_t>::max())
      return std::nullopt;
    return Arguments[0] + 1;
  };
  expectDifferential(Program, std::move(Environment));
}

TEST(SBFLLVMDifferential, LegacyRuntimeSyscallsThenInvokeInternalCallKeys) {
  constexpr size_t TargetSlot = 4;
  const uint32_t Key = legacyFunctionKey(TargetSlot, {});
  SBFProgram Program = checkedProgram(
      Version::V0,
      {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
       encode(Opcode::CALL_IMM, 0, 0, 0, 2),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 7), encode(Opcode::EXIT)});
  ASSERT_EQ(Program.Low.Instructions[1].SyscallHash, Key);
  ASSERT_EQ(Program.Low.Instructions[1].CallTarget,
            std::optional<size_t>(TargetSlot));

  ExecutionEnvironment Environment;
  Environment.Syscall =
      [Key](uint32_t Hash,
            const SyscallArguments &Arguments) -> std::optional<uint64_t> {
    if (Hash != Key)
      return std::nullopt;
    return Arguments[0] + 1;
  };
  expectDifferential(Program, std::move(Environment));
}

TEST(SBFLLVMDifferential, PropagatesHandledHostSyscallFaults) {
  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  SBFProgram Program = checkedProgram(
      Version::V3,
      {encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)});

  ExecutionEnvironment Environment;
  Environment.HostSyscall = [](uint32_t,
                               const SyscallArguments &) -> SyscallOutcome {
    return SyscallOutcome::fault(FaultCode::MemoryAccess);
  };
  expectDifferential(Program, std::move(Environment));
}

TEST(SBFLLVMDifferential,
     DoesNotHideHandledFaultBehindLegacyFunctionCollision) {
  constexpr size_t TargetSlot = 4;
  const uint32_t Key = legacyFunctionKey(TargetSlot, {});
  SBFProgram Program = checkedProgram(
      Version::V0,
      {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
       encode(Opcode::CALL_IMM, 0, 0, 0, 2),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 7), encode(Opcode::EXIT)});

  ExecutionEnvironment Environment;
  Environment.HostSyscall = [Key](uint32_t Hash,
                                  const SyscallArguments &) -> SyscallOutcome {
    return Hash == Key ? SyscallOutcome::fault(FaultCode::MemoryAccess)
                       : SyscallOutcome::unregistered();
  };
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

TEST(SBFLLVMDifferential, PropagatesTypedLoadAndStoreCallbackFaults) {
  SBFProgram Load = checkedProgram(
      Version::V3, {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                    encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)});
  expectDifferential(
      Load, {},
      JITFaultInjection{static_cast<uint32_t>(FaultCode::DivideByZero),
                        std::nullopt});

  SBFProgram Store =
      checkedProgram(Version::V3, {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                                   encode(Opcode::ST_DW_IMM, 1, 0, 0, 42),
                                   encode(Opcode::EXIT)});
  expectDifferential(
      Store, {},
      JITFaultInjection{std::nullopt,
                        static_cast<uint32_t>(FaultCode::DivideOverflow)});

  expectDifferential(Load, {},
                     JITFaultInjection{static_cast<uint32_t>(
                                           std::numeric_limits<int32_t>::max()),
                                       std::nullopt});
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

TEST(SBFLLVMDifferential,
     MatchesStaticCallDispatchAndContinuationFaultOrdering) {
  const auto Load = encodeLDDW(0, 0);
  SBFProgram Continuation =
      checkedProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                                   Load[0], Load[1], encode(Opcode::EXIT)});
  expectDifferential(Continuation);

  Continuation.Config.MaxCallDepth = 1;
  expectDifferential(Continuation);

  SBFProgram InvalidDiscriminator = checkedProgram(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 2), encode(Opcode::EXIT)});
  expectDifferential(InvalidDiscriminator);
  InvalidDiscriminator.Config.MaxCallDepth = 1;
  expectDifferential(InvalidDiscriminator);

  SBFProgram OutOfRange = checkedProgram(
      Version::V3,
      {encode(Opcode::CALL_IMM, 0, 1, 0, std::numeric_limits<int32_t>::max()),
       encode(Opcode::EXIT)});
  expectDifferential(OutOfRange);
  OutOfRange.Config.MaxCallDepth = 1;
  expectDifferential(OutOfRange);
}

TEST(SBFLLVMDifferential,
     MalformedInstructionsMatchTheRawFaultWithoutPoisonedOperands) {
  AnalyzeOptions Relaxed;
  Relaxed.Strict = false;
  auto ExpectFault = [&](llvm::StringRef Name, Version TheVersion,
                         std::initializer_list<EncodedInstruction> Text,
                         ValidationRule Rule, FaultCode Fault,
                         size_t FaultSlot = 0) {
    SCOPED_TRACE(Name.str());
    auto Strict = analyzeProgram(TheVersion, Text);
    EXPECT_FALSE(static_cast<bool>(Strict));
    if (Strict)
      return;
    llvm::consumeError(Strict.takeError());

    auto Program = analyzeProgram(TheVersion, Text, Relaxed);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    const auto It = std::find_if(Program->Med.Instructions.begin(),
                                 Program->Med.Instructions.end(),
                                 [&](const MedInstruction &Instruction) {
                                   return Instruction.Slot == FaultSlot;
                                 });
    ASSERT_NE(It, Program->Med.Instructions.end());
    EXPECT_EQ(It->Op, Operation::Invalid);
    EXPECT_EQ(It->InvalidReason, Rule);
    EXPECT_EQ(getValidationRuleInfo(Rule).Fault, Fault);
    EXPECT_EQ(It->Dst, 0);
    EXPECT_EQ(It->Src, 0);
    EXPECT_FALSE(It->BranchTarget.has_value());
    expectDifferential(*Program);
  };

  EncodedInstruction Unknown{};
  Unknown[kOpcodeOffset] = std::numeric_limits<uint8_t>::max();
  ExpectFault("unknown-opcode", Version::V3, {Unknown, encode(Opcode::EXIT)},
              ValidationRule::UnknownOpcode, FaultCode::InvalidInstruction);
  ExpectFault("version-inactive-opcode", Version::V2,
              {encode(Opcode::LDDW), {}, encode(Opcode::EXIT)},
              ValidationRule::UnknownOpcode, FaultCode::InvalidInstruction);
  ExpectFault("missing-lddw-continuation", Version::V0, {encode(Opcode::LDDW)},
              ValidationRule::MissingLDDWContinuation,
              FaultCode::InvalidInstruction);
  ExpectFault(
      "nonzero-lddw-continuation", Version::V0,
      {encode(Opcode::LDDW), encode(Opcode::EXIT), encode(Opcode::EXIT)},
      ValidationRule::NonZeroLDDWContinuation, FaultCode::InvalidInstruction);
  ExpectFault("lddw-continuation-precedes-invalid-registers", Version::V0,
              {encode(Opcode::LDDW, 15, 15), encode(Opcode::EXIT),
               encode(Opcode::EXIT)},
              ValidationRule::NonZeroLDDWContinuation,
              FaultCode::InvalidInstruction);
  ExpectFault("invalid-lddw-destination", Version::V0,
              {encode(Opcode::LDDW, 15), {}, encode(Opcode::EXIT)},
              ValidationRule::InvalidDestinationRegister,
              FaultCode::InvalidRegister);
  ExpectFault("immediate-division-by-zero", Version::V2,
              {encode(Opcode::UDIV64_IMM, 0, 0, 0, 0), encode(Opcode::EXIT)},
              ValidationRule::ImmediateDivisionByZero, FaultCode::DivideByZero);
  ExpectFault("division-precedes-invalid-registers", Version::V2,
              {encode(Opcode::UDIV64_IMM, 15, 15, 0, 0), encode(Opcode::EXIT)},
              ValidationRule::ImmediateDivisionByZero, FaultCode::DivideByZero);
  ExpectFault("immediate-shift-out-of-range", Version::V3,
              {encode(Opcode::LSH64_IMM, 0, 0, 0, kDoubleWordBitWidth),
               encode(Opcode::EXIT)},
              ValidationRule::ImmediateShiftOutOfRange,
              FaultCode::InvalidInstruction);
  ExpectFault("invalid-endian-immediate", Version::V3,
              {encode(Opcode::BE, 0, 0, 0, 24), encode(Opcode::EXIT)},
              ValidationRule::InvalidEndianImmediate,
              FaultCode::InvalidInstruction);
  ExpectFault("misaligned-frame-adjustment", Version::V2,
              {encode(Opcode::ADD64_IMM, kFramePointerRegister, 0, 0, 1),
               encode(Opcode::EXIT)},
              ValidationRule::MisalignedFrameAdjustment,
              FaultCode::InvalidInstruction);
  ExpectFault("branch-out-of-range", Version::V3,
              {encode(Opcode::JA, 0, 0, std::numeric_limits<int16_t>::max()),
               encode(Opcode::EXIT)},
              ValidationRule::BranchOutOfRange, FaultCode::InvalidBranch);
  ExpectFault(
      "branch-precedes-invalid-registers", Version::V3,
      {encode(Opcode::JEQ64_REG, 15, 15, std::numeric_limits<int16_t>::max()),
       encode(Opcode::EXIT)},
      ValidationRule::BranchOutOfRange, FaultCode::InvalidBranch);

  const auto Load = encodeLDDW(0, 0);
  ExpectFault(
      "branch-to-lddw-continuation", Version::V0,
      {Load[0], Load[1], encode(Opcode::JA, 0, 0, -2), encode(Opcode::EXIT)},
      ValidationRule::BranchToLDDWContinuation, FaultCode::InvalidBranch, 2);
  ExpectFault("invalid-callx-register", Version::V0,
              {encode(Opcode::CALL_REG, 0, 0, 0, -1), encode(Opcode::EXIT)},
              ValidationRule::InvalidCallXRegister, FaultCode::InvalidRegister);
  ExpectFault("callx-precedes-invalid-source-register", Version::V0,
              {encode(Opcode::CALL_REG, 0, 15, 0, -1), encode(Opcode::EXIT)},
              ValidationRule::InvalidCallXRegister, FaultCode::InvalidRegister);
  ExpectFault(
      "invalid-source-register", Version::V3,
      {encode(Opcode::MOV64_REG, 0, std::numeric_limits<uint8_t>::max()),
       encode(Opcode::EXIT)},
      ValidationRule::InvalidSourceRegister, FaultCode::InvalidRegister);
  ExpectFault(
      "frame-pointer-write", Version::V3,
      {encode(Opcode::MOV64_IMM, kFramePointerRegister), encode(Opcode::EXIT)},
      ValidationRule::FramePointerWrite, FaultCode::InvalidRegister);
  ExpectFault("invalid-destination-register", Version::V3,
              {encode(Opcode::MOV64_IMM, 15), encode(Opcode::EXIT)},
              ValidationRule::InvalidDestinationRegister,
              FaultCode::InvalidRegister);
}

} // namespace
} // namespace neverd::sbf
