//===- TranslationJITLinkerTests.cpp - Sealed linker tests ---------------===//

#include "TranslationJITLinkerInternal.h"
#include "gtest/gtest.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/GuestState.h"
#include "neverd/translate/ResolvedHostTarget.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/TranslationJITLinker.h"
#include "neverd/translate/TranslationLinkGraphVerifier.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

constexpr uint64_t EntryPC = 0x401000;
constexpr uint64_t StackAddress = 0x700000;
constexpr uint64_t ReturnTarget = 0x500123;

template <typename T>
std::array<uint8_t, sizeof(T)> objectRepresentation(const T &Value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::array<uint8_t, sizeof(T)> Bytes;
  std::memcpy(Bytes.data(), &Value, sizeof(Value));
  return Bytes;
}

template <typename Input>
concept CredentialedLinkInputV1 =
    requires(const Input &Object, const RuntimeSymbolRegistryV1 &Registry,
             const RuntimeCodeCredentialV1 &Credential) {
      linkTranslationObjectV1(Object, Registry, Credential);
    };

static_assert(CredentialedLinkInputV1<TranslationObjectResultV1>);
static_assert(!CredentialedLinkInputV1<TranslationObjectArtifactV1>);

TranslationOptions aarch64AOTOptions(llvm::StringRef Triple) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = GuestArchitecture::AArch64;
  Options.Target.Triple = Triple.str();
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  return Options;
}

TranslationOptions nativeJITOptions() {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::JIT;
  Options.Target = HostTarget();
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  Options.LLVMLevel = LLVMOptimizationLevel::O2;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  return Options;
}

bool hasNativeAArch64Target() {
  llvm::Expected<ResolvedHostTarget> Target =
      resolveHostTarget(nativeJITOptions());
  if (!Target) {
    ADD_FAILURE() << llvm::toString(Target.takeError());
    return false;
  }
  return Target->architecture() == GuestArchitecture::AArch64;
}

GuestState fixtureState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          19,
                          {0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3}});
  return State;
}

GuestState executableFixtureState() {
  GuestState State = fixtureState();
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != sizeof(uint64_t); ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write, 0,
                          std::move(Stack)});
  llvm::cantFail(setRegisterValue(State, 4, llvm::APInt(64, StackAddress)));
  llvm::cantFail(setRegisterValue(State, 7, llvm::APInt(64, 41)));
  llvm::cantFail(setRegisterValue(State, 16, llvm::APInt(64, EntryPC)));
  return State;
}

uint32_t alternateLoad(void *, uint64_t, uint32_t) noexcept { return 0; }

void expectLinkerError(llvm::Expected<LinkedTranslationBlockV1> Result,
                       TranslationJITLinkerErrorCode ExpectedCode) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationJITLinkerError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(), ExpectedCode) << Error.detail().str();
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

void expectInvocationError(llvm::Expected<uint32_t> Result,
                           TranslationJITLinkerErrorCode ExpectedCode) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationJITLinkerError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(), ExpectedCode) << Error.detail().str();
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

TEST(TranslationJITLinker, RejectsAOTArtifactBeforeGraphCreation) {
  GuestState State = fixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64AOTOptions("aarch64-unknown-linux-gnu")});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  expectLinkerError(
      linkTranslationObjectV1(ResultOrErr->artifact(), *RegistryOrErr),
      TranslationJITLinkerErrorCode::ArtifactTargetNotNative);
}

TEST(TranslationJITLinker, PostFixupRejectsBranchOpcodeSubstitution) {
  constexpr uint64_t FixupAddress = 0x1000;
  constexpr uint64_t TargetAddress = 0x1004;
  constexpr uint32_t B = 0x14000000U;
  constexpr uint32_t BL = 0x94000000U;
  constexpr uint32_t FixedB = B | 1U;
  constexpr uint32_t FixedBL = BL | 1U;

  EXPECT_TRUE(detail::isSealedAArch64Branch26FixupV1(B, FixedB, FixupAddress,
                                                     TargetAddress));
  EXPECT_TRUE(detail::isSealedAArch64Branch26FixupV1(BL, FixedBL, FixupAddress,
                                                     TargetAddress));
  EXPECT_FALSE(detail::isSealedAArch64Branch26FixupV1(B, FixedBL, FixupAddress,
                                                      TargetAddress));
  EXPECT_FALSE(detail::isSealedAArch64Branch26FixupV1(BL, FixedB, FixupAddress,
                                                      TargetAddress));
  EXPECT_FALSE(detail::isSealedAArch64Branch26FixupV1(
      B | 1U, FixedB, FixupAddress, TargetAddress));
  EXPECT_FALSE(detail::isSealedAArch64Branch26FixupV1(B, FixedB, FixupAddress,
                                                      TargetAddress + 4));
}

