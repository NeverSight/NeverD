//===- GuestStateTests.cpp - Cross-architecture state contracts ----------===//

#include "gtest/gtest.h"

#include "neverd/translate/GuestState.h"
#include "neverd/translate/TranslationOptions.h"
#include "neverd/translate/TranslationResult.h"
#include "neverd/translate/TranslationSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

#include <algorithm>

using namespace neverd::translate;

namespace {

TEST(GuestArchitecture, DescriptionsPinModeledBaselineState) {
  const ArchitectureDescription *X64 =
      getArchitectureDescription(GuestArchitecture::X86_64);
  const ArchitectureDescription *A64 =
      getArchitectureDescription(GuestArchitecture::AArch64);
  const ArchitectureDescription *ARM =
      getArchitectureDescription(GuestArchitecture::ARM32);
  const ArchitectureDescription *X86 =
      getArchitectureDescription(GuestArchitecture::X86_32);

  ASSERT_NE(X64, nullptr);
  ASSERT_NE(A64, nullptr);
  ASSERT_NE(ARM, nullptr);
  ASSERT_NE(X86, nullptr);
  EXPECT_EQ(X64->AddressWidth, 64u);
  EXPECT_EQ(A64->AddressWidth, 64u);
  EXPECT_EQ(ARM->AddressWidth, 32u);
  EXPECT_EQ(X86->AddressWidth, 32u);
  EXPECT_EQ(X64->ByteOrder, GuestEndianness::Little);
  EXPECT_EQ(A64->ByteOrder, GuestEndianness::Little);
  EXPECT_EQ(ARM->ByteOrder, GuestEndianness::Little);
  EXPECT_EQ(X86->ByteOrder, GuestEndianness::Little);

  for (const ArchitectureDescription *Description : {X64, A64, ARM, X86}) {
    EXPECT_NE(findRegister(*Description, Description->ProgramCounter), nullptr);
    EXPECT_NE(findRegister(*Description, Description->StackPointer), nullptr);

    bool HasFlags = false;
    bool HasVector = false;
    for (const RegisterDescription &Register : Description->Registers) {
      HasFlags |= Register.Kind == RegisterKind::Flags;
      HasVector |= Register.Kind == RegisterKind::Vector;
    }
    EXPECT_TRUE(HasFlags);
    EXPECT_TRUE(HasVector);
  }

  EXPECT_EQ(getArchitectureDescription(static_cast<GuestArchitecture>(0xff)),
            nullptr);
}

TEST(GuestState, WireV1ZeroX86_32MatchesTheImmutableGoldenFixture) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::X86_32);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  llvm::Expected<std::vector<uint8_t>> WireOrErr =
      serializeGuestState(*StateOrErr);
  ASSERT_TRUE(static_cast<bool>(WireOrErr))
      << llvm::toString(WireOrErr.takeError());

  constexpr llvm::StringLiteral ExpectedHex =
      "4e5644535441544501000000400000000401000020000000000000000000000000000000"
      "000000001300000000000000"
      "000000000000000000000000000000000000000020000000000000000000000004000000"
      "000000000000000001000000"
      "200000000000000000000000040000000000000000000000020000002000000000000000"
      "000000000400000000000000"
      "000000000300000020000000000000000000000004000000000000000000000004000000"
      "200000000000000000000000"
      "040000000000000000000000050000002000000000000000000000000400000000000000"
      "000000000600000020000000"
      "000000000000000004000000000000000000000007000000200000000000000000000000"
      "040000000000000000000000"
      "080000002000000000000000000000000400000000000000000000000900000020000000"
      "000000000000000004000000"
      "00000000000000000a000000200000000000000000000000040000000000000000000000"
      "200000008000000000000000"
      "000000001000000000000000000000000000000000000000000000002100000080000000"
      "000000000000000010000000"
      "000000000000000000000000000000000000000022000000800000000000000000000000"
      "100000000000000000000000"
      "000000000000000000000000230000008000000000000000000000001000000000000000"
      "000000000000000000000000"
      "000000002400000080000000000000000000000010000000000000000000000000000000"
      "000000000000000025000000"
      "800000000000000000000000100000000000000000000000000000000000000000000000"
      "260000008000000000000000"
      "000000001000000000000000000000000000000000000000000000002700000080000000"
      "000000000000000010000000"
      "0000000000000000000000000000000000000000";
  EXPECT_EQ(llvm::toHex(*WireOrErr, true), ExpectedHex);
}

