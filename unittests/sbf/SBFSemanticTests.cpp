//===- SBFSemanticTests.cpp - Solana SBF semantic oracle tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/sbf/runtime/SBFSemantics.h"

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

constexpr std::array<Version, 5> kConcreteVersions{
    Version::V0, Version::V1, Version::V2, Version::V3, Version::V4};

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
               size_t EntrySlot = 0, std::vector<Symbol> Symbols = {}) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart + EntrySlot * kInstructionSize;
  Image.Symbols = std::move(Symbols);
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

  AnalyzeOptions Options;
  if (TheVersion == Version::V4)
    Options.ExpertEnvironment = ExpertRuntimeEnvironmentOverride{
        Version::V0, Version::V4, SBFVMConfig{}};
  auto Program = analyze(Image, Options);
  EXPECT_TRUE(static_cast<bool>(Program))
      << (Program ? std::string() : llvm::toString(Program.takeError()));
  return Program ? std::move(*Program) : SBFProgram{};
}

TEST(SBFSemantics, NormalizesVersionedOperandsAndResults) {
  const OpcodeInfo &UnsignedPQR = *getOpcodeInfo(Opcode::UDIV64_IMM);
  const OpcodeInfo &SignedPQR = *getOpcodeInfo(Opcode::SDIV64_IMM);
  const OpcodeInfo &Move32Register = *getOpcodeInfo(Opcode::MOV32_REG);
  const OpcodeInfo &Add32Immediate = *getOpcodeInfo(Opcode::ADD32_IMM);
  const OpcodeInfo &Load = *getOpcodeInfo(Opcode::LD_DW_REG);
  const OpcodeInfo &HighOr = *getOpcodeInfo(Opcode::HOR64_IMM);
  const OpcodeInfo &Store = *getOpcodeInfo(Opcode::ST_DW_REG);

  EXPECT_EQ(semanticTraits(UnsignedPQR, Version::V2).Immediate,
            ImmediateExtension::Zero32);
  EXPECT_EQ(semanticTraits(SignedPQR, Version::V2).Immediate,
            ImmediateExtension::Sign32);
  EXPECT_EQ(semanticTraits(Move32Register, Version::V2).Result,
            ResultExtension::Sign32);
  EXPECT_EQ(semanticTraits(Move32Register, Version::V3).Result,
            ResultExtension::Zero32);
  EXPECT_EQ(semanticTraits(Add32Immediate, Version::V2).Result,
            ResultExtension::Zero32);
  EXPECT_EQ(semanticTraits(Add32Immediate, Version::V3).Result,
            ResultExtension::Sign32);
  EXPECT_EQ(semanticTraits(Load, Version::V3).Source,
            OperandSourceKind::SourceRegister);
  EXPECT_TRUE(semanticTraits(HighOr, Version::V2).WritesDestination);
  EXPECT_FALSE(semanticTraits(Store, Version::V3).WritesDestination);

  EXPECT_EQ(
      normalizeImmediate(UINT64_C(0x80000000), ImmediateExtension::Sign32),
      UINT64_C(0xffffffff80000000));
  EXPECT_EQ(normalizeImmediate(UINT64_C(0xffffffff80000000),
                               ImmediateExtension::Zero32),
            UINT64_C(0x80000000));
  EXPECT_EQ(extendALU32Result(UINT32_C(0x80000000), ResultExtension::Sign32),
            UINT64_C(0xffffffff80000000));
  EXPECT_EQ(extendALU32Result(UINT32_C(0x80000000), ResultExtension::Zero32),
            UINT64_C(0x80000000));
}