TEST(TranslationJITLinker, RepeatedLinkAndUnloadOwnsEveryAllocation) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 linking is unavailable";

  GuestState State = fixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  for (unsigned Iteration = 0; Iteration != 64; ++Iteration) {
    llvm::Expected<LinkedTranslationBlockV1> Linked =
        linkTranslationObjectV1(ResultOrErr->artifact(), *RegistryOrErr);
    ASSERT_TRUE(static_cast<bool>(Linked))
        << "iteration " << Iteration << ": "
        << llvm::toString(Linked.takeError());
    EXPECT_TRUE(Linked->isLoaded());
    EXPECT_FALSE(static_cast<bool>(Linked->unload()));
    EXPECT_FALSE(Linked->isLoaded());
  }
}

TEST(TranslationJITLinker,
     UncredentialedLinkCannotInvokeAndUnloadIsIdempotent) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 linking is unavailable";

  GuestState State = fixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());
  llvm::Expected<LinkedTranslationBlockV1> Linked =
      linkTranslationObjectV1(ResultOrErr->artifact(), *RegistryOrErr);
  ASSERT_TRUE(static_cast<bool>(Linked)) << llvm::toString(Linked.takeError());

  RuntimeGuestStateX86_64V1 RuntimeState =
      llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
  std::unique_ptr<GuestMemoryRuntime> Memory =
      llvm::cantFail(GuestMemoryRuntime::create(State));
  const RuntimeCodeCredentialV1 Credential = {
      /*SessionID=*/1, /*BlockID=*/2, /*EntryPC=*/EntryPC,
      /*CacheGeneration=*/3, /*CodeEpoch=*/4};
  RuntimeCallFrameV1 Frame =
      llvm::cantFail(createRuntimeCallFrameV1(*Memory, Credential, Credential));
  expectInvocationError(Linked->invoke(RuntimeState, Frame),
                        TranslationJITLinkerErrorCode::InvocationRejected);

  EXPECT_FALSE(static_cast<bool>(Linked->unload()));
  EXPECT_FALSE(static_cast<bool>(Linked->unload()));
  EXPECT_FALSE(Linked->isLoaded());
  expectInvocationError(Linked->invoke(RuntimeState, Frame),
                        TranslationJITLinkerErrorCode::InvocationRejected);
}

TEST(TranslationJITLinker, RejectsAddressSubstitutedRuntimeRegistry) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 linking is unavailable";

  GuestState State = fixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());

  const llvm::ArrayRef<RuntimeABIHelperBindingV1> Production =
      runtimeABIHelperBindingsV1();
  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> Substituted(
      Production.begin(), Production.end());
  ASSERT_EQ(Substituted.front().Class, RuntimeABIHelperClassV1::Load);
  Substituted.front().Load = &alternateLoad;
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create(Substituted);
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  expectLinkerError(
      linkTranslationObjectV1(ResultOrErr->artifact(), *RegistryOrErr),
      TranslationJITLinkerErrorCode::RuntimeRegistryMismatch);
}

TEST(TranslationJITLinker, ReauditsCompilerArtifactBytesBeforeAllocation) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 linking is unavailable";

  GuestState State = fixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  llvm::ArrayRef<uint8_t> Bytes = ResultOrErr->artifact().bytes();
  ASSERT_FALSE(Bytes.empty());
  const_cast<uint8_t *>(Bytes.data())[0] ^= 0xffU;
  expectLinkerError(
      linkTranslationObjectV1(ResultOrErr->artifact(), *RegistryOrErr),
      TranslationJITLinkerErrorCode::ArtifactAuditFailed);
}