TEST(GuestState, X86_32CoreStateRoundTripsWithoutHostLayout) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::X86_32, 9);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  GuestState State = std::move(*StateOrErr);
  const ArchitectureDescription *Description =
      getArchitectureDescription(State.Architecture);
  ASSERT_NE(Description, nullptr);
  ASSERT_NE(findRegister(*Description, "eax"), nullptr);
  ASSERT_NE(findRegister(*Description, "eflags"), nullptr);
  ASSERT_NE(findRegister(*Description, "mxcsr"), nullptr);
  ASSERT_NE(findRegister(*Description, "xmm7"), nullptr);
  ASSERT_FALSE(static_cast<bool>(setRegisterValue(
      State, Description->ProgramCounter, llvm::APInt(32, 0x8048000))));

  llvm::Expected<std::vector<uint8_t>> WireOrErr = serializeGuestState(State);
  ASSERT_TRUE(static_cast<bool>(WireOrErr))
      << llvm::toString(WireOrErr.takeError());
  llvm::Expected<GuestState> DecodedOrErr = deserializeGuestState(*WireOrErr);
  ASSERT_TRUE(static_cast<bool>(DecodedOrErr))
      << llvm::toString(DecodedOrErr.takeError());
  EXPECT_EQ(DecodedOrErr->Architecture, GuestArchitecture::X86_32);
  EXPECT_EQ(
      findRegisterValue(*DecodedOrErr, Description->ProgramCounter)->Value,
      llvm::APInt(32, 0x8048000));
}

TEST(GuestState, InvalidX86_32StateFailsBeforeSerialization) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::X86_32);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  GuestState State = std::move(*StateOrErr);

  State.Registers.pop_back();
  llvm::Expected<std::vector<uint8_t>> MissingRegister =
      serializeGuestState(State);
  ASSERT_FALSE(static_cast<bool>(MissingRegister));
  EXPECT_NE(
      llvm::toString(MissingRegister.takeError()).find("missing register"),
      std::string::npos);

  llvm::Expected<GuestState> FreshOrErr =
      createZeroedGuestState(GuestArchitecture::X86_32);
  ASSERT_TRUE(static_cast<bool>(FreshOrErr))
      << llvm::toString(FreshOrErr.takeError());
  State = std::move(*FreshOrErr);
  State.ExecutionMode = GuestExecutionMode::Thumb;
  llvm::Error InvalidMode = validateGuestState(State);
  EXPECT_NE(
      llvm::toString(std::move(InvalidMode)).find("execution mode is invalid"),
      std::string::npos);

  State.ExecutionMode = GuestExecutionMode::Default;
  State.Memory.push_back({0xffffffff, MemoryPermission::Read, 0, {0x01, 0x02}});
  llvm::Error OutOfRange = validateGuestState(State);
  EXPECT_NE(llvm::toString(std::move(OutOfRange))
                .find("exceeds the guest address space"),
            std::string::npos);
}

TEST(GuestState, ZeroStateCoversBaselineAndAcceptsNamedExtensions) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::AArch64, 17);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  GuestState State = std::move(*StateOrErr);

  const ArchitectureDescription *Description =
      getArchitectureDescription(State.Architecture);
  ASSERT_NE(Description, nullptr);
  EXPECT_EQ(State.ThreadID, 17u);
  EXPECT_EQ(State.Registers.size(), Description->Registers.size());
  EXPECT_FALSE(static_cast<bool>(validateGuestState(State)))
      << "a factory-created baseline must satisfy its public contract";

  const llvm::APInt ExtensionValue(257, 1);
  EXPECT_FALSE(static_cast<bool>(setRegisterValue(
      State, kFirstExtensionRegisterID, ExtensionValue, "vendor.trace")));
  const GuestRegisterValue *Extension =
      findRegisterValue(State, kFirstExtensionRegisterID);
  ASSERT_NE(Extension, nullptr);
  EXPECT_EQ(Extension->ExtensionName, "vendor.trace");
  EXPECT_EQ(Extension->Value, ExtensionValue);

  llvm::Error MissingName =
      setRegisterValue(State, kFirstExtensionRegisterID + 1, llvm::APInt(8, 0));
  EXPECT_EQ(llvm::toString(std::move(MissingName)),
            "extension register name is empty");

  llvm::Error ZeroWidth =
      setRegisterValue(State, kFirstExtensionRegisterID + 2,
                       llvm::APInt::getZeroWidth(), "invalid.zero");
  EXPECT_EQ(llvm::toString(std::move(ZeroWidth)),
            "extension register bit width is zero");

  llvm::Error MixedCase = setRegisterValue(State, kFirstExtensionRegisterID + 3,
                                           llvm::APInt(8, 0), "Vendor.Trace");
  EXPECT_NE(llvm::toString(std::move(MixedCase)).find("lower-case ASCII"),
            std::string::npos);
}

