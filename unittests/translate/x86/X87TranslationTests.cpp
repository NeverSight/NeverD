//===- X87TranslationTests.cpp - Qualified x87 object and session path ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X87TranslationTestSupport.h"
#include "gtest/gtest.h"

#include "neverd/translate/NativeTranslationSession.h"
#include "neverd/translate/TranslationJITLinker.h"
#include "neverd/translate/TranslationLinkGraphVerifier.h"
#include "neverd/translate/TranslationObjectRequest.h"
#include "neverd/translate/X86/RuntimeX87Helpers.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::translate;
using namespace neverd::translate::test;

namespace {

template <typename T> void expectFailure(llvm::Expected<T> Result) {
  EXPECT_FALSE(static_cast<bool>(Result));
  if (!Result)
    llvm::consumeError(Result.takeError());
}

TEST(X87Translation,
     ObjectAndGraphBindExplicitProfileAndFirstInstructionBoundary) {
  auto State = fixture();
  auto Options = options();
  auto Object = compileTranslationObjectRequestV1({State, Entry, Options});
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  EXPECT_EQ(Object->descriptor().Header.Terminator,
            TranslationBlockTerminatorKindV1::X87Runtime);
  EXPECT_EQ(Object->descriptor().Header.GuestInstructionCount, 2u);
  EXPECT_EQ(Object->descriptor().Header.FallthroughPC, Entry + 9);
  EXPECT_EQ(Object->descriptor().RuntimeProfile, Profile);
  EXPECT_EQ(Object->artifact().runtimeProfile(), Profile);
  auto Registry = llvm::cantFail(RuntimeSymbolRegistryV1::create(Profile));
  EXPECT_EQ(Object->artifact().runtimeRegistryIdentity(), Registry.identity());
  auto Audit = verifyTranslationLinkGraphV1(Object->artifact());
  ASSERT_TRUE(static_cast<bool>(Audit)) << llvm::toString(Audit.takeError());
  Options.RuntimeProfile = TranslationRuntimeProfileV1::ScalarInteger;
  Options.RequiredCapabilities = TranslationCapability::ScalarInteger;
  expectFailure(compileTranslationObjectRequestV1({State, Entry, Options}));
}

TEST(X87Translation, RefusesMissingStateUnsupportedFormAndForgedAddressSpace) {
  auto State =
      fixture({0xd9, 0x37, 0xc3}); // FNSTENV [RDI] is outside this profile.
  auto Options = options();
  expectFailure(compileTranslationObjectRequestV1({State, Entry, Options}));
  State = fixture();
  auto Missing = State;
  std::erase_if(Missing.Registers,
                [](const auto &Register) { return Register.ID == X87ID; });
  expectFailure(compileTranslationObjectRequestV1({Missing, Entry, Options}));
  Missing = State;
  findRegisterValue(Missing, X87ID)->Value.flipBit(32);
  expectFailure(compileTranslationObjectRequestV1({Missing, Entry, Options}));

  auto Memory = llvm::cantFail(GuestMemoryRuntime::create(State));
  auto Builder = llvm::cantFail(X86TranslationBlockBuilder::create(Profile));
  auto Descriptor = llvm::cantFail(Builder->build(*Memory, Entry));
  auto Target = llvm::cantFail(createTranslationTargetMachineV1(Options));
  llvm::LLVMContext Context;
  auto Valid = lowerX86TranslationBlockV1(Descriptor, Target.hostTarget(),
                                          Target.dataLayout(), Context);
  ASSERT_TRUE(static_cast<bool>(Valid)) << llvm::toString(Valid.takeError());
  auto Step =
      std::find_if(Descriptor.Ops.begin(), Descriptor.Ops.end(),
                   [](const auto &Op) { return Op.Opcode == NdOp::INTRINSIC; });
  ASSERT_NE(Step, Descriptor.Ops.end());
  Step->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  expectFailure(lowerX86TranslationBlockV1(Descriptor, Target.hostTarget(),
                                           Target.dataLayout(), Context));
}

TEST(X87Translation, RefusesUnqualifiedHostPolicyAndProfile) {
  const auto State = fixture();
  for (const char *Triple : {"arm64-apple-darwin", "aarch64-pc-windows-msvc"}) {
    auto Options = options();
    Options.Target.Triple = Triple;
    expectFailure(compileTranslationObjectRequestV1({State, Entry, Options}));
  }
  auto Options = options();
  Options.CodeInvalidation = CodeInvalidationPolicy::ValidateBeforeDispatch;
  expectFailure(compileTranslationObjectRequestV1({State, Entry, Options}));
  Options = options();
  Options.RuntimeProfile = static_cast<TranslationRuntimeProfileV1>(99);
  expectFailure(compileTranslationObjectRequestV1({State, Entry, Options}));

  auto WritableCode = fixture();
  WritableCode.Memory[2].Permissions =
      MemoryPermission::Write | MemoryPermission::Execute;
  Options = options(true);
  // The session contract rejects this mapping before host-target resolution,
  // so the negative boundary must execute even on non-AArch64 hosts.
  auto Rejected = NativeTranslationSessionV1::create(Options, WritableCode);
  ASSERT_FALSE(static_cast<bool>(Rejected));
  auto Unhandled = llvm::handleErrors(
      Rejected.takeError(), [](const NativeTranslationSessionError &Error) {
        EXPECT_EQ(Error.code(),
                  NativeTranslationSessionErrorCode::InvalidRequest);
        EXPECT_NE(Error.detail().find("writable executable guest memory"),
                  llvm::StringRef::npos);
      });
  EXPECT_FALSE(static_cast<bool>(Unhandled))
      << llvm::toString(std::move(Unhandled));
  Options.CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  auto Admitted = validateTranslationRequest(Options, WritableCode);
  EXPECT_FALSE(static_cast<bool>(Admitted))
      << llvm::toString(std::move(Admitted));
}

TEST(X87Translation,
     PureIntegerArtifactsStillHaveDifferentRuntimeAndCacheIdentity) {
  auto State = fixture({0x48, 0xc7, 0xc0, 42, 0, 0, 0, 0xc3});
  auto PortableOptions = options();
  auto Portable = llvm::cantFail(
      compileTranslationObjectRequestV1({State, Entry, PortableOptions}));
  auto ScalarOptions = PortableOptions;
  ScalarOptions.RuntimeProfile = TranslationRuntimeProfileV1::ScalarInteger;
  ScalarOptions.RequiredCapabilities = TranslationCapability::ScalarInteger;
  auto Scalar = llvm::cantFail(
      compileTranslationObjectRequestV1({State, Entry, ScalarOptions}));
  EXPECT_NE(Portable.cacheIdentity(), Scalar.cacheIdentity());
  EXPECT_NE(Portable.artifact().requestCacheKey(),
            Scalar.artifact().requestCacheKey());
  EXPECT_NE(Portable.artifact().runtimeRegistryIdentity(),
            Scalar.artifact().runtimeRegistryIdentity());
}

TEST(X87Translation, EveryControlFlowBlockCanBePublishedWithoutExecution) {
  for (bool Loop : {false, true}) {
    auto State = fixture(controlProgram(Loop));
    auto Options = options();
    std::vector<uint64_t> Pending{Entry};
    std::set<uint64_t> Published;
    while (!Pending.empty()) {
      const uint64_t PC = Pending.back();
      Pending.pop_back();
      if (!Published.insert(PC).second)
        continue;
      llvm::cantFail(setRegisterValue(State, 16, llvm::APInt(64, PC)));
      auto Object = compileTranslationObjectRequestV1({State, PC, Options});
      ASSERT_TRUE(static_cast<bool>(Object))
          << llvm::toString(Object.takeError());
      const auto &Header = Object->descriptor().Header;
      switch (Header.Terminator) {
      case TranslationBlockTerminatorKindV1::X87Runtime:
        Pending.push_back(Header.FallthroughPC);
        break;
      case TranslationBlockTerminatorKindV1::ConditionalBranch:
        Pending.push_back(Header.FallthroughPC);
        Pending.push_back(Header.StaticTargetPC);
        break;
      case TranslationBlockTerminatorKindV1::Return:
        break;
      default:
        FAIL() << "unexpected fixture boundary";
      }
    }
    EXPECT_EQ(Published.size(), 8u);
  }
}

TEST(X87TranslationNative,
     IntegerInvocationCannotEnterLargerProfileEvenWithoutX87Calls) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 invocation required";
  auto State = fixture({0x48, 0xc7, 0xc0, 42, 0, 0, 0, 0xc3});
  auto Object = llvm::cantFail(
      compileTranslationObjectRequestV1({State, Entry, options(true)}));
  auto Registry = llvm::cantFail(RuntimeSymbolRegistryV1::create(Profile));
  const RuntimeCodeCredentialV1 Credential{1, 2, Entry, 3, 4};
  const auto Bindings = runtimeABIHelperBindingsV1(Profile);
  std::vector<RuntimeABIHelperBindingV1> ForgedBindings(Bindings.begin(),
                                                        Bindings.end());
  auto ForgedStep = std::find_if(ForgedBindings.begin(), ForgedBindings.end(),
                                 [](const auto &Binding) {
                                   return Binding.Name == kRuntimeX87StepNameV1;
                                 });
  ASSERT_NE(ForgedStep, ForgedBindings.end());
  ForgedStep->X87Step = +[](void *, void *, uint32_t, uint64_t, uint64_t,
                            uint64_t) noexcept -> uint32_t {
    return static_cast<uint32_t>(BlockExitKindV1::Continue);
  };
  auto ForgedRegistry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create(Profile, ForgedBindings));
  EXPECT_EQ(ForgedRegistry.identity(), Registry.identity());
  expectFailure(linkTranslationObjectV1(Object, ForgedRegistry, Credential));
  auto Linked =
      llvm::cantFail(linkTranslationObjectV1(Object, Registry, Credential));
  GuestMemoryRuntimeConfig Config;
  Config.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  auto Memory = llvm::cantFail(GuestMemoryRuntime::create(State, Config));
  auto Frame =
      llvm::cantFail(createRuntimeCallFrameV1(*Memory, Credential, Credential));
  auto Runtime = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(State));
  expectFailure(Linked.invoke(Runtime.Integer, Frame));
  auto Unbound =
      llvm::cantFail(linkTranslationObjectV1(Object.artifact(), Registry));
  expectFailure(Unbound.invokePortableX87(Runtime, Frame));
  llvm::cantFail(Unbound.unload());
  Frame.Validated.CodeEpoch++;
  expectFailure(Linked.invokePortableX87(Runtime, Frame));
  Frame.Validated = Credential;
  auto WrongMemory = llvm::cantFail(GuestMemoryRuntime::create(State));
  auto WrongFrame = llvm::cantFail(
      createRuntimeCallFrameV1(*WrongMemory, Credential, Credential));
  expectFailure(Linked.invokePortableX87(Runtime, WrongFrame));
  EXPECT_EQ(Runtime.Integer.RIP, Entry);
  EXPECT_EQ(llvm::cantFail(Linked.invokePortableX87(Runtime, Frame)),
            static_cast<uint32_t>(BlockExitKindV1::Return));
  EXPECT_EQ(Runtime.Integer.GPR[0], 42u);
  EXPECT_EQ(Runtime.Integer.RIP, 0x1234u);
}