TEST(TranslationJITLinker,
     RejectsCredentialEntryPCDifferentFromTrustedDescriptor) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 linking is unavailable";

  GuestState State = executableFixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  const RuntimeCodeCredentialV1 WrongCredential = {
      /*SessionID=*/1, /*BlockID=*/2, /*EntryPC=*/EntryPC + 1,
      /*CacheGeneration=*/3, /*CodeEpoch=*/4};
  expectLinkerError(
      linkTranslationObjectV1(*ResultOrErr, *RegistryOrErr, WrongCredential),
      TranslationJITLinkerErrorCode::InvocationRejected);
}

TEST(TranslationJITLinker, RejectsCorruptedTrustedDescriptorBeforeLinking) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 linking is unavailable";

  GuestState State = executableFixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  TranslationBlockDescriptorV1 &Descriptor =
      const_cast<TranslationBlockDescriptorV1 &>(ResultOrErr->descriptor());
  Descriptor.Header.Magic ^= 1U;
  const RuntimeCodeCredentialV1 Credential = {
      /*SessionID=*/1, /*BlockID=*/2, /*EntryPC=*/EntryPC,
      /*CacheGeneration=*/3, /*CodeEpoch=*/4};
  expectLinkerError(
      linkTranslationObjectV1(*ResultOrErr, *RegistryOrErr, Credential),
      TranslationJITLinkerErrorCode::InvocationRejected);
}

TEST(TranslationJITLinker,
     RejectsRuntimeStateAtWrongEntryPCWithoutMutatingInvocationInputs) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State = executableFixtureState();
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  const RuntimeCodeCredentialV1 Credential = {
      /*SessionID=*/1, /*BlockID=*/2, /*EntryPC=*/EntryPC,
      /*CacheGeneration=*/3, /*CodeEpoch=*/4};
  llvm::Expected<LinkedTranslationBlockV1> Linked =
      linkTranslationObjectV1(*ResultOrErr, *RegistryOrErr, Credential);
  ASSERT_TRUE(static_cast<bool>(Linked)) << llvm::toString(Linked.takeError());

  RuntimeGuestStateX86_64V1 RuntimeState =
      llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
  RuntimeState.RIP = EntryPC + 1;
  std::unique_ptr<GuestMemoryRuntime> Memory =
      llvm::cantFail(GuestMemoryRuntime::create(State));
  RuntimeCallFrameV1 Frame =
      llvm::cantFail(createRuntimeCallFrameV1(*Memory, Credential, Credential));
  const auto StateBefore = objectRepresentation(RuntimeState);
  const auto FrameBefore = objectRepresentation(Frame);

  expectInvocationError(Linked->invoke(RuntimeState, Frame),
                        TranslationJITLinkerErrorCode::InvocationRejected);
  EXPECT_EQ(objectRepresentation(RuntimeState), StateBefore);
  EXPECT_EQ(objectRepresentation(Frame), FrameBefore);

  EXPECT_FALSE(static_cast<bool>(Linked->unload()));
}

