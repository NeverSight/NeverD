//===- TranslationObjectRequestTests.cpp - x86-64 object vertical slice -===//

#include "TranslationCacheIdentity.h"
#include "gtest/gtest.h"

#include "neverd/translate/GuestState.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::translate;
using neverd::LowOp;
using neverd::NdMemoryOrdering;
using neverd::NdOp;
using neverd::NdVar;
using neverd::TmpBase;
using neverd::TmpStride;

namespace {

static_assert(kTranslationObjectWrapperCacheIdentityVersionV1 == 1);
static_assert(kTranslationObjectWrapperCacheIdentityVersionV2 == 2);
static_assert(kTranslationObjectWrapperCacheIdentityVersion ==
              kTranslationObjectWrapperCacheIdentityVersionV2);

constexpr uint64_t EntryPC = 0x401000;
constexpr llvm::StringLiteral
    ExpectedBlockSymbol("nvd_x86_64_block_0000000000401000");

TranslationOptions aarch64Options(llvm::StringRef Triple) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = GuestArchitecture::AArch64;
  Options.Target.Triple = Triple.str();
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  Options.LLVMLevel = LLVMOptimizationLevel::O2;
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
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  return Options;
}

GuestState stateForBytes(std::vector<uint8_t> Bytes, uint64_t Generation = 17) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          Generation, std::move(Bytes)});
  return State;
}

GuestState fixtureState(uint64_t Generation = 17) {
  return stateForBytes({0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3},
                       Generation);
}

std::unique_ptr<llvm::object::ObjectFile>
parseObject(const TranslationObjectArtifactV1 &Artifact) {
  const llvm::ArrayRef<uint8_t> Bytes = Artifact.bytes();
  const llvm::StringRef Contents(reinterpret_cast<const char *>(Bytes.data()),
                                 Bytes.size());
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> ObjectOrErr =
      llvm::object::ObjectFile::createObjectFile(
          llvm::MemoryBufferRef(Contents, "x86-aarch64-translation"));
  if (!ObjectOrErr) {
    ADD_FAILURE() << llvm::toString(ObjectOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjectOrErr);
}

std::vector<std::string>
undefinedSymbols(const llvm::object::ObjectFile &Object) {
  std::vector<std::string> Result;
  for (const llvm::object::SymbolRef &Symbol : Object.symbols()) {
    llvm::Expected<uint32_t> FlagsOrErr = Symbol.getFlags();
    if (!FlagsOrErr) {
      ADD_FAILURE() << llvm::toString(FlagsOrErr.takeError());
      return {};
    }
    if ((*FlagsOrErr & llvm::object::SymbolRef::SF_Undefined) == 0)
      continue;
    llvm::Expected<llvm::StringRef> NameOrErr = Symbol.getName();
    if (!NameOrErr) {
      ADD_FAILURE() << llvm::toString(NameOrErr.takeError());
      return {};
    }
    if (!NameOrErr->empty())
      Result.push_back(NameOrErr->str());
  }
  llvm::sort(Result);
  return Result;
}

std::string descriptorIdentity(const TranslationBlockDescriptorV1 &Descriptor) {
  detail::StableHashWriter Hash;
  detail::hashTranslationBlockDescriptor(Hash, Descriptor);
  return Hash.finish("test.translation-block.sha256:");
}

void expectRequestError(
    llvm::Expected<TranslationObjectResultV1> Result,
    TranslationObjectRequestErrorCode ExpectedCode,
    std::optional<X86TranslationBlockBuilderErrorCode> BuilderCode =
        std::nullopt,
    std::optional<TranslationBlockLoweringErrorCode> LoweringCode =
        std::nullopt,
    std::optional<TranslationObjectCompilerErrorCode> CompilerCode =
        std::nullopt,
    std::optional<uint64_t> BuilderGuestPC = std::nullopt,
    std::optional<RuntimeMemoryFaultKindV1> BuilderMemoryFault = std::nullopt,
    std::optional<uint64_t> LoweringGuestPC = std::nullopt) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationObjectRequestError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(), ExpectedCode);
        EXPECT_EQ(Error.builderCode(), BuilderCode);
        EXPECT_EQ(Error.builderGuestPC(), BuilderGuestPC);
        EXPECT_EQ(Error.builderMemoryFault(), BuilderMemoryFault);
        EXPECT_EQ(Error.loweringCode(), LoweringCode);
        EXPECT_EQ(Error.loweringGuestPC(), LoweringGuestPC);
        EXPECT_EQ(Error.compilerCode(), CompilerCode);
        if (ExpectedCode ==
            TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded) {
          ASSERT_TRUE(Error.compilerBudgetObserved().has_value());
          ASSERT_TRUE(Error.compilerBudgetLimit().has_value());
          ASSERT_TRUE(Error.guestInstructionCount().has_value());
          EXPECT_GT(*Error.compilerBudgetObserved(),
                    *Error.compilerBudgetLimit());
          EXPECT_EQ(*Error.compilerBudgetLimit(), 1u);
          EXPECT_EQ(*Error.guestInstructionCount(), 3u);
        }
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