TEST(SBFSemantics, ClassifiesEveryActiveOpcodeConsistently) {
  for (Version TheVersion : kConcreteVersions) {
    for (const OpcodeInfo &Info : opcodeInfos()) {
      if (!Info.isAvailableIn(TheVersion))
        continue;
      SCOPED_TRACE(versionName(TheVersion).str() + ":" + Info.Mnemonic.str());
      const SemanticTraits Traits = semanticTraits(Info, TheVersion);

      const OperandSourceKind ExpectedSource =
          Info.Form == OperandForm::CallReg
              ? OperandSourceKind::VersionedCallRegister
          : Info.usesSourceRegister() ? OperandSourceKind::SourceRegister
          : Info.usesImmediate()      ? OperandSourceKind::Immediate
                                      : OperandSourceKind::None;
      EXPECT_EQ(Traits.Source, ExpectedSource);
      EXPECT_EQ(Traits.MemoryWidth,
                Info.readsMemory() || Info.writesMemory() ? Info.Width : 0);

      if (Info.Op == Operation::UDiv || Info.Op == Operation::URem) {
        EXPECT_TRUE(hasFaultPolicy(Traits.Faults, FaultPolicy::DivideByZero));
        EXPECT_FALSE(
            hasFaultPolicy(Traits.Faults, FaultPolicy::DivideOverflow));
      } else if (Info.Op == Operation::SDiv || Info.Op == Operation::SRem) {
        EXPECT_TRUE(hasFaultPolicy(Traits.Faults, FaultPolicy::DivideByZero));
        EXPECT_TRUE(hasFaultPolicy(Traits.Faults, FaultPolicy::DivideOverflow));
      } else if (Info.readsMemory() || Info.writesMemory()) {
        EXPECT_TRUE(hasFaultPolicy(Traits.Faults, FaultPolicy::MemoryAccess));
      } else if (Info.isCall()) {
        EXPECT_TRUE(hasFaultPolicy(Traits.Faults, FaultPolicy::Call));
      } else {
        EXPECT_EQ(Traits.Faults, FaultPolicy::None);
      }

      const TerminatorKind ExpectedTerminator =
          Info.Op == Operation::Jump   ? TerminatorKind::Jump
          : Info.isConditionalBranch() ? TerminatorKind::ConditionalJump
          : Info.isCall()              ? TerminatorKind::Call
          : Info.isExit()              ? TerminatorKind::Return
                                       : TerminatorKind::None;
      EXPECT_EQ(Traits.Terminator, ExpectedTerminator);
    }
  }
}

TEST(SBFSemantics, SelectsTheVersionedCallXRegister) {
  EXPECT_EQ(callxRegisterIndex(Version::V0, 3, 4, 5), 5);
  EXPECT_EQ(callxRegisterIndex(Version::V1, 3, 4, 5), 5);
  EXPECT_EQ(callxRegisterIndex(Version::V2, 3, 4, 5), 4);
  EXPECT_EQ(callxRegisterIndex(Version::V3, 3, 4, 5), 3);
  EXPECT_EQ(callxRegisterIndex(Version::V4, 3, 4, 5), 3);
}

TEST(SBFSemantics, ClassifiesTypedHostSyscallOutcomes) {
  const SyscallOutcome Unregistered = SyscallOutcome::unregistered();
  EXPECT_EQ(Unregistered.kind(), SyscallOutcome::Kind::Unregistered);
  EXPECT_TRUE(Unregistered.representsUnregisteredSyscall());
  EXPECT_EQ(Unregistered.abiStatus(), FaultCode::UnknownSyscall);

  const SyscallOutcome Returned = SyscallOutcome::returned(42);
  EXPECT_EQ(Returned.kind(), SyscallOutcome::Kind::Returned);
  EXPECT_EQ(Returned.value(), 42u);
  EXPECT_FALSE(Returned.representsUnregisteredSyscall());
  EXPECT_EQ(Returned.abiStatus(), FaultCode::None);

  const SyscallOutcome Fault = SyscallOutcome::fault(FaultCode::MemoryAccess);
  EXPECT_EQ(Fault.kind(), SyscallOutcome::Kind::Fault);
  EXPECT_EQ(Fault.faultCode(), FaultCode::MemoryAccess);
  EXPECT_FALSE(Fault.representsUnregisteredSyscall());
  EXPECT_EQ(Fault.abiStatus(), FaultCode::MemoryAccess);

  const SyscallOutcome Unknown =
      SyscallOutcome::fault(FaultCode::UnknownSyscall);
  EXPECT_TRUE(Unknown.representsUnregisteredSyscall());
  EXPECT_EQ(Unknown.abiStatus(), FaultCode::UnknownSyscall);

  for (FaultCode Invalid :
       {FaultCode::None, static_cast<FaultCode>(UINT32_MAX)}) {
    const SyscallOutcome Normalized = SyscallOutcome::fault(Invalid);
    EXPECT_EQ(Normalized.kind(), SyscallOutcome::Kind::Fault);
    EXPECT_EQ(Normalized.faultCode(), FaultCode::InvalidInstruction);
    EXPECT_EQ(Normalized.abiStatus(), FaultCode::InvalidInstruction);
    EXPECT_FALSE(Normalized.representsUnregisteredSyscall());
  }
}