TEST(TranslationJITLinker, InvokesAuditedX86ReturnThroughFinalizedAArch64Code) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State = executableFixtureState();
  TranslationOptions Options = nativeJITOptions();
  Options.CodeInvalidation = CodeInvalidationPolicy::ValidateBeforeDispatch;
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());

  const RuntimeCodeCredentialV1 Credential = {
      /*SessionID=*/1, /*BlockID=*/2, /*EntryPC=*/EntryPC,
      /*CacheGeneration=*/3, /*CodeEpoch=*/4};
  llvm::Expected<LinkedTranslationBlockV1> Linked =
      linkTranslationObjectV1(*ResultOrErr, *RegistryOrErr, Credential);
  ASSERT_TRUE(static_cast<bool>(Linked)) << llvm::toString(Linked.takeError());
  EXPECT_TRUE(Linked->isLoaded());
  EXPECT_TRUE(Linked->auditReceipt().InvocationCredentialBound);
  EXPECT_TRUE(Linked->auditReceipt().completed(
      TranslationJITLinkAuditStageV1::Finalized));

  RuntimeGuestStateX86_64V1 RuntimeState =
      llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
  GuestMemoryRuntimeConfig MemoryConfig;
  MemoryConfig.CodeInvalidation =
      CodeInvalidationPolicy::ValidateBeforeDispatch;
  std::unique_ptr<GuestMemoryRuntime> Memory =
      llvm::cantFail(GuestMemoryRuntime::create(State, MemoryConfig));
  ASSERT_EQ(Memory->validateExecutableGeneration(EntryPC, 19).Status,
            GuestMemoryAccessStatus::Completed);
  RuntimeCallFrameV1 Frame = llvm::cantFail(
      createRuntimeCallFrameV1(*Memory, Credential, Credential, EntryPC, 19));

  llvm::Expected<uint32_t> Status = Linked->invoke(RuntimeState, Frame);
  ASSERT_TRUE(static_cast<bool>(Status)) << llvm::toString(Status.takeError());
  EXPECT_EQ(*Status, static_cast<uint32_t>(BlockExitKindV1::Return));
  EXPECT_EQ(RuntimeState.GPR[static_cast<size_t>(RuntimeX86_64GPRV1::RAX)],
            42u);
  EXPECT_EQ(RuntimeState.GPR[static_cast<size_t>(RuntimeX86_64GPRV1::RSP)],
            StackAddress + sizeof(uint64_t));
  EXPECT_EQ(RuntimeState.RIP, ReturnTarget);

  EXPECT_FALSE(static_cast<bool>(Linked->unload()));
  EXPECT_FALSE(Linked->isLoaded());
}

TEST(TranslationJITLinker,
     InvokesAuditedDirectBranchWithoutInventingRuntimeReferences) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/23,
                          {0xeb, 0xfe}});
  llvm::cantFail(setRegisterValue(State, 16, llvm::APInt(64, EntryPC)));

  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, nativeJITOptions()});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  ASSERT_TRUE(static_cast<bool>(RegistryOrErr))
      << llvm::toString(RegistryOrErr.takeError());
  EXPECT_EQ(ResultOrErr->artifact().runtimeSymbols().size(),
            RegistryOrErr->entries().size());
  EXPECT_EQ(ResultOrErr->artifact().runtimeRegistryIdentity(),
            RegistryOrErr->identity());
  llvm::Expected<TranslationLinkGraphAuditV1> GraphAudit =
      verifyTranslationLinkGraphV1(ResultOrErr->artifact());
  ASSERT_TRUE(static_cast<bool>(GraphAudit))
      << llvm::toString(GraphAudit.takeError());
  EXPECT_EQ(GraphAudit->ExternalSymbolCount, 0u);
  EXPECT_EQ(GraphAudit->EdgeCount, 0u);

  const RuntimeCodeCredentialV1 Credential = {
      /*SessionID=*/11, /*BlockID=*/12, /*EntryPC=*/EntryPC,
      /*CacheGeneration=*/13, /*CodeEpoch=*/14};
  llvm::Expected<LinkedTranslationBlockV1> Linked =
      linkTranslationObjectV1(*ResultOrErr, *RegistryOrErr, Credential);
  ASSERT_TRUE(static_cast<bool>(Linked)) << llvm::toString(Linked.takeError());
  EXPECT_EQ(Linked->auditReceipt().RuntimeReferenceCount, 0u);
  EXPECT_EQ(Linked->auditReceipt().StubCount, 0u);
  EXPECT_EQ(Linked->auditReceipt().GOTEntryCount, 0u);
  EXPECT_EQ(Linked->auditReceipt().FinalSectionCount, GraphAudit->SectionCount);
  EXPECT_EQ(Linked->auditReceipt().FinalBlockCount, GraphAudit->BlockCount);
  EXPECT_EQ(Linked->auditReceipt().FinalEdgeCount, GraphAudit->EdgeCount);

  RuntimeGuestStateX86_64V1 RuntimeState =
      llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
  GuestMemoryRuntimeConfig MemoryConfig;
  MemoryConfig.CodeInvalidation =
      CodeInvalidationPolicy::InvalidateOnExecutableWrite;
  std::unique_ptr<GuestMemoryRuntime> Memory =
      llvm::cantFail(GuestMemoryRuntime::create(State, MemoryConfig));
  RuntimeCallFrameV1 Frame =
      llvm::cantFail(createRuntimeCallFrameV1(*Memory, Credential, Credential));

  llvm::Expected<uint32_t> Status = Linked->invoke(RuntimeState, Frame);
  ASSERT_TRUE(static_cast<bool>(Status)) << llvm::toString(Status.takeError());
  EXPECT_EQ(*Status, static_cast<uint32_t>(BlockExitKindV1::DirectBranch));
  EXPECT_EQ(RuntimeState.RIP, EntryPC);
  EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::None);

  EXPECT_FALSE(static_cast<bool>(Linked->unload()));
}