void expectBlockConstructionError(
    llvm::Expected<TranslationObjectResultV1> Result,
    X86TranslationBlockBuilderErrorCode ExpectedBuilderCode,
    uint64_t ExpectedGuestPC,
    std::optional<RuntimeMemoryFaultKindV1> ExpectedMemoryFault) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationObjectRequestError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(),
                  TranslationObjectRequestErrorCode::BlockConstructionFailed);
        EXPECT_EQ(Error.builderCode(), ExpectedBuilderCode);
        EXPECT_EQ(Error.builderGuestPC(), ExpectedGuestPC);
        EXPECT_EQ(Error.builderMemoryFault(), ExpectedMemoryFault);
        if (ExpectedMemoryFault) {
          ASSERT_TRUE(Error.builderMemoryFaultDetails().has_value());
          const GuestMemoryFault &Fault = *Error.builderMemoryFaultDetails();
          EXPECT_EQ(Fault.Kind, *ExpectedMemoryFault);
          EXPECT_EQ(Fault.Exit.Address, ExpectedGuestPC);
          EXPECT_EQ(Fault.Exit.Access, MemoryAccessKind::Execute);
          EXPECT_EQ(Fault.Exit.AccessWidthBits, 8u);
          EXPECT_EQ(Fault.Exit.RequiredAlignment, 0u);
          EXPECT_EQ(Fault.AccessSize, 1u);
          EXPECT_EQ(Fault.ExpectedGeneration, 0u);
          EXPECT_EQ(Fault.ObservedGeneration, 0u);
        } else {
          EXPECT_FALSE(Error.builderMemoryFaultDetails().has_value());
        }
        EXPECT_FALSE(Error.targetCode().has_value());
        EXPECT_FALSE(Error.loweringCode().has_value());
        EXPECT_FALSE(Error.compilerCode().has_value());
        EXPECT_FALSE(Error.compilerBudgetObserved().has_value());
        EXPECT_FALSE(Error.compilerBudgetLimit().has_value());
        EXPECT_FALSE(Error.guestInstructionCount().has_value());
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

TEST(TranslationObjectRequest,
     ProducesAuditedAArch64ELFFromExactX86GuestBytes) {
  GuestState State = fixtureState();
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState(State));
  const TranslationObjectRequestV1 Request(
      State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu"));

  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1(Request);
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  const TranslationObjectResultV1 &Result = *ResultOrErr;

  EXPECT_EQ(
      Result.descriptor().Bytes,
      (std::vector<uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3}));
  ASSERT_EQ(Result.descriptor().GenerationBindings.size(), 1u);
  EXPECT_EQ(Result.descriptor().GenerationBindings.front().Generation, 17u);
  ASSERT_EQ(Result.artifact().blockSymbols().size(), 1u);
  EXPECT_EQ(Result.artifact().blockSymbols().front().IRName,
            ExpectedBlockSymbol);
  EXPECT_EQ(Result.artifact().hostTarget().architecture(),
            GuestArchitecture::AArch64);
  EXPECT_TRUE(Result.artifact().llvmOptimizationPipelineRan());
  EXPECT_GT(Result.artifact().semanticReport().FunctionPassInvocations, 0u);
  EXPECT_TRUE(Result.cacheIdentity().starts_with(
      "neverd.translation-object-wrapper.v1.sha256:"));
  EXPECT_FALSE(Result.artifact().requestCacheKey().empty());
  EXPECT_FALSE(Result.artifact().artifactCacheKey().empty());

  std::unique_ptr<llvm::object::ObjectFile> Object =
      parseObject(Result.artifact());
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_TRUE(Object->isELF());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);

  std::optional<std::string> ExpectedRuntimeSymbol;
  for (const TranslationObjectSymbolV1 &Symbol :
       Result.artifact().runtimeSymbols())
    if (Symbol.IRName == "nvd_rt_v1_load64_le")
      ExpectedRuntimeSymbol = Symbol.ObjectName;
  ASSERT_TRUE(ExpectedRuntimeSymbol.has_value());
  EXPECT_EQ(undefinedSymbols(*Object),
            std::vector<std::string>({*ExpectedRuntimeSymbol}));
  EXPECT_EQ(llvm::cantFail(serializeGuestState(State)), Before);
}