TEST(GuestState, ARM32ModeStatusAndProgramCounterStayCoherent) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::ARM32);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  GuestState State = std::move(*StateOrErr);

  State.ExecutionMode = GuestExecutionMode::Thumb;
  llvm::Error MissingThumbBit = validateGuestState(State);
  EXPECT_NE(llvm::toString(std::move(MissingThumbBit)).find("CPSR.T"),
            std::string::npos);

  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(State, 16, llvm::APInt(32, uint64_t{1} << 5))));
  EXPECT_FALSE(static_cast<bool>(validateGuestState(State)));

  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(State, 15, llvm::APInt(32, 0x1001))));
  llvm::Error TaggedPC = validateGuestState(State);
  EXPECT_NE(llvm::toString(std::move(TaggedPC)).find("canonical"),
            std::string::npos);

  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(State, 15, llvm::APInt(32, 0x1000))));
  State.ExecutionMode = GuestExecutionMode::ARM;
  llvm::Error StaleThumbBit = validateGuestState(State);
  EXPECT_NE(llvm::toString(std::move(StaleThumbBit)).find("CPSR.T"),
            std::string::npos);

  ASSERT_FALSE(static_cast<bool>(setRegisterValue(State, 16, llvm::APInt(32, 0))));
  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(State, 15, llvm::APInt(32, 0x1002))));
  llvm::Error MisalignedARMPC = validateGuestState(State);
  EXPECT_NE(llvm::toString(std::move(MisalignedARMPC)).find("word aligned"),
            std::string::npos);
}