TEST(TranslationJITLinker,
     InvokesAuditedZeroFlagBranchesWithoutRuntimeReferences) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  struct BranchCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    bool TakenWhenZF;
  };
  const std::array<BranchCase, 4> Cases = {{
      {"short-je", {0x74, 0xfe}, true},
      {"short-jne", {0x75, 0xfe}, false},
      {"near-je", {0x0f, 0x84, 0xfa, 0xff, 0xff, 0xff}, true},
      {"near-jne", {0x0f, 0x85, 0xfa, 0xff, 0xff, 0xff}, false},
  }};

  uint64_t BlockID = 20;
  for (const BranchCase &Case : Cases) {
    for (bool ZF : {false, true}) {
      SCOPED_TRACE(Case.Name);
      SCOPED_TRACE(ZF);
      GuestState State =
          llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
      State.Memory.push_back(
          {EntryPC, MemoryPermission::Read | MemoryPermission::Execute,
           /*Generation=*/29, Case.Bytes});
      llvm::cantFail(setRegisterValue(State, 16, llvm::APInt(64, EntryPC)));
      llvm::cantFail(setRegisterValue(
          State, 17, llvm::APInt(64, ZF ? uint64_t{1} << 6 : 0)));

      llvm::Expected<TranslationObjectResultV1> ResultOrErr =
          compileTranslationObjectRequestV1(
              {State, EntryPC, nativeJITOptions()});
      ASSERT_TRUE(static_cast<bool>(ResultOrErr))
          << llvm::toString(ResultOrErr.takeError());
      llvm::Expected<TranslationLinkGraphAuditV1> GraphAudit =
          verifyTranslationLinkGraphV1(ResultOrErr->artifact());
      ASSERT_TRUE(static_cast<bool>(GraphAudit))
          << llvm::toString(GraphAudit.takeError());
      EXPECT_EQ(GraphAudit->ExternalSymbolCount, 0u);
      EXPECT_EQ(GraphAudit->EdgeCount, 0u);

      RuntimeSymbolRegistryV1 Registry =
          llvm::cantFail(RuntimeSymbolRegistryV1::create());
      const RuntimeCodeCredentialV1 Credential = {
          /*SessionID=*/19, BlockID++, /*EntryPC=*/EntryPC,
          /*CacheGeneration=*/21, /*CodeEpoch=*/22};
      llvm::Expected<LinkedTranslationBlockV1> Linked =
          linkTranslationObjectV1(*ResultOrErr, Registry, Credential);
      ASSERT_TRUE(static_cast<bool>(Linked))
          << llvm::toString(Linked.takeError());
      EXPECT_EQ(Linked->auditReceipt().RuntimeReferenceCount, 0u);
      EXPECT_EQ(Linked->auditReceipt().StubCount, 0u);
      EXPECT_EQ(Linked->auditReceipt().GOTEntryCount, 0u);
      EXPECT_EQ(Linked->auditReceipt().FinalEdgeCount, 0u);

      RuntimeGuestStateX86_64V1 RuntimeState =
          llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
      std::unique_ptr<GuestMemoryRuntime> Memory =
          llvm::cantFail(GuestMemoryRuntime::create(State));
      RuntimeCallFrameV1 Frame = llvm::cantFail(
          createRuntimeCallFrameV1(*Memory, Credential, Credential));
      llvm::Expected<uint32_t> Status = Linked->invoke(RuntimeState, Frame);
      ASSERT_TRUE(static_cast<bool>(Status))
          << llvm::toString(Status.takeError());
      EXPECT_EQ(*Status, static_cast<uint32_t>(BlockExitKindV1::DirectBranch));
      const bool IsTaken = ZF == Case.TakenWhenZF;
      EXPECT_EQ(RuntimeState.RIP,
                IsTaken ? EntryPC : EntryPC + Case.Bytes.size());
      EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::None);
      EXPECT_FALSE(static_cast<bool>(Linked->unload()));
    }
  }
}

