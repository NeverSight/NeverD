//===- RuntimeX87HelpersTests.cpp - Terminal x87 translation protocol -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "neverd/translate/X86/RuntimeX87Helpers.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

constexpr uint32_t Load80 =
    0x28db | X87ContextMemory | (0x28u << X87ContextOriginalModRMShift);
constexpr uint32_t Store80 =
    0x38db | X87ContextMemory | (0x38u << X87ContextOriginalModRMShift);
constexpr uint32_t ReadStatus = 0xe0df;
constexpr auto Profile = TranslationRuntimeProfileV1::PortableX87;
const std::vector<uint8_t> ExtendedOne = {1, 0, 0,    0,    0,
                                          0, 0, 0x80, 0xff, 0x3f};

void expectError(llvm::Error Error) {
  EXPECT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
}

std::unique_ptr<GuestMemoryRuntime>
makeMemory(MemoryPermission OutputPermissions = MemoryPermission::Write,
           unsigned OutputSize = 10) {
  auto Logical =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  Logical.Memory = {
      {0x1000, MemoryPermission::Read, 0, ExtendedOne},
      {0x2000, OutputPermissions, 7, std::vector<uint8_t>(OutputSize, 0xa5)}};
  return llvm::cantFail(GuestMemoryRuntime::create(Logical));
}

RuntimeGuestStateX86_64X87V1 makeState(unsigned Top = 5) {
  RuntimeGuestStateX86_64X87V1 State;
  State.Integer.RIP = 0x4000;
  State.Integer.GPR[0] = 0x1122334455667788;
  State.X87.EnvironmentKnown = 1;
  llvm::support::endian::write16le(State.X87.Image.data(), 0x37f);
  llvm::support::endian::write16le(State.X87.Image.data() + 2, Top << 11);
  return State;
}

RuntimeCallFrameV1 frame(GuestMemoryRuntime &Memory, uint64_t PC) {
  const RuntimeCodeCredentialV1 Credential{1, 2, PC, 3, 4};
  return llvm::cantFail(
      createRuntimeCallFrameV1(Memory, Credential, Credential));
}

uint32_t step(RuntimeGuestStateX86_64X87V1 &State, RuntimeCallFrameV1 &Frame,
              uint32_t Selector, uint64_t Address) {
  State.LastStep = {};
  const RuntimeX87StepContractV1 Contract{Selector, State.Integer.RIP,
                                          State.Integer.RIP + 2};
  const auto Status = nvd_rt_x87_v1_step(&Frame, &State, Selector, Contract.PC,
                                         Contract.NextPC, Address);
  llvm::cantFail(
      validateRuntimeX87AfterInvocationV1(State, Frame, Status, Contract));
  return Status;
}

TEST(RuntimeX87Helpers, TerminalCompletionPreservesUpperRAXAndAdvancesOnce) {
  for (unsigned Top = 0; Top != 8; ++Top) {
    auto Memory = makeMemory();
    auto State = makeState(Top);
    auto Frame = frame(*Memory, State.Integer.RIP);
    EXPECT_EQ(step(State, Frame, Load80, 0x1000),
              static_cast<uint32_t>(BlockExitKindV1::Continue));
    EXPECT_EQ(State.Integer.RIP, 0x4002u);
    EXPECT_EQ(State.LastStep.InstructionCompleted, 1);
    EXPECT_EQ(State.LastStep.HasScalarResult, 0);
    Frame = frame(*Memory, State.Integer.RIP);
    EXPECT_EQ(step(State, Frame, Store80, 0x2000),
              static_cast<uint32_t>(BlockExitKindV1::Continue));
    EXPECT_EQ(Memory->snapshotMemoryRegions()[1].Bytes, ExtendedOne);
    Frame = frame(*Memory, State.Integer.RIP);
    EXPECT_EQ(step(State, Frame, ReadStatus, 0),
              static_cast<uint32_t>(BlockExitKindV1::Continue));
    EXPECT_EQ(State.Integer.RIP, 0x4006u);
    EXPECT_EQ(State.Integer.GPR[0],
              0x1122334455660000u | (uint64_t{Top} << 11));
    EXPECT_EQ(State.LastStep.ScalarResult, Top << 11);
    EXPECT_EQ(State.LastStep.HasScalarResult, 1);
    auto Bad = State;
    Bad.Integer.GPR[0] ^= 1;
    expectError(validateRuntimeX87AfterInvocationV1(
        Bad, Frame, static_cast<uint32_t>(BlockExitKindV1::Continue),
        RuntimeX87StepContractV1{ReadStatus, 0x4004, 0x4006}));
  }
}