TEST(TranslationObjectRequest, EmitsAArch64MachORelocatableObject) {
  GuestState State = fixtureState();
  const TranslationObjectRequestV1 Request(
      State, EntryPC, aarch64Options("aarch64-apple-macosx"));
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1(Request);
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());

  std::unique_ptr<llvm::object::ObjectFile> Object =
      parseObject(ResultOrErr->artifact());
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_TRUE(Object->isMachO());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
  ASSERT_EQ(ResultOrErr->artifact().blockSymbols().size(), 1u);
  EXPECT_EQ(ResultOrErr->artifact().blockSymbols().front().ObjectName,
            std::string("_") + ExpectedBlockSymbol.str());
}

TEST(TranslationObjectRequest,
     EmitsOneAuditedNativeArtifactForAnAArch64JITProcess) {
  GuestState State = fixtureState();
  const TranslationOptions Options = nativeJITOptions();
  llvm::Expected<ResolvedHostTarget> HostOrErr = resolveHostTarget(Options);
  ASSERT_TRUE(static_cast<bool>(HostOrErr))
      << llvm::toString(HostOrErr.takeError());

  if (HostOrErr->architecture() != GuestArchitecture::AArch64) {
    expectRequestError(
        compileTranslationObjectRequestV1({State, EntryPC, Options}),
        TranslationObjectRequestErrorCode::InvalidRequest);
    return;
  }

  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1({State, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->artifact().hostTarget().requestedTarget().Kind,
            HostTargetKind::Native);
  EXPECT_EQ(ResultOrErr->artifact().hostTarget().architecture(),
            GuestArchitecture::AArch64);
  ASSERT_EQ(ResultOrErr->artifact().blockSymbols().size(), 1u);
  EXPECT_EQ(ResultOrErr->artifact().blockSymbols().front().IRName,
            ExpectedBlockSymbol);

  std::unique_ptr<llvm::object::ObjectFile> Object =
      parseObject(ResultOrErr->artifact());
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
}

TEST(TranslationObjectRequest,
     WholePipelineIdentityIsStableAndBindsExecutableGeneration) {
  GuestState FirstState = fixtureState(21);
  GuestState SameState = fixtureState(21);
  GuestState NewGeneration = fixtureState(22);
  const TranslationOptions Options =
      aarch64Options("aarch64-unknown-linux-gnu");

  auto FirstOrErr =
      compileTranslationObjectRequestV1({FirstState, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(FirstOrErr))
      << llvm::toString(FirstOrErr.takeError());
  auto SameOrErr =
      compileTranslationObjectRequestV1({SameState, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(SameOrErr))
      << llvm::toString(SameOrErr.takeError());
  auto ChangedOrErr =
      compileTranslationObjectRequestV1({NewGeneration, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(ChangedOrErr))
      << llvm::toString(ChangedOrErr.takeError());
  TranslationObjectResultV1 First = std::move(*FirstOrErr);
  TranslationObjectResultV1 Same = std::move(*SameOrErr);
  TranslationObjectResultV1 Changed = std::move(*ChangedOrErr);

  EXPECT_EQ(First.cacheIdentity(), Same.cacheIdentity());
  EXPECT_EQ(First.artifact().requestCacheKey(),
            Same.artifact().requestCacheKey());
  EXPECT_EQ(First.artifact().artifactCacheKey(),
            Same.artifact().artifactCacheKey());
  EXPECT_NE(First.cacheIdentity(), Changed.cacheIdentity());
  // Generation is dispatcher provenance rather than LLVM IR, so only the
  // wrapper key changes for an otherwise byte-identical object request.
  EXPECT_EQ(First.artifact().requestCacheKey(),
            Changed.artifact().requestCacheKey());
}

TEST(TranslationObjectRequest,
     CacheIdentityTreatsGuestFeaturesAsAnOrderIndependentSet) {
  GuestState FirstState = fixtureState(21);
  FirstState.Features = {"avx2", "bmi2"};
  GuestState PermutedState = fixtureState(21);
  PermutedState.Features = {"bmi2", "avx2"};
  const TranslationOptions Options =
      aarch64Options("aarch64-unknown-linux-gnu");

  auto FirstOrErr =
      compileTranslationObjectRequestV1({FirstState, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(FirstOrErr))
      << llvm::toString(FirstOrErr.takeError());
  auto PermutedOrErr =
      compileTranslationObjectRequestV1({PermutedState, EntryPC, Options});
  ASSERT_TRUE(static_cast<bool>(PermutedOrErr))
      << llvm::toString(PermutedOrErr.takeError());

  EXPECT_EQ(FirstOrErr->cacheIdentity(), PermutedOrErr->cacheIdentity());
  EXPECT_EQ(FirstOrErr->artifact().requestCacheKey(),
            PermutedOrErr->artifact().requestCacheKey());
  EXPECT_EQ(FirstOrErr->artifact().artifactCacheKey(),
            PermutedOrErr->artifact().artifactCacheKey());
}

TEST(TranslationObjectRequest,
     CacheIdentityDistinguishesAddressOccurrenceOwners) {
  TranslationBlockDescriptorV1 First;
  LowOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = NdVar::tmp(TmpBase, 8);
  Op.addInput(NdVar::dataAddress(0x2000, 8, 0x1000));
  First.Ops.push_back(Op);

  TranslationBlockDescriptorV1 Second = First;
  Second.Ops.front().Inputs[0].AddressOwnerVA = 0x2000;

  ASSERT_NE(First.Ops.front().Inputs[0], Second.Ops.front().Inputs[0]);
  EXPECT_NE(descriptorIdentity(First), descriptorIdentity(Second));
}

TEST(TranslationObjectRequest, CacheIdentityDistinguishesMemoryOrdering) {
  TranslationBlockDescriptorV1 Relaxed;
  LowOp Op;
  Op.Opcode = NdOp::LOAD;
  Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Op.Output = NdVar::tmp(TmpBase, 8);
  Op.addInput(NdVar::tmp(TmpBase + TmpStride, 8));
  Relaxed.Ops.push_back(Op);

  TranslationBlockDescriptorV1 Acquire = Relaxed;
  Acquire.Ops.front().MemoryOrdering = NdMemoryOrdering::Acquire;

  EXPECT_NE(descriptorIdentity(Relaxed), descriptorIdentity(Acquire));
}

TEST(TranslationObjectRequest, RejectsUnsupportedModeAndHostBeforeLowering) {
  GuestState State = fixtureState();
  TranslationOptions JIT = aarch64Options("aarch64-unknown-linux-gnu");
  JIT.Mode = TranslationMode::JIT;
  expectRequestError(compileTranslationObjectRequestV1({State, EntryPC, JIT}),
                     TranslationObjectRequestErrorCode::InvalidRequest);

  TranslationOptions WrongHost = aarch64Options("aarch64-unknown-linux-gnu");
  WrongHost.Target.Architecture = GuestArchitecture::X86_64;
  WrongHost.Target.Triple = "x86_64-unknown-linux-gnu";
  expectRequestError(
      compileTranslationObjectRequestV1({State, EntryPC, WrongHost}),
      TranslationObjectRequestErrorCode::InvalidRequest);

  TranslationOptions COFF = aarch64Options("aarch64-pc-windows-msvc");
  expectRequestError(compileTranslationObjectRequestV1({State, EntryPC, COFF}),
                     TranslationObjectRequestErrorCode::InvalidRequest);
}

TEST(TranslationObjectRequest,
     PreservesUnmappedInstructionFetchAtTheRequestBoundary) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));

  expectBlockConstructionError(
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")}),
      X86TranslationBlockBuilderErrorCode::InstructionFetchFailed, EntryPC,
      RuntimeMemoryFaultKindV1::Unmapped);
}

TEST(TranslationObjectRequest,
     PreservesExecutePermissionFailureAtTheRequestBoundary) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC, MemoryPermission::Read, /*Generation=*/17, {0xc3}});

  expectBlockConstructionError(
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")}),
      X86TranslationBlockBuilderErrorCode::InstructionFetchFailed, EntryPC,
      RuntimeMemoryFaultKindV1::PermissionDenied);
}