TEST(TranslationJITLinker,
     InvokesAuditedSingleFlagBranchesWithoutRuntimeReferences) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  struct BranchCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    uint64_t FlagMask;
    bool TakenWhenSet;
  };
  const std::array<BranchCase, 16> Cases = {{
      {"short-jo", {0x70, 0xfe}, uint64_t{1} << 11, true},
      {"near-jo",
       {0x0f, 0x80, 0xfa, 0xff, 0xff, 0xff},
       uint64_t{1} << 11,
       true},
      {"short-jno", {0x71, 0xfe}, uint64_t{1} << 11, false},
      {"near-jno",
       {0x0f, 0x81, 0xfa, 0xff, 0xff, 0xff},
       uint64_t{1} << 11,
       false},
      {"short-jb", {0x72, 0xfe}, uint64_t{1}, true},
      {"near-jb", {0x0f, 0x82, 0xfa, 0xff, 0xff, 0xff}, uint64_t{1}, true},
      {"short-jae", {0x73, 0xfe}, uint64_t{1}, false},
      {"near-jae", {0x0f, 0x83, 0xfa, 0xff, 0xff, 0xff}, uint64_t{1}, false},
      {"short-js", {0x78, 0xfe}, uint64_t{1} << 7, true},
      {"near-js", {0x0f, 0x88, 0xfa, 0xff, 0xff, 0xff}, uint64_t{1} << 7, true},
      {"short-jns", {0x79, 0xfe}, uint64_t{1} << 7, false},
      {"near-jns",
       {0x0f, 0x89, 0xfa, 0xff, 0xff, 0xff},
       uint64_t{1} << 7,
       false},
      {"short-jp", {0x7a, 0xfe}, uint64_t{1} << 2, true},
      {"near-jp", {0x0f, 0x8a, 0xfa, 0xff, 0xff, 0xff}, uint64_t{1} << 2, true},
      {"short-jnp", {0x7b, 0xfe}, uint64_t{1} << 2, false},
      {"near-jnp",
       {0x0f, 0x8b, 0xfa, 0xff, 0xff, 0xff},
       uint64_t{1} << 2,
       false},
  }};

  uint64_t BlockID = 40;
  for (const BranchCase &Case : Cases) {
    for (bool FlagSet : {false, true}) {
      SCOPED_TRACE(Case.Name);
      SCOPED_TRACE(FlagSet);
      GuestState State =
          llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
      State.Memory.push_back(
          {EntryPC, MemoryPermission::Read | MemoryPermission::Execute,
           /*Generation=*/31, Case.Bytes});
      llvm::cantFail(setRegisterValue(State, 16, llvm::APInt(64, EntryPC)));
      llvm::cantFail(setRegisterValue(
          State, 17, llvm::APInt(64, FlagSet ? Case.FlagMask : 0)));

      llvm::Expected<TranslationObjectResultV1> ResultOrErr =
          compileTranslationObjectRequestV1(
              {State, EntryPC, nativeJITOptions()});
      ASSERT_TRUE(static_cast<bool>(ResultOrErr))
          << llvm::toString(ResultOrErr.takeError());
      llvm::Expected<TranslationLinkGraphAuditV1> GraphAudit =
          verifyTranslationLinkGraphV1(ResultOrErr->artifact());
      ASSERT_TRUE(static_cast<bool>(GraphAudit))
          << llvm::toString(GraphAudit.takeError());
      EXPECT_EQ(GraphAudit->ExternalSymbolCount, 0u);
      EXPECT_EQ(GraphAudit->EdgeCount, 0u);

      RuntimeSymbolRegistryV1 Registry =
          llvm::cantFail(RuntimeSymbolRegistryV1::create());
      const RuntimeCodeCredentialV1 Credential = {
          /*SessionID=*/39, BlockID++, /*EntryPC=*/EntryPC,
          /*CacheGeneration=*/41, /*CodeEpoch=*/42};
      llvm::Expected<LinkedTranslationBlockV1> Linked =
          linkTranslationObjectV1(*ResultOrErr, Registry, Credential);
      ASSERT_TRUE(static_cast<bool>(Linked))
          << llvm::toString(Linked.takeError());
      EXPECT_EQ(Linked->auditReceipt().RuntimeReferenceCount, 0u);
      EXPECT_EQ(Linked->auditReceipt().StubCount, 0u);
      EXPECT_EQ(Linked->auditReceipt().GOTEntryCount, 0u);
      EXPECT_EQ(Linked->auditReceipt().FinalEdgeCount, 0u);

      RuntimeGuestStateX86_64V1 RuntimeState =
          llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
      std::unique_ptr<GuestMemoryRuntime> Memory =
          llvm::cantFail(GuestMemoryRuntime::create(State));
      RuntimeCallFrameV1 Frame = llvm::cantFail(
          createRuntimeCallFrameV1(*Memory, Credential, Credential));
      llvm::Expected<uint32_t> Status = Linked->invoke(RuntimeState, Frame);
      ASSERT_TRUE(static_cast<bool>(Status))
          << llvm::toString(Status.takeError());
      EXPECT_EQ(*Status, static_cast<uint32_t>(BlockExitKindV1::DirectBranch));
      const bool IsTaken = FlagSet == Case.TakenWhenSet;
      EXPECT_EQ(RuntimeState.RIP,
                IsTaken ? EntryPC : EntryPC + Case.Bytes.size());
      EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::None);
      EXPECT_FALSE(static_cast<bool>(Linked->unload()));
    }
  }
}