TEST(SBFInterpreter, DeliversTheResolvedRuntimeFeatureSnapshotToTheHost) {
  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  SBFProgram Program = analyzeProgram(
      Version::V3,
      {encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)});
  Program.ActiveRuntimeFeatures =
      RuntimeFeature::EnableSBPFV2 |
      RuntimeFeature::VirtualAddressSpaceAdjustments |
      RuntimeFeature::DisableAllocFreeDeployment;

  std::optional<RuntimeFeature> Observed;
  ExecutionEnvironment DefaultEnvironment;
  DefaultEnvironment.Syscall =
      [](uint32_t, const SyscallArguments &) -> std::optional<uint64_t> {
    ADD_FAILURE() << "feature-aware callback did not take precedence";
    return std::nullopt;
  };
  DefaultEnvironment.HostSyscall =
      [](uint32_t, const SyscallArguments &) -> SyscallOutcome {
    ADD_FAILURE() << "feature-aware callback did not take precedence";
    return SyscallOutcome::unregistered();
  };
  DefaultEnvironment.FeatureAwareSyscall =
      [&](const SyscallInvocation &Invocation) -> SyscallOutcome {
    EXPECT_EQ(Invocation.Hash, Log64->Hash);
    Observed = Invocation.RuntimeFeatures;
    return SyscallOutcome::returned(0);
  };
  auto DefaultResult = executeRaw(Program, std::move(DefaultEnvironment));
  ASSERT_TRUE(static_cast<bool>(DefaultResult))
      << llvm::toString(DefaultResult.takeError());
  ASSERT_TRUE(Observed.has_value());
  EXPECT_EQ(*Observed, Program.ActiveRuntimeFeatures);

  const RuntimeFeature CustomSnapshot = RuntimeFeature::None;
  ExecutionEnvironment CustomEnvironment;
  CustomEnvironment.RuntimeFeatures = CustomSnapshot;
  CustomEnvironment.FeatureAwareSyscall =
      [&](const SyscallInvocation &Invocation) -> SyscallOutcome {
    Observed = Invocation.RuntimeFeatures;
    return SyscallOutcome::returned(0);
  };
  auto CustomResult = executeRaw(Program, std::move(CustomEnvironment));
  ASSERT_TRUE(static_cast<bool>(CustomResult))
      << llvm::toString(CustomResult.takeError());
  ASSERT_TRUE(Observed.has_value());
  EXPECT_EQ(*Observed, CustomSnapshot);
}

TEST(SBFInterpreter, RequiresCallerProvidedDirectAccountMemoryRegions) {
  SBFProgram Program = analyzeProgram(
      Version::V3, {encode(Opcode::LD_DW_REG, 0, kFirstArgumentRegister),
                    encode(Opcode::EXIT)});
  Program.ActiveRuntimeFeatures = RuntimeFeature::AccountDataDirectMapping;

  auto MissingRegion = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(MissingRegion))
      << llvm::toString(MissingRegion.takeError());
  EXPECT_EQ(MissingRegion->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(MissingRegion->Fault, FaultCode::MemoryAccess);

  ExecutionEnvironment SuppliedRegion;
  SuppliedRegion.Memory.push_back(
      {kInputStart, {9, 0, 0, 0, 0, 0, 0, 0}, true, "account-data"});
  auto Mapped = executeRaw(Program, std::move(SuppliedRegion));
  ASSERT_TRUE(static_cast<bool>(Mapped)) << llvm::toString(Mapped.takeError());
  EXPECT_EQ(Mapped->Status, ExecutionStatus::Returned);
  EXPECT_EQ(Mapped->ReturnValue, 9u);
}

TEST(SBFInterpreter, ExecutesRawBytesIndependentlyOfAnalyzedInstructions) {
  constexpr size_t TargetSlot = 6;
  SBFProgram Program = analyzeProgram(
      Version::V3,
      {encode(Opcode::MOV64_IMM, 0, 0, 0, 5),
       encode(Opcode::CALL_IMM, 0, 1, 0, 4),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 7),
       encode(Opcode::JEQ64_IMM, 0, 0, 1, 19),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 99), encode(Opcode::EXIT),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 7), encode(Opcode::EXIT)});
  ASSERT_FALSE(Program.Med.Instructions.empty());
  Program.Med.Instructions.front().Immediate = 0xffff;
  Program.Low.Instructions.front().Info = nullptr;
  Program.Low.Instructions.front().InvalidReason =
      ValidationRule::UnknownOpcode;
  Program.Low.Instructions.front().Dst = 15;
  Program.Low.Instructions[1].Call = CallKind::Syscall;
  Program.Low.Instructions[1].CallTarget = 4;
  Program.Med.Instructions[1].Call = CallKind::Syscall;
  Program.Med.Instructions[1].CallTarget = 4;
  Program.Low.TheVersion = Version::V0;
  Program.Low.EntrySlot = 7;
  Program.Low.TextAddress = 0;

  auto Result = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
  EXPECT_EQ(Result->ReturnValue, 19u);
  ASSERT_EQ(Result->Trace.size(), 7u);
  EXPECT_EQ(Result->Trace.front().Address, kBytecodeStart);
  EXPECT_EQ(Result->Trace[2].Slot, TargetSlot);
  EXPECT_EQ(Result->Trace[5].Op, Opcode::JEQ64_IMM);
}