TEST(GuestState, VersionedWireFormatIsCanonicalAndFailClosed) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::ARM32, 0x1234);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  GuestState State = std::move(*StateOrErr);

  const ArchitectureDescription *Description =
      getArchitectureDescription(State.Architecture);
  ASSERT_NE(Description, nullptr);
  State.ExecutionMode = GuestExecutionMode::Thumb;
  State.Features = {"vfpv3", "thumb2"};
  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(State, 16, llvm::APInt(32, uint64_t{1} << 5))));
  ASSERT_FALSE(static_cast<bool>(setRegisterValue(
      State, Description->ProgramCounter, llvm::APInt(32, 0x1000))));
  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(State, kFirstExtensionRegisterID + 9,
                       llvm::APInt(9, 0x101), "debug.itstate")));
  State.Memory.push_back({0x2000,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          7,
                          {0x01, 0x02, 0x03}});
  State.Memory.push_back({0x1000,
                          MemoryPermission::Read | MemoryPermission::Write,
                          3,
                          {0xaa, 0xbb}});
  State.Exception = GuestExceptionState{
      GuestExceptionKind::HardwareFault, 11, 0x2001, 0x1001, {0xde, 0xad}};

  llvm::Expected<std::vector<uint8_t>> WireOrErr = serializeGuestState(State);
  ASSERT_TRUE(static_cast<bool>(WireOrErr))
      << llvm::toString(WireOrErr.takeError());
  const std::vector<uint8_t> Wire = *WireOrErr;
  ASSERT_GE(Wire.size(), 64u);
  EXPECT_EQ(std::string(Wire.begin(), Wire.begin() + 8), "NVDSTATE");
  EXPECT_EQ(Wire[8], kGuestStateWireVersion);
  EXPECT_EQ(Wire[9], 0u);
  EXPECT_EQ(Wire[10], 0u);
  EXPECT_EQ(Wire[11], 0u);

  std::reverse(State.Registers.begin(), State.Registers.end());
  std::reverse(State.Memory.begin(), State.Memory.end());
  llvm::Expected<std::vector<uint8_t>> ReorderedOrErr =
      serializeGuestState(State);
  ASSERT_TRUE(static_cast<bool>(ReorderedOrErr))
      << llvm::toString(ReorderedOrErr.takeError());
  EXPECT_EQ(*ReorderedOrErr, Wire);

  llvm::Expected<GuestState> DecodedOrErr = deserializeGuestState(Wire);
  ASSERT_TRUE(static_cast<bool>(DecodedOrErr))
      << llvm::toString(DecodedOrErr.takeError());
  EXPECT_EQ(DecodedOrErr->Architecture, GuestArchitecture::ARM32);
  EXPECT_EQ(DecodedOrErr->ExecutionMode, GuestExecutionMode::Thumb);
  EXPECT_EQ(DecodedOrErr->Features,
            (std::vector<std::string>{"thumb2", "vfpv3"}));
  EXPECT_EQ(DecodedOrErr->ThreadID, 0x1234u);
  ASSERT_EQ(DecodedOrErr->Memory.size(), 2u);
  EXPECT_EQ(DecodedOrErr->Memory[0].Address, 0x1000u);
  ASSERT_TRUE(DecodedOrErr->Exception.has_value());
  EXPECT_EQ(DecodedOrErr->Exception->Payload,
            (std::vector<uint8_t>{0xde, 0xad}));
  const GuestRegisterValue *Extension =
      findRegisterValue(*DecodedOrErr, kFirstExtensionRegisterID + 9);
  ASSERT_NE(Extension, nullptr);
  EXPECT_EQ(Extension->Value, llvm::APInt(9, 0x101));

  std::vector<uint8_t> FutureVersion = Wire;
  FutureVersion[8] = 2;
  llvm::Expected<GuestState> FutureOrErr = deserializeGuestState(FutureVersion);
  ASSERT_FALSE(static_cast<bool>(FutureOrErr));
  EXPECT_NE(llvm::toString(FutureOrErr.takeError())
                .find("unsupported guest-state wire version"),
            std::string::npos);

  std::vector<uint8_t> Trailing = Wire;
  Trailing.push_back(0);
  llvm::Expected<GuestState> TrailingOrErr = deserializeGuestState(Trailing);
  ASSERT_FALSE(static_cast<bool>(TrailingOrErr));
  EXPECT_NE(llvm::toString(TrailingOrErr.takeError()).find("trailing bytes"),
            std::string::npos);

  GuestStateWireLimits WireBudget;
  WireBudget.MaxWireBytes = Wire.size() - 1;
  llvm::Expected<GuestState> TooLarge = deserializeGuestState(Wire, WireBudget);
  ASSERT_FALSE(static_cast<bool>(TooLarge));
  EXPECT_NE(llvm::toString(TooLarge.takeError()).find("byte budget"),
            std::string::npos);

  GuestStateWireLimits RegisterBudget;
  RegisterBudget.MaxRegisterBits = 31;
  llvm::Expected<GuestState> WideRegister =
      deserializeGuestState(Wire, RegisterBudget);
  ASSERT_FALSE(static_cast<bool>(WideRegister));
  EXPECT_NE(llvm::toString(WideRegister.takeError()).find("register width"),
            std::string::npos);

  GuestStateWireLimits MemoryBudget;
  MemoryBudget.MaxGuestMemoryBytes = 4;
  llvm::Expected<GuestState> TooMuchMemory =
      deserializeGuestState(Wire, MemoryBudget);
  ASSERT_FALSE(static_cast<bool>(TooMuchMemory));
  EXPECT_NE(llvm::toString(TooMuchMemory.takeError()).find("memory bytes"),
            std::string::npos);
}