TEST(TranslationJITLinker,
     InvokesEveryMultiFlagBranchTruthTableWithoutRuntimeReferences) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  enum class Predicate {
    BelowOrEqual,
    Above,
    Less,
    GreaterOrEqual,
    LessOrEqual,
    Greater,
  };
  struct BranchCase {
    const char *Name;
    std::vector<uint8_t> Bytes;
    Predicate Condition;
  };
  const std::array<BranchCase, 12> Cases = {{
      {"short-jbe", {0x76, 0xfe}, Predicate::BelowOrEqual},
      {"near-jbe",
       {0x0f, 0x86, 0xfa, 0xff, 0xff, 0xff},
       Predicate::BelowOrEqual},
      {"short-ja", {0x77, 0xfe}, Predicate::Above},
      {"near-ja", {0x0f, 0x87, 0xfa, 0xff, 0xff, 0xff}, Predicate::Above},
      {"short-jl", {0x7c, 0xfe}, Predicate::Less},
      {"near-jl", {0x0f, 0x8c, 0xfa, 0xff, 0xff, 0xff}, Predicate::Less},
      {"short-jge", {0x7d, 0xfe}, Predicate::GreaterOrEqual},
      {"near-jge",
       {0x0f, 0x8d, 0xfa, 0xff, 0xff, 0xff},
       Predicate::GreaterOrEqual},
      {"short-jle", {0x7e, 0xfe}, Predicate::LessOrEqual},
      {"near-jle",
       {0x0f, 0x8e, 0xfa, 0xff, 0xff, 0xff},
       Predicate::LessOrEqual},
      {"short-jg", {0x7f, 0xfe}, Predicate::Greater},
      {"near-jg", {0x0f, 0x8f, 0xfa, 0xff, 0xff, 0xff}, Predicate::Greater},
  }};

  const auto IsTaken = [](Predicate Condition, bool CF, bool ZF, bool SF,
                          bool OF) {
    switch (Condition) {
    case Predicate::BelowOrEqual:
      return CF || ZF;
    case Predicate::Above:
      return !CF && !ZF;
    case Predicate::Less:
      return SF != OF;
    case Predicate::GreaterOrEqual:
      return SF == OF;
    case Predicate::LessOrEqual:
      return ZF || SF != OF;
    case Predicate::Greater:
      return !ZF && SF == OF;
    }
    return false;
  };
  const auto IsRelevantTruthRow = [](Predicate Condition, bool CF, bool ZF,
                                     bool SF, bool OF) {
    switch (Condition) {
    case Predicate::BelowOrEqual:
    case Predicate::Above:
      return !SF && !OF;
    case Predicate::Less:
    case Predicate::GreaterOrEqual:
      return !CF && !ZF;
    case Predicate::LessOrEqual:
    case Predicate::Greater:
      return !CF;
    }
    return false;
  };

  uint64_t BlockID = 80;
  for (const BranchCase &Case : Cases) {
    for (bool CF : {false, true}) {
      for (bool ZF : {false, true}) {
        for (bool SF : {false, true}) {
          for (bool OF : {false, true}) {
            if (!IsRelevantTruthRow(Case.Condition, CF, ZF, SF, OF))
              continue;
            SCOPED_TRACE(Case.Name);
            SCOPED_TRACE(CF);
            SCOPED_TRACE(ZF);
            SCOPED_TRACE(SF);
            SCOPED_TRACE(OF);

            uint64_t EFlags = CF ? uint64_t{1} : 0;
            EFlags |= ZF ? uint64_t{1} << 6 : 0;
            EFlags |= SF ? uint64_t{1} << 7 : 0;
            EFlags |= OF ? uint64_t{1} << 11 : 0;
            GuestState State = llvm::cantFail(
                createZeroedGuestState(GuestArchitecture::X86_64));
            State.Memory.push_back(
                {EntryPC, MemoryPermission::Read | MemoryPermission::Execute,
                 /*Generation=*/43, Case.Bytes});
            llvm::cantFail(
                setRegisterValue(State, 16, llvm::APInt(64, EntryPC)));
            llvm::cantFail(
                setRegisterValue(State, 17, llvm::APInt(64, EFlags)));

            llvm::Expected<TranslationObjectResultV1> ResultOrErr =
                compileTranslationObjectRequestV1(
                    {State, EntryPC, nativeJITOptions()});
            ASSERT_TRUE(static_cast<bool>(ResultOrErr))
                << llvm::toString(ResultOrErr.takeError());
            llvm::Expected<TranslationLinkGraphAuditV1> GraphAudit =
                verifyTranslationLinkGraphV1(ResultOrErr->artifact());
            ASSERT_TRUE(static_cast<bool>(GraphAudit))
                << llvm::toString(GraphAudit.takeError());
            EXPECT_EQ(GraphAudit->ExternalSymbolCount, 0u);
            EXPECT_EQ(GraphAudit->EdgeCount, 0u);

            RuntimeSymbolRegistryV1 Registry =
                llvm::cantFail(RuntimeSymbolRegistryV1::create());
            const RuntimeCodeCredentialV1 Credential = {
                /*SessionID=*/79, BlockID++, /*EntryPC=*/EntryPC,
                /*CacheGeneration=*/81, /*CodeEpoch=*/82};
            llvm::Expected<LinkedTranslationBlockV1> Linked =
                linkTranslationObjectV1(*ResultOrErr, Registry, Credential);
            ASSERT_TRUE(static_cast<bool>(Linked))
                << llvm::toString(Linked.takeError());
            EXPECT_EQ(Linked->auditReceipt().RuntimeReferenceCount, 0u);
            EXPECT_EQ(Linked->auditReceipt().StubCount, 0u);
            EXPECT_EQ(Linked->auditReceipt().GOTEntryCount, 0u);
            EXPECT_EQ(Linked->auditReceipt().FinalEdgeCount, 0u);

            RuntimeGuestStateX86_64V1 RuntimeState =
                llvm::cantFail(createRuntimeGuestStateX86_64V1(State));
            std::unique_ptr<GuestMemoryRuntime> Memory =
                llvm::cantFail(GuestMemoryRuntime::create(State));
            RuntimeCallFrameV1 Frame = llvm::cantFail(
                createRuntimeCallFrameV1(*Memory, Credential, Credential));
            llvm::Expected<uint32_t> Status =
                Linked->invoke(RuntimeState, Frame);
            ASSERT_TRUE(static_cast<bool>(Status))
                << llvm::toString(Status.takeError());
            EXPECT_EQ(*Status,
                      static_cast<uint32_t>(BlockExitKindV1::DirectBranch));
            EXPECT_EQ(RuntimeState.RIP, IsTaken(Case.Condition, CF, ZF, SF, OF)
                                            ? EntryPC
                                            : EntryPC + Case.Bytes.size());
            EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::None);
            EXPECT_FALSE(static_cast<bool>(Linked->unload()));
          }
        }
      }
    }
  }
}

} // namespace