TEST(RuntimeX87Helpers, RefusalAndTenByteFaultPreserveCurrentStateAndPC) {
  auto Memory = makeMemory(MemoryPermission::Write, 9);
  auto State = makeState();
  State.X87.EnvironmentKnown = 0;
  auto Frame = frame(*Memory, State.Integer.RIP);
  const auto Before = State.X87;
  EXPECT_EQ(step(State, Frame, Load80, UINT64_MAX),
            static_cast<uint32_t>(BlockExitKindV1::Unsupported));
  EXPECT_EQ(State.Integer.RIP, 0x4000u);
  EXPECT_EQ(State.LastStep.Status, PortableX87StepStatusV1::UnknownEnvironment);
  EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::None);
  EXPECT_EQ(std::memcmp(&Before, &State.X87, sizeof(Before)), 0);
  State.X87.EnvironmentKnown = 1;
  EXPECT_EQ(step(State, Frame, Load80, 0x1000),
            static_cast<uint32_t>(BlockExitKindV1::Continue));
  Frame = frame(*Memory, State.Integer.RIP);
  const auto Loaded = State.X87;
  EXPECT_EQ(step(State, Frame, Store80, 0x2000),
            static_cast<uint32_t>(RuntimeABIExitKindV1::MemoryFault));
  EXPECT_EQ(State.Integer.RIP, 0x4002u);
  EXPECT_EQ(State.LastStep.InstructionCompleted, 0);
  EXPECT_EQ(Frame.Control.Exit.Size, 10u);
  EXPECT_EQ(std::memcmp(&Loaded, &State.X87, sizeof(Loaded)), 0);
  expectError(validateRuntimeControlBlockV1(Frame.Control));
  llvm::cantFail(validateRuntimeControlBlockExactSpanV1(Frame.Control));
  Memory->requestCancellation();
  Frame.Control = Memory->snapshotControlBlock();
  llvm::cantFail(validateRuntimeX87AfterInvocationV1(
      State, Frame, static_cast<uint32_t>(RuntimeABIExitKindV1::Cancelled),
      RuntimeX87StepContractV1{Store80, 0x4002, 0x4004}));
  EXPECT_EQ(State.LastStep.Status, PortableX87StepStatusV1::MemoryFault);
}

TEST(RuntimeX87Helpers, CompletedExecutableWriteSurvivesCancellationOverride) {
  auto Memory = makeMemory(MemoryPermission::Write | MemoryPermission::Execute);
  auto State = makeState();
  auto Frame = frame(*Memory, State.Integer.RIP);
  ASSERT_EQ(step(State, Frame, Load80, 0x1000),
            static_cast<uint32_t>(BlockExitKindV1::Continue));
  Frame = frame(*Memory, State.Integer.RIP);
  EXPECT_EQ(step(State, Frame, Store80, 0x2000),
            static_cast<uint32_t>(RuntimeABIExitKindV1::SelfModification));
  EXPECT_EQ(State.LastStep.InstructionCompleted, 1);
  EXPECT_EQ(State.Integer.RIP, 0x4004u);
  EXPECT_EQ(State.X87.Image[4], 0);
  EXPECT_EQ(Frame.Validated, RuntimeCodeCredentialV1{});
  EXPECT_EQ(Memory->snapshotMemoryRegions()[1].Generation, 8u);
  EXPECT_EQ(Memory->snapshotMemoryRegions()[1].Bytes, ExtendedOne);
  Memory->requestCancellation();
  Frame.Control = Memory->snapshotControlBlock();
  const RuntimeX87StepContractV1 Contract{Store80, 0x4002, 0x4004};
  llvm::cantFail(validateRuntimeX87AfterInvocationV1(
      State, Frame, static_cast<uint32_t>(RuntimeABIExitKindV1::Cancelled),
      Contract));
  EXPECT_EQ(State.LastStep.Status, PortableX87StepStatusV1::SelfModification);
  auto Bad = State;
  Bad.LastStep.InstructionCompleted = 0;
  expectError(validateRuntimeX87AfterInvocationV1(
      Bad, Frame, static_cast<uint32_t>(RuntimeABIExitKindV1::Cancelled),
      Contract));
  Frame.Validated = Frame.Published;
  expectError(validateRuntimeX87AfterInvocationV1(
      State, Frame, static_cast<uint32_t>(RuntimeABIExitKindV1::Cancelled),
      Contract));
}