TEST(X87TranslationNative,
     EveryTopAndPrecisionRoundingRetainsExactMemoryAndState) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 session required";
  for (unsigned Top = 0; Top != 8; ++Top)
    for (unsigned PC : {0u, 2u, 3u})
      for (unsigned RC = 0; RC != 4; ++RC) {
        const uint16_t CW = 0x7f | PC << 8 | RC << 10;
        auto Session = llvm::cantFail(NativeTranslationSessionV1::create(
            options(true), fixture(program(), Top, CW)));
        auto Result = Session->run();
        ASSERT_TRUE(static_cast<bool>(Result))
            << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::Returned);
        const auto &Logical = Session->state();
        auto Runtime =
            llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
        uint64_t Mantissa = 0xc000000000000000;
        if (PC == 3)
          Mantissa |= 1;
        else if (RC == 2)
          Mantissa |= uint64_t{1} << (PC == 2 ? 11 : 40);
        const auto &Output = Logical.Memory[2].Bytes;
        ASSERT_EQ(Output.size(), 12u);
        EXPECT_EQ(Output.front(), 0xa5);
        EXPECT_EQ(Output.back(), 0xa5);
        EXPECT_EQ(llvm::support::endian::read64le(Output.data() + 1), Mantissa);
        EXPECT_EQ(llvm::support::endian::read16le(Output.data() + 9), 0x3fff);
        EXPECT_EQ(Runtime.Integer.RIP, 0x1234u);
        EXPECT_EQ(Runtime.Integer.GPR[0], (Top << 11) | (PC != 3 ? 0x20u : 0));
        EXPECT_EQ(Runtime.X87.Image[4], 0);
        EXPECT_EQ(Runtime.X87.KnownPayload,
                  (1u << ((Top + 7) & 7)) | (1u << ((Top + 6) & 7)));
        EXPECT_EQ(llvm::support::endian::read16le(Runtime.X87.Image.data()),
                  CW);
        EXPECT_EQ(llvm::support::endian::read64le(Runtime.X87.Image.data() + 8),
                  Entry + 13);
        EXPECT_EQ(
            llvm::support::endian::read64le(Runtime.X87.Image.data() + 16),
            0x3001u);
      }
}