TEST(TranslationOptions, MatrixAndPolicyValidationAreExplicit) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  EXPECT_EQ(Options.Target.Kind, HostTargetKind::Native);
  EXPECT_EQ(
      getTranslationPairSupport(Options.Guest, GuestArchitecture::AArch64),
      TranslationPairSupport::ContractDefined);
  EXPECT_FALSE(static_cast<bool>(validateTranslationOptions(Options)));
  EXPECT_EQ(Options.InstructionBudget, 0u);
  EXPECT_EQ(Options.BlockBudget, 0u);
  EXPECT_EQ(Options.GeneratedCodeByteBudget, 0u);

  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = GuestArchitecture::ARM32;
  Options.Target.Triple = "armv7-unknown-linux-gnueabihf";
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  EXPECT_EQ(
      getTranslationPairSupport(Options.Guest, *Options.Target.Architecture),
      TranslationPairSupport::Unsupported);
  llvm::Error Unsupported = validateTranslationOptions(Options);
  EXPECT_NE(llvm::toString(std::move(Unsupported))
                .find("unsupported translation matrix cell"),
            std::string::npos);

  Options.Guest = GuestArchitecture::AArch64;
  Options.Target.Architecture = GuestArchitecture::X86_64;
  Options.Target.Triple = "x86_64-unknown-linux-gnu";
  Options.UnsupportedInstructions =
      UnsupportedInstructionPolicy::InterpreterFallback;
  llvm::Error InvalidFallback = validateTranslationOptions(Options);
  EXPECT_NE(llvm::toString(std::move(InvalidFallback))
                .find("AOT translation cannot request interpreter fallback"),
            std::string::npos);

  Options.Target.Features = {"+sse2", "-sse2"};
  llvm::Error ConflictingFeatures = validateTranslationOptions(Options);
  EXPECT_NE(llvm::toString(std::move(ConflictingFeatures))
                .find("duplicate or conflicting features"),
            std::string::npos);

  EXPECT_EQ(getTranslationPairSupport(GuestArchitecture::X86_32,
                                      GuestArchitecture::AArch64),
            TranslationPairSupport::ContractDefined);
  EXPECT_EQ(getTranslationPairSupport(GuestArchitecture::X86_32,
                                      GuestArchitecture::ARM32),
            TranslationPairSupport::ContractDefined);
  EXPECT_EQ(getTranslationPairSupport(GuestArchitecture::ARM32,
                                      GuestArchitecture::X86_32),
            TranslationPairSupport::ContractDefined);
  EXPECT_EQ(getTranslationPairSupport(GuestArchitecture::ARM32,
                                      GuestArchitecture::X86_64),
            TranslationPairSupport::ContractDefined);

  TranslationOptions NativeOnly;
  NativeOnly.Target.Kind = HostTargetKind::Explicit;
  NativeOnly.Target.Architecture = GuestArchitecture::AArch64;
  NativeOnly.Target.Triple = "aarch64-unknown-linux-gnu";
  llvm::Error ExplicitJIT = validateTranslationOptions(NativeOnly);
  EXPECT_NE(llvm::toString(std::move(ExplicitJIT))
                .find("JIT translation only accepts the native process target"),
            std::string::npos);

  TranslationOptions SIMD;
  SIMD.RequiredCapabilities = TranslationCapability::SIMD;
  llvm::Error UnsupportedCapability = validateTranslationOptions(SIMD);
  EXPECT_NE(llvm::toString(std::move(UnsupportedCapability))
                .find("capability is unsupported"),
            std::string::npos);
  EXPECT_EQ(
      getInitialTranslationCapability(TranslationCapability::ScalarInteger),
      TranslationCapabilityStatus::ContractDefined);
  for (TranslationCapability Capability :
       {TranslationCapability::FloatingPoint, TranslationCapability::SIMD,
        TranslationCapability::X87, TranslationCapability::Atomics,
        TranslationCapability::SystemInstructions})
    EXPECT_EQ(getInitialTranslationCapability(Capability),
              TranslationCapabilityStatus::Unsupported);
}