TEST(TranslationObjectRequest,
     PreservesUnsupportedInstructionAtItsExactGuestPC) {
  // mov rax, rdi followed by a full maximum-length window that is invalid in
  // x86-64 mode.  The unsupported second instruction must fail closed.
  GuestState State =
      stateForBytes({0x48, 0x89, 0xf8, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
                     0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06});

  expectBlockConstructionError(
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")}),
      X86TranslationBlockBuilderErrorCode::UndecodableInstruction, EntryPC + 3,
      std::nullopt);
}

TEST(TranslationObjectRequest,
     TranslatesGeneralPublishedScalarSliceWithoutMutatingGuestState) {
  GuestState State = stateForBytes(
      {0x48, 0x89, 0xd8, 0x48, 0x83, 0xe8, 0x02, 0xc2, 0x10, 0x00});
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState(State));
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->descriptor().Header.GuestInstructionCount, 3u);
  EXPECT_EQ(ResultOrErr->descriptor().Header.ReturnImmediate, 0x10u);
  EXPECT_TRUE(ResultOrErr->artifact().llvmOptimizationPipelineRan());
  EXPECT_EQ(llvm::cantFail(serializeGuestState(State)), Before);
}

TEST(TranslationObjectRequest,
     EmitsAuditedObjectForRegisterAndSignedImmediateTruthVectorForms) {
  const std::vector<uint8_t> Bytes = {
      0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x48, 0xbb,
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x01, 0xd8, 0x48,
      0x83, 0xe8, 0xff, 0x48, 0x05, 0x00, 0x00, 0x00, 0x80, 0xc2, 0x20, 0x00,
  };
  GuestState State = stateForBytes(Bytes);
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState(State));

  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->descriptor().Bytes, Bytes);
  EXPECT_EQ(ResultOrErr->descriptor().Header.GuestInstructionCount, 6u);
  EXPECT_EQ(ResultOrErr->descriptor().Header.ReturnImmediate, 0x20u);
  EXPECT_EQ(llvm::cantFail(serializeGuestState(State)), Before);

  std::unique_ptr<llvm::object::ObjectFile> Object =
      parseObject(ResultOrErr->artifact());
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_TRUE(Object->isELF());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
  EXPECT_FALSE(ResultOrErr->artifact().requestCacheKey().empty());
  EXPECT_FALSE(ResultOrErr->artifact().artifactCacheKey().empty());
}