TEST(X87TranslationNative, RefusalsAndMemoryFaultRetainCommittedPrefix) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 session required";
  constexpr PortableX87StepStatusV1 Expected[] = {
      PortableX87StepStatusV1::UnknownEnvironment,
      PortableX87StepStatusV1::PendingException,
      PortableX87StepStatusV1::InvalidControl,
      PortableX87StepStatusV1::StackFault,
      PortableX87StepStatusV1::UnsupportedValue,
      PortableX87StepStatusV1::MemoryFault};
  for (unsigned Kind = 0; Kind != std::size(Expected); ++Kind) {
    SCOPED_TRACE(Kind);
    auto Before = fixture();
    auto Initial = llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Before));
    if (Kind == 0)
      Initial.X87.EnvironmentKnown = 0;
    if (Kind == 1) {
      Initial.X87.Image[0] &= ~1u;
      Initial.X87.Image[2] = 1;
    }
    if (Kind == 2)
      Initial.X87.Image[1] = 1;
    if (Kind == 3) {
      Initial.X87.Image[4] = 0xff;
      Initial.X87.KnownPayload = 0xff;
    }
    llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(Initial, Before));
    if (Kind == 4)
      Before.Memory[0].Bytes = {0x23, 1, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f};
    if (Kind == 5)
      Before.Memory[2].Bytes.resize(10); // Last operand byte is unmapped.
    auto Session = llvm::cantFail(
        NativeTranslationSessionV1::create(options(true), Before));
    auto Result = Session->run();
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    const auto &Logical = Session->state();
    const auto After =
        llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
    EXPECT_EQ(Result->Exit.Reason,
              Kind == 5 ? TranslationStopReason::MemoryFault
                        : TranslationStopReason::UnsupportedInstruction);
    const uint64_t Stop = Entry + (Kind < 4 ? 7 : Kind == 4 ? 11 : 13);
    EXPECT_EQ(Result->Exit.PC, Stop);
    EXPECT_EQ(Result->Exit.NextPC, Stop);
    EXPECT_EQ(After.Integer.RIP, Stop);
    EXPECT_EQ(After.Integer.GPR[0], 0u); // MOV committed, FNSTSW not reached.
    EXPECT_EQ(After.Integer.GPR[4], 0x8000u); // RET not reached.
    EXPECT_EQ(Logical.Memory[2].Bytes, Before.Memory[2].Bytes);
    EXPECT_EQ(Result->GuestInstructions, Kind < 4 ? 2u : Kind == 4 ? 4u : 5u);
    EXPECT_EQ(Result->BlocksTranslated, Kind < 4 ? 1u : Kind == 4 ? 3u : 4u);
    if (Kind < 4)
      EXPECT_EQ(std::memcmp(&Initial.X87, &After.X87, sizeof(Initial.X87)), 0);
    else {
      EXPECT_EQ(llvm::support::endian::read16le(After.X87.Image.data() + 2),
                (Kind == 4 ? 3u : 4u) << 11);
      EXPECT_EQ(After.X87.Image[4], Kind == 4 ? 0x18 : 0x10);
    }
    if (Kind == 5) {
      ASSERT_TRUE(Result->Exit.MemoryFault);
      EXPECT_EQ(Result->Exit.MemoryFault->Address, 0x3001u);
      EXPECT_EQ(Result->Exit.MemoryFault->AccessWidthBits, 80u);
      EXPECT_EQ(Result->Exit.MemoryFault->Access, MemoryAccessKind::Write);
    } else {
      ASSERT_TRUE(Result->Exit.Trap);
      EXPECT_EQ(Result->Exit.Trap->Code, kRuntimeX87RefusalTrapCodeV1);
      EXPECT_EQ(Result->Exit.Trap->Subcode,
                static_cast<uint32_t>(Expected[Kind]));
      EXPECT_TRUE(Result->Exit.Trap->Restartable);
    }
  }
}