TEST(SBFInterpreter, ExecutesLoaderRelocatedLegacyRelativeCalls) {
  SBFProgram Program = analyzeProgram(
      Version::V0,
      {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)});
  ASSERT_EQ(Program.Low.Instructions.size(), 4u);
  EXPECT_EQ(Program.Low.Instructions[0].Call, CallKind::Internal);
  EXPECT_EQ(Program.Low.Instructions[0].CallTarget, 2u);

  auto Result = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned) << Result->Error;
  EXPECT_EQ(Result->ReturnValue, 42u);
}

TEST(SBFInterpreter, StaticCallsAcceptAnyInRangeSlotBeforeTheNextFetch) {
  SBFProgram Program =
      analyzeProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                                   encode(Opcode::LDDW),
                                   {},
                                   encode(Opcode::EXIT)});

  auto Result = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->Fault, FaultCode::InvalidInstruction);
  EXPECT_EQ(Result->FinalSlot, 2u);
  EXPECT_EQ(Result->Steps, 2u);
  EXPECT_EQ(Result->MaxCallDepth, 1u);
  ASSERT_EQ(Result->Trace.size(), 2u);
  EXPECT_EQ(Result->Trace.back().Slot, 2u);
  EXPECT_EQ(Result->Trace.back().RawOpcode, 0u);
  EXPECT_EQ(Result->Trace.back().Op, Opcode::Unknown);
}

TEST(SBFInterpreter, CallXContinuationFetchHonorsMeterPrecedence) {
  EncodedInstruction Continuation{};
  llvm::support::endian::write32le(
      Continuation.data() + kImmediateOffset,
      static_cast<uint32_t>(kBytecodeStart >> kWordBitWidth));
  SBFProgram Program = analyzeProgram(
      Version::V3,
      {encode(Opcode::LDDW, 2, 0, 0, static_cast<int32_t>(kInstructionSize)),
       Continuation, encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT)});

  InterpreterOptions MeterFirst;
  MeterFirst.MaxSteps = 2;
  auto Metered = executeRaw(Program, {}, MeterFirst);
  ASSERT_TRUE(static_cast<bool>(Metered))
      << llvm::toString(Metered.takeError());
  EXPECT_EQ(Metered->Status, ExecutionStatus::StepLimit);
  EXPECT_EQ(Metered->Fault, FaultCode::ExecutionOverrun);
  EXPECT_EQ(Metered->Steps, 2u);
  ASSERT_EQ(Metered->Trace.size(), 2u);
  EXPECT_EQ(Metered->Trace.back().Slot, 2u);

  InterpreterOptions FetchAtBoundary;
  FetchAtBoundary.MaxSteps = 3;
  auto Fetched = executeRaw(Program, {}, FetchAtBoundary);
  ASSERT_TRUE(static_cast<bool>(Fetched))
      << llvm::toString(Fetched.takeError());
  EXPECT_EQ(Fetched->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Fetched->Fault, FaultCode::InvalidInstruction);
  EXPECT_EQ(Fetched->Steps, 3u);
  ASSERT_EQ(Fetched->Trace.size(), 3u);
  EXPECT_EQ(Fetched->Trace.back().Slot, 1u);
  EXPECT_EQ(Fetched->Trace.back().RawOpcode, 0u);
  EXPECT_EQ(Fetched->Trace.back().Op, Opcode::Unknown);
}

TEST(SBFInterpreter, StaticCallDepthPrecedesAContinuationFetchFault) {
  SBFProgram Program =
      analyzeProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                                   encode(Opcode::LDDW),
                                   {},
                                   encode(Opcode::EXIT)});
  Program.Config.MaxCallDepth = 1;

  auto Result = executeRaw(Program);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->Fault, FaultCode::CallDepth);
  EXPECT_EQ(Result->FinalSlot, 0u);
  EXPECT_EQ(Result->Steps, 1u);
}