TEST(TranslationObjectRequest, EmitsAuditedObjectForLogicalScalarForms) {
  const std::vector<uint8_t> Bytes = {
      0x48, 0xb8, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, // movabs
      0x48, 0xbb, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, // movabs
      0x48, 0x21, 0xd8,                                           // and
      0x48, 0x83, 0xc8, 0xff,                                     // or -1
      0x48, 0x31, 0xd8,                                           // xor
      0xc3,                                                       // ret
  };
  GuestState State = stateForBytes(Bytes);
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->descriptor().Bytes, Bytes);
  EXPECT_EQ(ResultOrErr->descriptor().Header.GuestInstructionCount, 6u);
  EXPECT_TRUE(ResultOrErr->artifact().llvmOptimizationPipelineRan());
  std::unique_ptr<llvm::object::ObjectFile> Object =
      parseObject(ResultOrErr->artifact());
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->isRelocatableObject());
  EXPECT_TRUE(Object->isELF());
  EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
}

TEST(TranslationObjectRequest,
     EmitsAuditedObjectsForCanonicalRel8AndRel32DirectJumps) {
  const std::array<std::vector<uint8_t>, 2> Encodings = {{
      {0xeb, 0xfe},                   // jmp EntryPC
      {0xe9, 0xfb, 0xff, 0xff, 0xff}, // jmp EntryPC
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    GuestState State = stateForBytes(Bytes);
    llvm::Expected<TranslationObjectResultV1> ResultOrErr =
        compileTranslationObjectRequestV1(
            {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
    ASSERT_TRUE(static_cast<bool>(ResultOrErr))
        << llvm::toString(ResultOrErr.takeError());
    EXPECT_EQ(ResultOrErr->descriptor().Bytes, Bytes);
    EXPECT_EQ(ResultOrErr->descriptor().Header.Terminator,
              TranslationBlockTerminatorKindV1::DirectBranch);
    EXPECT_EQ(ResultOrErr->descriptor().Header.StaticTargetPC, EntryPC);
    ASSERT_EQ(ResultOrErr->artifact().blockSymbols().size(), 1u);
    RuntimeSymbolRegistryV1 Registry =
        llvm::cantFail(RuntimeSymbolRegistryV1::create());
    EXPECT_EQ(ResultOrErr->artifact().runtimeSymbols().size(),
              Registry.entries().size());
    EXPECT_EQ(ResultOrErr->artifact().runtimeRegistryIdentity(),
              Registry.identity());

    std::unique_ptr<llvm::object::ObjectFile> Object =
        parseObject(ResultOrErr->artifact());
    ASSERT_NE(Object, nullptr);
    EXPECT_TRUE(Object->isRelocatableObject());
    EXPECT_TRUE(Object->isELF());
    EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
    EXPECT_TRUE(undefinedSymbols(*Object).empty());
  }
}

TEST(TranslationObjectRequest,
     EmitsAuditedObjectsForCanonicalZeroFlagConditionalBranches) {
  const std::array<std::vector<uint8_t>, 4> Encodings = {{
      {0x74, 0xfe},                         // je EntryPC
      {0x75, 0xfe},                         // jne EntryPC
      {0x0f, 0x84, 0xfa, 0xff, 0xff, 0xff}, // je EntryPC
      {0x0f, 0x85, 0xfa, 0xff, 0xff, 0xff}, // jne EntryPC
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    GuestState State = stateForBytes(Bytes);
    llvm::Expected<TranslationObjectResultV1> ResultOrErr =
        compileTranslationObjectRequestV1(
            {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
    ASSERT_TRUE(static_cast<bool>(ResultOrErr))
        << llvm::toString(ResultOrErr.takeError());
    EXPECT_EQ(ResultOrErr->descriptor().Bytes, Bytes);
    EXPECT_EQ(ResultOrErr->descriptor().Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    EXPECT_EQ(ResultOrErr->descriptor().Header.StaticTargetPC, EntryPC);

    std::unique_ptr<llvm::object::ObjectFile> Object =
        parseObject(ResultOrErr->artifact());
    ASSERT_NE(Object, nullptr);
    EXPECT_TRUE(Object->isRelocatableObject());
    EXPECT_TRUE(Object->isELF());
    EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
    EXPECT_TRUE(undefinedSymbols(*Object).empty());
  }
}

TEST(TranslationObjectRequest,
     EmitsAuditedObjectsForCanonicalSingleFlagConditionalBranches) {
  const std::array<std::vector<uint8_t>, 16> Encodings = {{
      {0x70, 0xfe},
      {0x0f, 0x80, 0xfa, 0xff, 0xff, 0xff},
      {0x71, 0xfe},
      {0x0f, 0x81, 0xfa, 0xff, 0xff, 0xff},
      {0x72, 0xfe},
      {0x0f, 0x82, 0xfa, 0xff, 0xff, 0xff},
      {0x73, 0xfe},
      {0x0f, 0x83, 0xfa, 0xff, 0xff, 0xff},
      {0x78, 0xfe},
      {0x0f, 0x88, 0xfa, 0xff, 0xff, 0xff},
      {0x79, 0xfe},
      {0x0f, 0x89, 0xfa, 0xff, 0xff, 0xff},
      {0x7a, 0xfe},
      {0x0f, 0x8a, 0xfa, 0xff, 0xff, 0xff},
      {0x7b, 0xfe},
      {0x0f, 0x8b, 0xfa, 0xff, 0xff, 0xff},
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    GuestState State = stateForBytes(Bytes);
    llvm::Expected<TranslationObjectResultV1> ResultOrErr =
        compileTranslationObjectRequestV1(
            {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
    ASSERT_TRUE(static_cast<bool>(ResultOrErr))
        << llvm::toString(ResultOrErr.takeError());
    EXPECT_EQ(ResultOrErr->descriptor().Bytes, Bytes);
    EXPECT_EQ(ResultOrErr->descriptor().Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    EXPECT_EQ(ResultOrErr->descriptor().Header.StaticTargetPC, EntryPC);

    std::unique_ptr<llvm::object::ObjectFile> Object =
        parseObject(ResultOrErr->artifact());
    ASSERT_NE(Object, nullptr);
    EXPECT_TRUE(Object->isRelocatableObject());
    EXPECT_TRUE(Object->isELF());
    EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
    EXPECT_TRUE(undefinedSymbols(*Object).empty());
  }
}

TEST(TranslationObjectRequest,
     EmitsAuditedObjectsForCanonicalMultiFlagConditionalBranches) {
  const std::array<std::vector<uint8_t>, 12> Encodings = {{
      {0x76, 0xfe},
      {0x0f, 0x86, 0xfa, 0xff, 0xff, 0xff},
      {0x77, 0xfe},
      {0x0f, 0x87, 0xfa, 0xff, 0xff, 0xff},
      {0x7c, 0xfe},
      {0x0f, 0x8c, 0xfa, 0xff, 0xff, 0xff},
      {0x7d, 0xfe},
      {0x0f, 0x8d, 0xfa, 0xff, 0xff, 0xff},
      {0x7e, 0xfe},
      {0x0f, 0x8e, 0xfa, 0xff, 0xff, 0xff},
      {0x7f, 0xfe},
      {0x0f, 0x8f, 0xfa, 0xff, 0xff, 0xff},
  }};

  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Bytes));
    GuestState State = stateForBytes(Bytes);
    llvm::Expected<TranslationObjectResultV1> ResultOrErr =
        compileTranslationObjectRequestV1(
            {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")});
    ASSERT_TRUE(static_cast<bool>(ResultOrErr))
        << llvm::toString(ResultOrErr.takeError());
    EXPECT_EQ(ResultOrErr->descriptor().Bytes, Bytes);
    EXPECT_EQ(ResultOrErr->descriptor().Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    EXPECT_EQ(ResultOrErr->descriptor().Header.StaticTargetPC, EntryPC);

    std::unique_ptr<llvm::object::ObjectFile> Object =
        parseObject(ResultOrErr->artifact());
    ASSERT_NE(Object, nullptr);
    EXPECT_TRUE(Object->isRelocatableObject());
    EXPECT_TRUE(Object->isELF());
    EXPECT_EQ(Object->getArch(), llvm::Triple::aarch64);
    EXPECT_TRUE(undefinedSymbols(*Object).empty());
  }
}

TEST(TranslationObjectRequest, RejectsUnsupportedLowIROperations) {
  GuestState State = stateForBytes({0x90, 0xc3}, /*Generation=*/1);
  expectRequestError(
      compileTranslationObjectRequestV1(
          {State, EntryPC, aarch64Options("aarch64-unknown-linux-gnu")}),
      TranslationObjectRequestErrorCode::BlockLoweringFailed, std::nullopt,
      TranslationBlockLoweringErrorCode::UnsupportedBlockShape, std::nullopt,
      std::nullopt, std::nullopt, EntryPC);
}

TEST(TranslationObjectRequest, BudgetsFailClosedWithNestedTypedErrors) {
  GuestState State = fixtureState();
  TranslationOptions InstructionBudget =
      aarch64Options("aarch64-unknown-linux-gnu");
  InstructionBudget.InstructionBudget = 2;
  expectRequestError(
      compileTranslationObjectRequestV1({State, EntryPC, InstructionBudget}),
      TranslationObjectRequestErrorCode::InstructionBudgetExceeded,
      X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded,
      std::nullopt, std::nullopt, EntryPC + 7);

  TranslationOptions ObjectBudget = aarch64Options("aarch64-unknown-linux-gnu");
  ObjectBudget.GeneratedCodeByteBudget = 1;
  expectRequestError(
      compileTranslationObjectRequestV1({State, EntryPC, ObjectBudget}),
      TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded,
      std::nullopt, std::nullopt,
      TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded);
}

} // namespace