TEST(X87TranslationNative, ExecutableStoreCommitsOnlyUnderInvalidatePolicy) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 session required";
  auto Before = fixture();
  Before.Memory[2].Permissions =
      MemoryPermission::Write | MemoryPermission::Execute;
  Before.Memory[2].Generation = 7;
  auto Options = options(true);
  Options.CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  auto SessionOrErr = NativeTranslationSessionV1::create(Options, Before);
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  auto Session = std::move(*SessionOrErr);
  auto Result = Session->run();
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::SelfModification);
  EXPECT_EQ(Result->GuestInstructions, 5u);
  EXPECT_EQ(Result->BlocksTranslated, 4u);
  auto After = createRuntimeGuestStateX86_64X87V1(Session->state());
  ASSERT_TRUE(static_cast<bool>(After)) << llvm::toString(After.takeError());
  EXPECT_EQ(After->Integer.RIP, Entry + 15);
  EXPECT_EQ(After->Integer.GPR[0], 0u);
  EXPECT_EQ(After->X87.Image[4], 0);
  EXPECT_EQ(Session->state().Memory[2].Generation, 8u);
  ASSERT_TRUE(Result->Exit.SelfModification);
  EXPECT_EQ(Result->Exit.SelfModification->Size, 10u);
  EXPECT_EQ(Result->Exit.SelfModification->Address, 0x3001u);
  EXPECT_EQ(Result->Exit.SelfModification->OldGeneration, 7u);
  EXPECT_EQ(Result->Exit.SelfModification->NewGeneration, 8u);
  EXPECT_EQ(llvm::support::endian::read64le(
                Session->state().Memory[2].Bytes.data() + 1),
            0xc000000000000001u);
}