TEST(TranslationResult, StopReasonsAreStableAndStructured) {
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Returned),
               "returned");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Syscall),
               "syscall");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Exception),
               "exception");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Signal),
               "signal");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Breakpoint),
               "breakpoint");
  EXPECT_STREQ(
      translationStopReasonName(TranslationStopReason::UnsupportedInstruction),
      "unsupported-instruction");
  EXPECT_STREQ(
      translationStopReasonName(TranslationStopReason::SelfModification),
      "self-modification");
  EXPECT_STREQ(
      translationStopReasonName(TranslationStopReason::BudgetExhausted),
      "budget-exhausted");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Cancelled),
               "cancelled");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::InternalError),
               "internal-error");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::Dispatch),
               "dispatch");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::ExternalCall),
               "external-call");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::MemoryFault),
               "memory-fault");
  EXPECT_STREQ(translationStopReasonName(TranslationStopReason::JITUnavailable),
               "jit-unavailable");

  TranslationResult Result;
  Result.Guest = GuestArchitecture::X86_64;
  Result.Host = GuestArchitecture::AArch64;
  Result.Exit.Reason = TranslationStopReason::UnsupportedInstruction;
  Result.Exit.PC = 0x401000;
  Result.Exit.FallbackRequested = true;
  Result.Exit.Trap = TrapExit{TranslationTrapKind::UnsupportedInstruction, 0x0f,
                              0, 0x401000, true};
  EXPECT_TRUE(isResumableTranslationStop(Result.Exit.Reason));
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));

  Result.Exit = TranslationExit{};
  Result.Exit.Reason = TranslationStopReason::MemoryFault;
  Result.Exit.MemoryFault =
      MemoryFaultExit{0x402000, MemoryAccessKind::Read, 32, 4};
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));

  Result.Exit = TranslationExit{};
  Result.Exit.Reason = TranslationStopReason::SelfModification;
  Result.Exit.SelfModification = SelfModificationExit{0x403000, 4, 7, 8};
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));

  Result.Exit = TranslationExit{};
  Result.Exit.Reason = TranslationStopReason::BudgetExhausted;
  Result.GuestInstructions = 100;
  Result.Exit.Budget =
      BudgetExit{TranslationBudgetKind::GuestInstructions, 100, 100};
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));

  Result.Exit = TranslationExit{};
  Result.Exit.Reason = TranslationStopReason::Syscall;
  Result.Exit.Syscall = SyscallExit{60, {0}};
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));

  Result.Exit = TranslationExit{};
  Result.Exit.Reason = TranslationStopReason::ExternalCall;
  Result.Exit.ExternalCall = ExternalCallExit{0x404000, "runtime.helper", {1}};
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));

  Result.Exit = TranslationExit{};
  Result.Exit.Reason = TranslationStopReason::InternalError;
  llvm::Error MissingDiagnostic = validateTranslationResult(Result);
  EXPECT_NE(llvm::toString(std::move(MissingDiagnostic))
                .find("requires a diagnostic"),
            std::string::npos);

  Result.Exit.Reason = TranslationStopReason::JITUnavailable;
  Result.Exit.Diagnostic = "native host pair is unavailable";
  Result.Host = GuestArchitecture::X86_64;
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result)));
}

TEST(TranslationResult, ExitRangesAndBudgetCountersAreCrossChecked) {
  TranslationResult Result;
  Result.Guest = GuestArchitecture::X86_32;
  Result.Host = GuestArchitecture::AArch64;
  Result.Exit.Reason = TranslationStopReason::MemoryFault;
  Result.Exit.MemoryFault =
      MemoryFaultExit{0xffffffffu, MemoryAccessKind::Read, 16, 1};
  llvm::Error CrossingFault = validateTranslationResult(Result);
  EXPECT_NE(llvm::toString(std::move(CrossingFault))
                .find("memory-fault range exceeds"),
            std::string::npos);

  Result = TranslationResult{};
  Result.Guest = GuestArchitecture::X86_64;
  Result.Host = GuestArchitecture::AArch64;
  Result.GuestInstructions = 99;
  Result.Exit.Reason = TranslationStopReason::BudgetExhausted;
  Result.Exit.Budget =
      BudgetExit{TranslationBudgetKind::GuestInstructions, 100, 100};
  llvm::Error MismatchedCounter = validateTranslationResult(Result);
  EXPECT_NE(llvm::toString(std::move(MismatchedCounter))
                .find("does not match its result counter"),
            std::string::npos);
}