TEST(SBFInterpreter, UnsupportedStaticCallsDoNotPushAFrame) {
  auto ExpectUnsupported = [](SBFProgram Program) {
    Program.Config.MaxCallDepth = 1;
    auto Result = executeRaw(Program);
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
    EXPECT_EQ(Result->Fault, FaultCode::InvalidInstruction);
    EXPECT_EQ(Result->FinalSlot, 0u);
    EXPECT_EQ(Result->Steps, 1u);
    EXPECT_EQ(Result->MaxCallDepth, 0u);
  };

  ExpectUnsupported(
      analyzeProgram(Version::V3, {encode(Opcode::CALL_IMM, 0, 1, 0, 100),
                                   encode(Opcode::EXIT)}));
  ExpectUnsupported(analyzeProgram(
      Version::V3, {encode(Opcode::CALL_IMM, 0, 2), encode(Opcode::EXIT)}));
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

TEST(SBFInterpreter, PropagatesHandledHostSyscallFaults) {
  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  SBFProgram Program = analyzeProgram(
      Version::V3,
      {encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
       encode(Opcode::EXIT)});

  ExecutionEnvironment Environment;
  Environment.HostSyscall = [](uint32_t,
                               const SyscallArguments &) -> SyscallOutcome {
    return SyscallOutcome::fault(FaultCode::MemoryAccess);
  };

  auto Result = executeRaw(Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->Fault, FaultCode::MemoryAccess);
  EXPECT_EQ(Result->FinalSlot, 0u);
  EXPECT_TRUE(Result->Syscalls.empty());
}

TEST(SBFInterpreter, DoesNotHideHandledFaultBehindLegacyFunctionCollision) {
  constexpr size_t TargetSlot = 4;
  const uint32_t Key = legacyFunctionKey(TargetSlot, {});
  SBFProgram Program = analyzeProgram(
      Version::V0,
      {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
       encode(Opcode::CALL_IMM, 0, 0, 0, 2),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 7), encode(Opcode::EXIT)});
  ASSERT_EQ(Program.Low.Instructions[1].SyscallHash, Key);
  ASSERT_EQ(Program.Low.Instructions[1].CallTarget,
            std::optional<size_t>(TargetSlot));
  ASSERT_EQ(Program.Low.Instructions[1].Dispatch,
            CallDispatchPolicy::LegacyRuntimeThenFunction);

  ExecutionEnvironment Environment;
  Environment.HostSyscall = [Key](uint32_t Hash,
                                  const SyscallArguments &) -> SyscallOutcome {
    return Hash == Key ? SyscallOutcome::fault(FaultCode::MemoryAccess)
                       : SyscallOutcome::unregistered();
  };

  auto Result = executeRaw(Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Result->Fault, FaultCode::MemoryAccess);
  EXPECT_EQ(Result->FinalSlot, 1u);
  EXPECT_TRUE(Result->Syscalls.empty());
}

TEST(SBFInterpreter, LegacyReturnedSyscallStillInvokesCollidingFunction) {
  constexpr size_t TargetSlot = 4;
  const uint32_t Key = legacyFunctionKey(TargetSlot, {});
  SBFProgram Program = analyzeProgram(
      Version::V0,
      {encode(Opcode::MOV64_IMM, 1, 0, 0, 41),
       encode(Opcode::CALL_IMM, 0, 0, 0, 2),
       encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 7), encode(Opcode::EXIT)});
  ASSERT_EQ(Program.Low.Instructions[1].SyscallHash, Key);
  ASSERT_EQ(Program.Low.Instructions[1].CallTarget,
            std::optional<size_t>(TargetSlot));

  ExecutionEnvironment Environment;
  Environment.HostSyscall = [Key](uint32_t Hash,
                                  const SyscallArguments &) -> SyscallOutcome {
    return Hash == Key ? SyscallOutcome::returned(42)
                       : SyscallOutcome::unregistered();
  };

  auto Result = executeRaw(Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Status, ExecutionStatus::Returned);
  EXPECT_EQ(Result->ReturnValue, 8u);
  EXPECT_EQ(Result->Steps, 6u);
  ASSERT_EQ(Result->Syscalls.size(), 1u);
  EXPECT_EQ(Result->Syscalls.front().Result, 42u);
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

  Options.MaxSteps = kDefaultMaxExecutionSteps;
  Options.MaxCallDepth = Loop.Config.MaxCallDepth + 1;
  auto InvalidLimit = executeRaw(Loop, {}, Options);
  ASSERT_FALSE(static_cast<bool>(InvalidLimit));
  EXPECT_NE(llvm::toString(InvalidLimit.takeError()).find("exceeds"),
            std::string::npos);
}