TEST(X87TranslationNative,
     MixedUnsupportedFormStopsAfterTheLastCommittedBlock) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 session required";
  auto State = fixture({0xdb, 0x2f, 0xd9, 0x37, 0xc3}); // FLD; FNSTENV; RET.
  auto Session =
      llvm::cantFail(NativeTranslationSessionV1::create(options(true), State));
  auto Result = Session->run();
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::UnsupportedInstruction);
  EXPECT_EQ(Result->Exit.PC, Entry + 2);
  const auto After =
      llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Session->state()));
  EXPECT_EQ(After.Integer.RIP, Entry + 2);
  EXPECT_EQ(After.X87.Image[4], 0x10);
  EXPECT_EQ(llvm::support::endian::read16le(After.X87.Image.data() + 2),
            4u << 11);
  EXPECT_EQ(Session->state().Memory[0].Bytes, State.Memory[0].Bytes);
}

TEST(X87TranslationNative, BranchesAndBackedgesPreserveTheOwnedStack) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 session required";
  for (const auto Level :
       {LLVMOptimizationLevel::O0, LLVMOptimizationLevel::O2})
    for (bool Loop : {false, true})
      for (unsigned Count : {1u, 2u}) {
        auto State = fixture(controlProgram(Loop));
        llvm::cantFail(setRegisterValue(
            State, 1, llvm::APInt(64, Loop ? Count : Count - 1)));
        auto Options = options(true);
        Options.LLVMLevel = Level;
        auto Session =
            llvm::cantFail(NativeTranslationSessionV1::create(Options, State));
        auto Result = Session->run();
        ASSERT_TRUE(static_cast<bool>(Result))
            << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::Returned);
        const auto After = llvm::cantFail(
            createRuntimeGuestStateX86_64X87V1(Session->state()));
        EXPECT_EQ(After.Integer.RIP, 0x1234u);
        EXPECT_EQ(After.Integer.GPR[0],
                  (5u << 11) + (!Loop && Count == 2 ? 1u : 0u));
        EXPECT_EQ(After.X87.Image[4], 0);
        const auto &Output = Session->state().Memory[2].Bytes;
        EXPECT_EQ(llvm::support::endian::read64le(Output.data() + 1),
                  0xc000000000000001u);
        EXPECT_EQ(llvm::support::endian::read16le(Output.data() + 9),
                  Loop && Count % 2 ? 0xbfff : 0x3fff);
        EXPECT_EQ(Output.front(), 0xa5);
        EXPECT_EQ(Output.back(), 0xa5);
      }
}