TEST(RuntimeX87Helpers, RejectsUnboundCompletionAndStaleInvocation) {
  auto Memory = makeMemory();
  auto State = makeState();
  auto Frame = frame(*Memory, State.Integer.RIP);
  Frame.Validated.CacheGeneration++;
  EXPECT_EQ(nvd_rt_x87_v1_step(&Frame, &State, Load80, 0x4000, 0x4002, 0x1000),
            static_cast<uint32_t>(BlockExitKindV1::Unsupported));
  EXPECT_FALSE(State.LastStep.Attempted);
  EXPECT_EQ(State.Integer.RIP, 0x4000u);
  Frame.Validated = Frame.Published;
  ASSERT_EQ(step(State, Frame, Load80, 0x1000),
            static_cast<uint32_t>(BlockExitKindV1::Continue));
  const auto Status = static_cast<uint32_t>(BlockExitKindV1::Continue);
  expectError(
      validateRuntimeX87AfterInvocationV1(State, Frame, Status, std::nullopt));
  for (const auto Contract : {RuntimeX87StepContractV1{Store80, 0x4000, 0x4002},
                              RuntimeX87StepContractV1{Load80, 0x4001, 0x4002},
                              RuntimeX87StepContractV1{Load80, 0x4000, 0x4003}})
    expectError(
        validateRuntimeX87AfterInvocationV1(State, Frame, Status, Contract));
  auto Bad = State;
  Bad.Integer.RIP = 0x4000;
  expectError(validateRuntimeX87AfterInvocationV1(
      Bad, Frame, Status, RuntimeX87StepContractV1{Load80, 0x4000, 0x4002}));
}

TEST(RuntimeX87Helpers, ClosedRegistryKeepsLegacyFactoryAndExactNativeBinding) {
  auto Scalar = llvm::cantFail(RuntimeSymbolRegistryV1::create());
  auto Portable = llvm::cantFail(RuntimeSymbolRegistryV1::create(Profile));
  EXPECT_NE(Scalar.identity(), Portable.identity());
  EXPECT_EQ(Portable.entries().size(), Scalar.entries().size() + 1);
  auto Missing = Scalar.lookup(kRuntimeX87StepNameV1);
  ASSERT_FALSE(static_cast<bool>(Missing));
  llvm::consumeError(Missing.takeError());
  EXPECT_EQ(llvm::cantFail(Portable.lookup(kRuntimeX87StepNameV1)),
            llvm::orc::ExecutorAddr::fromPtr(&nvd_rt_x87_v1_step));
  auto BadBindings =
      RuntimeSymbolRegistryV1::create(runtimeABIHelperBindingsV1(Profile));
  ASSERT_FALSE(static_cast<bool>(BadBindings));
  llvm::consumeError(BadBindings.takeError());
  auto BadProfile = RuntimeSymbolRegistryV1::create(
      static_cast<TranslationRuntimeProfileV1>(99));
  ASSERT_FALSE(static_cast<bool>(BadProfile));
  llvm::consumeError(BadProfile.takeError());
}

TEST(RuntimeX87Helpers, IRRequiresCanonicalPointersAndExactTerminalStatus) {
  const std::string Prefix = R"(
target triple = "aarch64-unknown-linux-gnu"
target datalayout = "e-p:64:64-i64:64-n32:64-S128"
declare i32 @nvd_rt_x87_v1_step(ptr, ptr, i32, i64, i64, i64) nounwind
define hidden i32 @block(ptr %state, ptr %runtime) nounwind {
)";
  const std::string Call =
      "  %status = call i32 @nvd_rt_x87_v1_step(ptr %runtime, ptr %state, i32 "
      "57567, i64 4096, i64 4098, i64 0) nounwind\n";
  const auto Verify = [&](const std::string &Body,
                          bool Legacy = false) -> llvm::Error {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module =
        llvm::parseAssemblyString(Prefix + Body + "}\n", Diagnostic, Context);
    EXPECT_NE(Module, nullptr);
    if (!Module)
      return llvm::Error::success();
    return verifyRuntimeTranslationIRV1(
        *Module, llvm::Triple(Module->getTargetTriple()),
        Module->getDataLayout(), kRuntimeGuestStateX86_64X87SizeV1,
        runtimeGuestStateX86_64MemorySlotsV1(),
        Legacy ? TranslationRuntimeProfileV1::ScalarInteger : Profile);
  };
  llvm::LLVMContext Context;
  llvm::Module Empty("unknown-profile", Context);
  expectError(verifyRuntimeABIHelperProtocolV1(
      Empty, static_cast<TranslationRuntimeProfileV1>(99)));
  llvm::cantFail(Verify(Call + "  ret i32 %status\n"));
  expectError(Verify(Call + "  ret i32 %status\n", true));
  expectError(Verify(Call + "  ret i32 256\n"));
  expectError(
      Verify(Call + "  %p = getelementptr i8, ptr %state, i64 136\n  store i64 "
                    "9, ptr %p, align 8\n  ret i32 %status\n"));
  std::string Forged = Call;
  const auto Position = Forged.find("ptr %state");
  Forged.replace(Position, std::string("ptr %state").size(), "ptr %runtime");
  expectError(Verify(Forged + "  ret i32 %status\n"));
  std::string BadDP = Call;
  BadDP.replace(BadDP.find("i64 0)"), 6, "i64 1)");
  expectError(Verify(BadDP + "  ret i32 %status\n"));
  expectError(
      Verify(Call + "  %p = getelementptr i8, ptr %state, i64 688\n  %v = load "
                    "i32, ptr %p, align 4\n  ret i32 %status\n"));
}

} // namespace