TEST(SBFInterpreter,
     RejectsCallDepthBeyondTheHostResourceEnvelopeBeforeAllocation) {
  SBFProgram Program = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  Program.Config.StackFrameSize = 1;
  Program.Config.MaxCallDepth = kMaximumHostCallDepth + 1;

  auto Result = executeRaw(Program);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("host call-depth limit"),
            std::string::npos);
}

TEST(SBFInterpreter,
     RejectsStackBytesBeyondTheHostResourceEnvelopeBeforeAllocation) {
  SBFProgram Program = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  Program.Config.StackFrameSize = kMaximumHostStackByteCount + 1;
  Program.Config.MaxCallDepth = 1;

  auto Result = executeRaw(Program);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("host stack-byte limit"),
            std::string::npos);
}

TEST(SBFInterpreter, AcceptsTheExactHostVMResourceEnvelopeBoundaries) {
  SBFProgram MaximumDepth = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  MaximumDepth.Config.StackFrameSize = 1;
  MaximumDepth.Config.MaxCallDepth = kMaximumHostCallDepth;
  auto DepthResult = executeRaw(MaximumDepth);
  ASSERT_TRUE(static_cast<bool>(DepthResult))
      << llvm::toString(DepthResult.takeError());
  EXPECT_EQ(DepthResult->Status, ExecutionStatus::Returned);

  SBFProgram MaximumStack = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  MaximumStack.Config.StackFrameSize = kMaximumHostStackByteCount;
  MaximumStack.Config.MaxCallDepth = 1;
  auto StackResult = executeRaw(MaximumStack);
  ASSERT_TRUE(static_cast<bool>(StackResult))
      << llvm::toString(StackResult.takeError());
  EXPECT_EQ(StackResult->Status, ExecutionStatus::Returned);
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
  V3.Config.AlignedMemoryMapping = false;
  auto Accepted = executeRaw(V3, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->Status, ExecutionStatus::Returned);
}

TEST(SBFInterpreter, HonorsTheRuntimeAlignedMemoryMappingPolicy) {
  ExecutionEnvironment DuplicateIndex;
  DuplicateIndex.Memory.push_back({kInputStart, {1}, false, "input.first"});
  DuplicateIndex.Memory.push_back(
      {kInputStart + kInstructionSize, {2}, false, "input.second"});

  SBFProgram AlignedV3 = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  AlignedV3.Config.AlignedMemoryMapping = true;
  auto Rejected = executeRaw(AlignedV3, DuplicateIndex);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("aligned index"),
            std::string::npos);

  SBFProgram UnalignedV3 = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  UnalignedV3.Config.AlignedMemoryMapping = false;
  auto Accepted = executeRaw(UnalignedV3, std::move(DuplicateIndex));
  ASSERT_TRUE(static_cast<bool>(Accepted))
      << llvm::toString(Accepted.takeError());
  EXPECT_EQ(Accepted->Status, ExecutionStatus::Returned);

  ExecutionEnvironment BoundaryAndOverlap;
  BoundaryAndOverlap.Memory.push_back(
      {kInputStart + kMemoryRegionSize - 1, {1, 2}, false, "crossing"});
  BoundaryAndOverlap.Memory.push_back(
      {kInputStart + kMemoryRegionSize - 1, {3}, false, "overlap"});
  auto OrderedFault = executeRaw(AlignedV3, std::move(BoundaryAndOverlap));
  ASSERT_FALSE(static_cast<bool>(OrderedFault));
  EXPECT_NE(llvm::toString(OrderedFault.takeError()).find("boundary"),
            std::string::npos);
}

TEST(SBFInterpreter, RejectsNonPowerOfTwoGappedMemoryRegions) {
  SBFProgram Program = analyzeProgram(Version::V3, {encode(Opcode::EXIT)});
  ExecutionEnvironment Environment;
  MemoryRegion InvalidGap;
  InvalidGap.Address = kInputStart;
  InvalidGap.Bytes.resize(12);
  InvalidGap.VMGapSize = 6;
  InvalidGap.Name = "invalid-gap";
  Environment.Memory.push_back(std::move(InvalidGap));

  auto Result = executeRaw(Program, std::move(Environment));
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("power of two"),
            std::string::npos);
}

} // namespace
} // namespace neverd::sbf