TEST(X87TranslationNative, BudgetStopAndRestoreKeepExplicitState) {
  if (!nativeLinuxAArch64())
    GTEST_SKIP() << "native Linux AArch64 session required";
  auto Options = options(true);
  Options.BlockBudget = 1;
  auto Session =
      llvm::cantFail(NativeTranslationSessionV1::create(Options, fixture()));
  auto Result = Session->run();
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::BudgetExhausted);
  EXPECT_EQ(Result->Exit.PC, Entry + 9);
  EXPECT_EQ(Result->GuestInstructions, 2u);
  const auto Checkpoint = Session->state();
  auto Bad = Checkpoint;
  std::erase_if(Bad.Registers,
                [](const auto &Register) { return Register.ID == X87ID; });
  auto Error = Session->restoreState(std::move(Bad));
  EXPECT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(llvm::cantFail(serializeGuestState(Session->state())),
            llvm::cantFail(serializeGuestState(Checkpoint)));
  auto Resumed = llvm::cantFail(
      NativeTranslationSessionV1::create(options(true), Checkpoint));
  auto Completion = Resumed->run();
  ASSERT_TRUE(static_cast<bool>(Completion))
      << llvm::toString(Completion.takeError());
  EXPECT_EQ(Completion->Exit.Reason, TranslationStopReason::Returned);
  EXPECT_EQ(llvm::support::endian::read64le(
                Resumed->state().Memory[2].Bytes.data() + 1),
            0xc000000000000001u);
}

} // namespace