TEST(TranslationResult, RequestContextBindsFallbackAndBudgetPolicy) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;

  TranslationResult Result;
  Result.Guest = GuestArchitecture::X86_64;
  Result.Host = GuestArchitecture::AArch64;
  Result.Exit.Reason = TranslationStopReason::UnsupportedInstruction;
  Result.Exit.FallbackRequested = true;
  Result.Exit.Trap = TrapExit{TranslationTrapKind::UnsupportedInstruction, 0, 0,
                              0x401000, true};

  llvm::Error ForbiddenFallback = validateTranslationResult(Result, Options);
  EXPECT_NE(
      llvm::toString(std::move(ForbiddenFallback)).find("fallback forbidden"),
      std::string::npos);

  Options.UnsupportedInstructions =
      UnsupportedInstructionPolicy::InterpreterFallback;
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result, Options)));

  Result.Exit.Trap->Restartable = false;
  llvm::Error NonRestartable = validateTranslationResult(Result, Options);
  EXPECT_NE(llvm::toString(std::move(NonRestartable)).find("restartable"),
            std::string::npos);

  Result = TranslationResult{};
  Result.Guest = GuestArchitecture::X86_64;
  Result.Host = GuestArchitecture::AArch64;
  Result.GuestInstructions = 100;
  Result.Exit.Reason = TranslationStopReason::BudgetExhausted;
  Result.Exit.Budget =
      BudgetExit{TranslationBudgetKind::GuestInstructions, 100, 100};
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.InstructionBudget = 100;
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result, Options)));

  Options.InstructionBudget = 101;
  llvm::Error WrongLimit = validateTranslationResult(Result, Options);
  EXPECT_NE(llvm::toString(std::move(WrongLimit)).find("does not match"),
            std::string::npos);

  Options.InstructionBudget = 0;
  llvm::Error Unbounded = validateTranslationResult(Result, Options);
  EXPECT_NE(llvm::toString(std::move(Unbounded)).find("unbounded"),
            std::string::npos);

  Result = TranslationResult{};
  Result.Guest = GuestArchitecture::X86_64;
  Result.Host = GuestArchitecture::AArch64;
  Result.Exit.Reason = TranslationStopReason::Returned;
  Result.GuestInstructions = 101;
  Result.BlocksTranslated = 201;
  Result.GeneratedCodeBytes = 301;
  Options.InstructionBudget = 100;
  Options.BlockBudget = 200;
  Options.GeneratedCodeByteBudget = 300;
  llvm::Error InstructionOverrun =
      validateTranslationResult(Result, Options);
  EXPECT_NE(llvm::toString(std::move(InstructionOverrun))
                .find("guest-instruction count exceeds"),
            std::string::npos);

  Result.GuestInstructions = 100;
  llvm::Error BlockOverrun = validateTranslationResult(Result, Options);
  EXPECT_NE(llvm::toString(std::move(BlockOverrun))
                .find("translated-block count exceeds"),
            std::string::npos);

  Result.BlocksTranslated = 200;
  llvm::Error CodeOverrun = validateTranslationResult(Result, Options);
  EXPECT_NE(llvm::toString(std::move(CodeOverrun))
                .find("generated-code byte count exceeds"),
            std::string::npos);

  Result.GeneratedCodeBytes = 300;
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(Result, Options)));
}

TEST(TranslationSession, RequestValidationBindsPolicyToGuestState) {
  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::X86_64);
  ASSERT_TRUE(static_cast<bool>(StateOrErr))
      << llvm::toString(StateOrErr.takeError());
  GuestState State = std::move(*StateOrErr);
  State.Memory.push_back({0x400000,
                          MemoryPermission::Read | MemoryPermission::Write |
                              MemoryPermission::Execute,
                          1,
                          {0x90}});

  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  EXPECT_FALSE(static_cast<bool>(validateTranslationRequest(Options, State)));

  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  llvm::Error WritableCode = validateTranslationRequest(Options, State);
  EXPECT_NE(llvm::toString(std::move(WritableCode))
                .find("writable executable guest memory"),
            std::string::npos);

  Options.CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  State.Architecture = GuestArchitecture::ARM32;
  llvm::Error MismatchedState = validateTranslationRequest(Options, State);
  EXPECT_NE(llvm::toString(std::move(MismatchedState))
                .find("state architecture does not match translation options"),
            std::string::npos);
}

} // namespace
