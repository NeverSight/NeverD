//===- GuestMemoryRuntimeTests.cpp - Checked translation memory runtime ---===//

#include "gtest/gtest.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/RuntimeABI.h"

#include "llvm/ADT/Twine.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace neverd::translate;

namespace {

constexpr llvm::StringLiteral HostTriple("aarch64-unknown-linux-gnu");
constexpr llvm::StringLiteral HostLayout("e-p:64:64-i64:64-n32:64-S128");

std::unique_ptr<llvm::Module> parseRuntimeModule(llvm::LLVMContext &Context,
                                                 llvm::StringRef Body) {
  const std::string IR = (llvm::Twine("target datalayout = \"") + HostLayout +
                          "\"\ntarget triple = \"" + HostTriple + "\"\n" + Body)
                             .str();
  llvm::SMDiagnostic Diagnostic;
  std::unique_ptr<llvm::Module> Module =
      llvm::parseAssemblyString(IR, Diagnostic, Context);
  EXPECT_NE(Module, nullptr);
  if (Module)
    for (llvm::Function &Function : *Module)
      if (!Function.isDeclaration())
        Function.setVisibility(llvm::GlobalValue::HiddenVisibility);
  return Module;
}

void expectRuntimeIRViolation(llvm::Error Error,
                              TranslationIRViolation Expected) {
  ASSERT_TRUE(static_cast<bool>(Error));
  bool Seen = false;
  llvm::handleAllErrors(std::move(Error),
                        [&](const TranslationIRVerificationError &Failure) {
                          Seen = true;
                          EXPECT_EQ(Failure.reason(), Expected);
                        });
  EXPECT_TRUE(Seen);
}

GuestState stateWithMemory(std::vector<GuestMemoryRegion> Memory) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory = std::move(Memory);
  return State;
}

std::unique_ptr<GuestMemoryRuntime>
createRuntime(const GuestState &State,
              CodeInvalidationPolicy Policy =
                  CodeInvalidationPolicy::InvalidateOnExecutableWrite,
              uint64_t InstructionBudget = 0, uint64_t BlockBudget = 0) {
  GuestMemoryRuntimeConfig Config;
  Config.CodeInvalidation = Policy;
  Config.InstructionBudget = InstructionBudget;
  Config.BlockBudget = BlockBudget;
  return llvm::cantFail(GuestMemoryRuntime::create(State, Config));
}

void expectFault(const GuestMemoryAccessResult &Result,
                 RuntimeMemoryFaultKindV1 Kind) {
  ASSERT_EQ(Result.Status, GuestMemoryAccessStatus::Fault);
  ASSERT_TRUE(Result.Fault.has_value());
  EXPECT_EQ(Result.Fault->Kind, Kind);
  EXPECT_FALSE(Result.SelfModification.has_value());
}

void expectFetchFault(const GuestInstructionFetchResult &Result,
                      RuntimeMemoryFaultKindV1 Kind, uint64_t Address,
                      uint64_t Size) {
  ASSERT_EQ(Result.Status, GuestMemoryAccessStatus::Fault);
  ASSERT_TRUE(Result.Fault.has_value());
  EXPECT_EQ(Result.Fault->Kind, Kind);
  EXPECT_EQ(Result.Fault->Exit.Address, Address);
  EXPECT_EQ(Result.Fault->Exit.Access, MemoryAccessKind::Execute);
  EXPECT_EQ(Result.Fault->AccessSize, Size);
  if (Size <= std::numeric_limits<uint32_t>::max() / 8)
    EXPECT_EQ(Result.Fault->Exit.AccessWidthBits, Size * 8);
  EXPECT_TRUE(Result.Bindings.empty());
}

void expectInvalidControl(const RuntimeControlBlockV1 &Control,
                          llvm::StringRef Detail) {
  llvm::Error Error = validateRuntimeControlBlockV1(Control);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find(Detail.str()),
            std::string::npos);
}

void expectValidControl(const RuntimeControlBlockV1 &Control) {
  EXPECT_FALSE(static_cast<bool>(validateRuntimeControlBlockV1(Control)));
}

void setMemoryFaultDetails(RuntimeControlBlockV1 &Control,
                           RuntimeMemoryAccessKindV1 Access,
                           uint32_t RequiredAlignment = 0,
                           uint64_t ExpectedGeneration = 0,
                           uint64_t ObservedGeneration = 0) {
  RuntimeMemoryFaultDetailsV1 Details;
  Details.Access = Access;
  Details.RequiredAlignment = RequiredAlignment;
  Details.ExpectedGeneration = ExpectedGeneration;
  Details.ObservedGeneration = ObservedGeneration;
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> EncodingOrErr =
      packRuntimeMemoryFaultDetailsV1(Control.Exit.Fault, Details);
  ASSERT_TRUE(static_cast<bool>(EncodingOrErr))
      << llvm::toString(EncodingOrErr.takeError());
  Control.Exit.Detail0 = EncodingOrErr->Detail0;
  Control.Exit.Detail1 = EncodingOrErr->Detail1;
  Control.ExpectedGeneration = ExpectedGeneration;
  Control.ObservedGeneration = ObservedGeneration;
}

void expectInvalidFaultDetails(RuntimeMemoryFaultKindV1 Fault,
                               const RuntimeMemoryFaultDetailsV1 &Details) {
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> EncodingOrErr =
      packRuntimeMemoryFaultDetailsV1(Fault, Details);
  ASSERT_FALSE(static_cast<bool>(EncodingOrErr));
  llvm::consumeError(EncodingOrErr.takeError());
}

void expectSnapshotFaultDetails(const RuntimeControlBlockV1 &Control,
                                RuntimeMemoryFaultKindV1 Fault,
                                RuntimeMemoryAccessKindV1 Access,
                                uint32_t RequiredAlignment,
                                uint64_t ExpectedGeneration = 0,
                                uint64_t ObservedGeneration = 0) {
  ASSERT_EQ(Control.Exit.Kind, RuntimeABIExitKindV1::MemoryFault);
  ASSERT_EQ(Control.Exit.Fault, Fault);
  llvm::Expected<RuntimeMemoryFaultDetailsV1> DetailsOrErr =
      unpackRuntimeMemoryFaultDetailsV1(
          Fault, {Control.Exit.Detail0, Control.Exit.Detail1});
  ASSERT_TRUE(static_cast<bool>(DetailsOrErr))
      << llvm::toString(DetailsOrErr.takeError());
  EXPECT_EQ(DetailsOrErr->Access, Access);
  EXPECT_EQ(DetailsOrErr->RequiredAlignment, RequiredAlignment);
  EXPECT_EQ(DetailsOrErr->ExpectedGeneration, ExpectedGeneration);
  EXPECT_EQ(DetailsOrErr->ObservedGeneration, ObservedGeneration);
  expectValidControl(Control);
}

TEST(RuntimeABI, V1LayoutAndHelperPolicyAreExact) {
  static_assert(std::is_standard_layout_v<RuntimeABIExitV1>);
  static_assert(std::is_trivially_copyable_v<RuntimeABIExitV1>);
  static_assert(std::is_standard_layout_v<RuntimeControlBlockV1>);
  static_assert(std::is_trivially_copyable_v<RuntimeControlBlockV1>);

  EXPECT_EQ(sizeof(RuntimeABIExitV1), 40u);
  EXPECT_EQ(sizeof(RuntimeControlBlockV1), kRuntimeControlBlockSizeV1);
  EXPECT_EQ(offsetof(RuntimeControlBlockV1, Flags), 12u);
  EXPECT_EQ(offsetof(RuntimeControlBlockV1, CurrentPC), 16u);
  EXPECT_EQ(offsetof(RuntimeControlBlockV1, ScalarResult), 72u);
  EXPECT_EQ(offsetof(RuntimeControlBlockV1, Exit), 88u);

  RuntimeControlBlockV1 Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::ValidateBeforeDispatch, 17, 9);
  EXPECT_EQ(Control.Magic, kRuntimeABIMagicV1);
  EXPECT_EQ(Control.Version, kRuntimeABIVersionV1);
  EXPECT_EQ(Control.Size, kRuntimeControlBlockSizeV1);
  EXPECT_EQ(
      Control.CodeInvalidation,
      static_cast<uint32_t>(CodeInvalidationPolicy::ValidateBeforeDispatch));
  EXPECT_EQ(Control.InstructionBudget, 17u);
  EXPECT_EQ(Control.BlockBudget, 9u);
  Control.Flags = static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);
  EXPECT_FALSE(static_cast<bool>(validateRuntimeControlBlockV1(Control)));

  Control.Size = 0;
  EXPECT_NE(llvm::toString(validateRuntimeControlBlockV1(Control))
                .find("runtime ABI size"),
            std::string::npos);
  Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::ValidateBeforeDispatch, 17, 9);
  Control.Version = 2;
  EXPECT_NE(llvm::toString(validateRuntimeControlBlockV1(Control))
                .find("runtime ABI version"),
            std::string::npos);
  Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::ValidateBeforeDispatch, 17, 9);
  Control.CodeInvalidation = 0x101;
  EXPECT_NE(llvm::toString(validateRuntimeControlBlockV1(Control))
                .find("code-invalidation policy"),
            std::string::npos);

  const llvm::ArrayRef<RuntimeABIHelperSignatureV1> Signatures =
      runtimeABIHelperSignaturesV1();
  EXPECT_EQ(Signatures.size(), 8u);
  const llvm::ArrayRef<TranslationIRMemorySlot> Slots =
      runtimeABIMemorySlotsV1();
  ASSERT_EQ(Slots.size(), 1u);
  EXPECT_EQ(Slots[0].Region, TranslationIRMemoryRegion::Runtime);
  EXPECT_EQ(Slots[0].Offset, offsetof(RuntimeControlBlockV1, ScalarResult));
  EXPECT_EQ(Slots[0].Size, sizeof(uint64_t));
  EXPECT_EQ(Slots[0].Access, TranslationIRMemoryAccess::Read);
  const RuntimeABIHelperSignatureV1 *Load64 =
      findRuntimeABIHelperSignatureV1("nvd_rt_v1_load64_le");
  ASSERT_NE(Load64, nullptr);
  EXPECT_EQ(Load64->Result, RuntimeABIValueKind::I32);
  ASSERT_EQ(Load64->Parameters.size(), 3u);
  EXPECT_EQ(Load64->Parameters[0], RuntimeABIValueKind::RuntimePointer);
  EXPECT_EQ(Load64->Parameters[1], RuntimeABIValueKind::I64);
  EXPECT_EQ(Load64->Parameters[2], RuntimeABIValueKind::I32);
  EXPECT_EQ(findRuntimeABIHelperSignatureV1("nvd_rt_v1_load64"), nullptr);
  EXPECT_EQ(findRuntimeABIHelperSignatureV1("nvd_rt_v1_load64_le_suffix"),
            nullptr);
  EXPECT_EQ(findRuntimeABIHelperSignatureV1("nvd_rt_v1_dispatch"), nullptr);

  llvm::LLVMContext Context;
  const std::vector<TranslationRuntimeHelper> VerifierPolicy =
      createRuntimeABIHelperPolicyV1(Context);
  ASSERT_EQ(VerifierPolicy.size(), Signatures.size());
  ASSERT_NE(VerifierPolicy[3].Type, nullptr);
  EXPECT_TRUE(VerifierPolicy[3].Type->getReturnType()->isIntegerTy(32));
  EXPECT_EQ(VerifierPolicy[3].Parameters[0],
            TranslationRuntimeParameterKind::RuntimePointer);
}

TEST(RuntimeABI, OrdinaryMemoryFaultDetailsHaveAStableEncoding) {
  RuntimeMemoryFaultDetailsV1 Details;
  Details.Access = RuntimeMemoryAccessKindV1::Read;
  Details.RequiredAlignment = 8;

  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> EncodingOrErr =
      packRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1::Unmapped,
                                      Details);
  ASSERT_TRUE(static_cast<bool>(EncodingOrErr))
      << llvm::toString(EncodingOrErr.takeError());
  EXPECT_EQ(EncodingOrErr->Detail0, (uint64_t{8} << 32) | uint64_t{1});
  EXPECT_EQ(EncodingOrErr->Detail1, 0u);

  llvm::Expected<RuntimeMemoryFaultDetailsV1> DecodedOrErr =
      unpackRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1::Unmapped,
                                        *EncodingOrErr);
  ASSERT_TRUE(static_cast<bool>(DecodedOrErr))
      << llvm::toString(DecodedOrErr.takeError());
  EXPECT_EQ(DecodedOrErr->Access, RuntimeMemoryAccessKindV1::Read);
  EXPECT_EQ(DecodedOrErr->RequiredAlignment, 8u);
  EXPECT_EQ(DecodedOrErr->ExpectedGeneration, 0u);
  EXPECT_EQ(DecodedOrErr->ObservedGeneration, 0u);
}

TEST(RuntimeABI, AlignmentFaultDetailsRetainTheRejectedContract) {
  RuntimeMemoryFaultDetailsV1 Misaligned;
  Misaligned.Access = RuntimeMemoryAccessKindV1::Write;
  Misaligned.RequiredAlignment = 8;
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> MisalignedEncoding =
      packRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1::Misaligned,
                                      Misaligned);
  ASSERT_TRUE(static_cast<bool>(MisalignedEncoding))
      << llvm::toString(MisalignedEncoding.takeError());
  EXPECT_EQ(MisalignedEncoding->Detail0, (uint64_t{8} << 32) | uint64_t{2});
  llvm::Expected<RuntimeMemoryFaultDetailsV1> MisalignedDecoded =
      unpackRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1::Misaligned,
                                        *MisalignedEncoding);
  ASSERT_TRUE(static_cast<bool>(MisalignedDecoded))
      << llvm::toString(MisalignedDecoded.takeError());
  EXPECT_EQ(MisalignedDecoded->Access, RuntimeMemoryAccessKindV1::Write);
  EXPECT_EQ(MisalignedDecoded->RequiredAlignment, 8u);

  RuntimeMemoryFaultDetailsV1 InvalidAlignment;
  InvalidAlignment.Access = RuntimeMemoryAccessKindV1::Read;
  InvalidAlignment.RequiredAlignment = 3;
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> InvalidEncoding =
      packRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::InvalidAlignment, InvalidAlignment);
  ASSERT_TRUE(static_cast<bool>(InvalidEncoding))
      << llvm::toString(InvalidEncoding.takeError());
  EXPECT_EQ(InvalidEncoding->Detail0, (uint64_t{3} << 32) | uint64_t{1});
  llvm::Expected<RuntimeMemoryFaultDetailsV1> InvalidDecoded =
      unpackRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::InvalidAlignment, *InvalidEncoding);
  ASSERT_TRUE(static_cast<bool>(InvalidDecoded))
      << llvm::toString(InvalidDecoded.takeError());
  EXPECT_EQ(InvalidDecoded->RequiredAlignment, 3u);

  Misaligned.RequiredAlignment = 3;
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> BadMisaligned =
      packRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1::Misaligned,
                                      Misaligned);
  ASSERT_FALSE(static_cast<bool>(BadMisaligned));
  EXPECT_NE(llvm::toString(BadMisaligned.takeError()).find("alignment"),
            std::string::npos);

  InvalidAlignment.RequiredAlignment = 4;
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> BadInvalid =
      packRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::InvalidAlignment, InvalidAlignment);
  ASSERT_FALSE(static_cast<bool>(BadInvalid));
  EXPECT_NE(llvm::toString(BadInvalid.takeError()).find("fault kind"),
            std::string::npos);
}

TEST(RuntimeABI, GenerationFaultDetailsRemainLossless) {
  RuntimeMemoryFaultDetailsV1 Mismatch;
  Mismatch.Access = RuntimeMemoryAccessKindV1::Execute;
  Mismatch.ExpectedGeneration = 0x0123456789abcdefull;
  Mismatch.ObservedGeneration = 0xfedcba9876543210ull;
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> MismatchEncoding =
      packRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch, Mismatch);
  ASSERT_TRUE(static_cast<bool>(MismatchEncoding))
      << llvm::toString(MismatchEncoding.takeError());
  EXPECT_EQ(MismatchEncoding->Detail0, Mismatch.ExpectedGeneration);
  EXPECT_EQ(MismatchEncoding->Detail1, Mismatch.ObservedGeneration);
  llvm::Expected<RuntimeMemoryFaultDetailsV1> MismatchDecoded =
      unpackRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch,
          *MismatchEncoding);
  ASSERT_TRUE(static_cast<bool>(MismatchDecoded))
      << llvm::toString(MismatchDecoded.takeError());
  EXPECT_EQ(MismatchDecoded->Access, RuntimeMemoryAccessKindV1::Execute);
  EXPECT_EQ(MismatchDecoded->RequiredAlignment, 0u);
  EXPECT_EQ(MismatchDecoded->ExpectedGeneration, Mismatch.ExpectedGeneration);
  EXPECT_EQ(MismatchDecoded->ObservedGeneration, Mismatch.ObservedGeneration);

  RuntimeMemoryFaultDetailsV1 Overflow;
  Overflow.Access = RuntimeMemoryAccessKindV1::Write;
  Overflow.RequiredAlignment = 8;
  Overflow.ObservedGeneration = std::numeric_limits<uint64_t>::max();
  llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> OverflowEncoding =
      packRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow, Overflow);
  ASSERT_TRUE(static_cast<bool>(OverflowEncoding))
      << llvm::toString(OverflowEncoding.takeError());
  EXPECT_EQ(OverflowEncoding->Detail0, (uint64_t{8} << 32) | uint64_t{2});
  EXPECT_EQ(OverflowEncoding->Detail1, std::numeric_limits<uint64_t>::max());
  llvm::Expected<RuntimeMemoryFaultDetailsV1> OverflowDecoded =
      unpackRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow,
          *OverflowEncoding);
  ASSERT_TRUE(static_cast<bool>(OverflowDecoded))
      << llvm::toString(OverflowDecoded.takeError());
  EXPECT_EQ(OverflowDecoded->Access, RuntimeMemoryAccessKindV1::Write);
  EXPECT_EQ(OverflowDecoded->RequiredAlignment, 8u);
  EXPECT_EQ(OverflowDecoded->ExpectedGeneration, 0u);
  EXPECT_EQ(OverflowDecoded->ObservedGeneration,
            std::numeric_limits<uint64_t>::max());

  Mismatch.Access = RuntimeMemoryAccessKindV1::Read;
  expectInvalidFaultDetails(
      RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch, Mismatch);
  Mismatch.Access = RuntimeMemoryAccessKindV1::Execute;
  Mismatch.RequiredAlignment = 2;
  expectInvalidFaultDetails(
      RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch, Mismatch);
  Mismatch.RequiredAlignment = 0;
  Mismatch.ObservedGeneration = Mismatch.ExpectedGeneration;
  expectInvalidFaultDetails(
      RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch, Mismatch);

  Overflow.Access = RuntimeMemoryAccessKindV1::Read;
  expectInvalidFaultDetails(
      RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow, Overflow);
}

TEST(RuntimeABI, GeneratedIRCannotCallTheHostDispatcher) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseRuntimeModule(Context, R"(
declare i32 @nvd_rt_v1_dispatch(ptr, ptr, i64) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %status = call i32 @nvd_rt_v1_dispatch(
      ptr %state, ptr %runtime, i64 0) nounwind
  ret i32 %status
}
)");
  ASSERT_NE(Module, nullptr);
  const std::vector<TranslationRuntimeHelper> Helpers =
      createRuntimeABIHelperPolicyV1(Context);
  expectRuntimeIRViolation(
      verifyTranslationIR(
          *Module, llvm::Triple(HostTriple), llvm::DataLayout(HostLayout), 1,
          kRuntimeControlBlockSizeV1, runtimeABIMemorySlotsV1(), Helpers),
      TranslationIRViolation::ExternalSymbolNotAllowed);
}

TEST(RuntimeABI, RejectsIncoherentEmptyAndCancelledExits) {
  RuntimeControlBlockV1 Control =
      makeRuntimeControlBlockV1(CodeInvalidationPolicy::ValidateBeforeDispatch);
  expectInvalidControl(Control, "generation validation");

  Control.Flags = static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);
  expectValidControl(Control);

  Control.ScalarResult = 1;
  expectInvalidControl(Control, "stale scalar");

  Control.ScalarResult = 0;
  Control.Exit.Detail0 = 1;
  expectInvalidControl(Control, "payload");

  Control.Exit.Detail0 = 0;
  Control.Flags |= 1u << 31;
  expectInvalidControl(Control, "unknown flag");

  Control.Flags = static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);

  Control.CancellationRequested = 1;
  expectInvalidControl(Control, "cancellation");

  Control =
      makeRuntimeControlBlockV1(CodeInvalidationPolicy::ValidateBeforeDispatch);
  Control.Flags = static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);
  Control.ExpectedGeneration = 7;
  Control.ObservedGeneration = 8;
  expectInvalidControl(Control, "generation");

  Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  Control.ExpectedGeneration = 7;
  Control.ObservedGeneration = 7;
  expectInvalidControl(Control, "generation");

  Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite, 3, 0);
  Control.InstructionCount = 3;
  expectInvalidControl(Control, "budget");

  Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  Control.Exit.Kind = RuntimeABIExitKindV1::Cancelled;
  Control.CancellationRequested = 1;
  expectValidControl(Control);

  Control.Flags = static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);
  expectInvalidControl(Control, "flags");

  Control.Flags = 0;

  Control.CancellationRequested = 0;
  expectInvalidControl(Control, "cancelled");

  Control.CancellationRequested = 1;
  Control.Exit.Address = 1;
  expectInvalidControl(Control, "cancelled");

  Control.Exit.Address = 0;
  Control.ExpectedGeneration = 1;
  expectInvalidControl(Control, "generation");
}

TEST(RuntimeABI, RejectsIncoherentBudgetAndSelfModificationExits) {
  RuntimeControlBlockV1 Budget = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite, 3, 2);
  Budget.InstructionCount = 3;
  Budget.BlockCount = 1;
  Budget.Exit.Kind = RuntimeABIExitKindV1::BudgetExhausted;
  Budget.Exit.Size =
      static_cast<uint64_t>(TranslationBudgetKind::GuestInstructions);
  Budget.Exit.Detail0 = 3;
  Budget.Exit.Detail1 = 3;
  expectValidControl(Budget);

  Budget.Exit.Address = 1;
  expectInvalidControl(Budget, "budget");

  Budget.Exit.Address = 0;
  Budget.Exit.Detail0 = 2;
  expectInvalidControl(Budget, "budget");

  Budget.Exit.Detail0 = 3;
  Budget.Exit.Detail1 = 4;
  expectInvalidControl(Budget, "budget");

  Budget.Exit.Detail1 = 3;
  Budget.ScalarResult = 1;
  expectInvalidControl(Budget, "scalar");

  Budget.ScalarResult = 0;
  Budget.ExpectedGeneration = 1;
  expectInvalidControl(Budget, "generation");

  Budget.ExpectedGeneration = 0;
  Budget.Exit.Size =
      static_cast<uint64_t>(TranslationBudgetKind::GeneratedCodeBytes);
  expectInvalidControl(Budget, "budget kind");

  RuntimeControlBlockV1 BlockBudget = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite, 3, 2);
  BlockBudget.InstructionCount = 2;
  BlockBudget.BlockCount = 2;
  BlockBudget.Exit.Kind = RuntimeABIExitKindV1::BudgetExhausted;
  BlockBudget.Exit.Size = static_cast<uint64_t>(TranslationBudgetKind::Blocks);
  BlockBudget.Exit.Detail0 = 2;
  BlockBudget.Exit.Detail1 = 2;
  expectValidControl(BlockBudget);

  BlockBudget.InstructionCount = 3;
  expectInvalidControl(BlockBudget, "block-budget");

  RuntimeControlBlockV1 SelfModification = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  SelfModification.ObservedGeneration = 8;
  SelfModification.Exit.Kind = RuntimeABIExitKindV1::SelfModification;
  SelfModification.Exit.Address = 0;
  SelfModification.Exit.Size = 4;
  SelfModification.Exit.Detail0 = 7;
  SelfModification.Exit.Detail1 = 8;
  expectValidControl(SelfModification);

  SelfModification.Exit.Detail1 = 9;
  expectInvalidControl(SelfModification, "self-modification");

  SelfModification.Exit.Detail1 = 8;
  SelfModification.ObservedGeneration = 7;
  expectInvalidControl(SelfModification, "generation");

  SelfModification.ObservedGeneration = 8;
  SelfModification.ExpectedGeneration = 1;
  expectInvalidControl(SelfModification, "generation");

  SelfModification.ExpectedGeneration = 0;
  SelfModification.Exit.Address = std::numeric_limits<uint64_t>::max();
  expectInvalidControl(SelfModification, "self-modification");

  SelfModification.Exit.Address = 0;
  SelfModification.Exit.Size = 3;
  expectInvalidControl(SelfModification, "self-modification");

  SelfModification.Exit.Size = 4;
  SelfModification.CodeInvalidation =
      static_cast<uint32_t>(CodeInvalidationPolicy::ValidateBeforeDispatch);
  expectInvalidControl(SelfModification, "policy");
}

TEST(RuntimeABI, RejectsIncoherentMemoryFaultGenerationMirrors) {
  RuntimeControlBlockV1 InvalidWidth = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  InvalidWidth.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  InvalidWidth.Exit.Fault = RuntimeMemoryFaultKindV1::InvalidAccessWidth;
  InvalidWidth.Exit.Address = 0;
  setMemoryFaultDetails(InvalidWidth, RuntimeMemoryAccessKindV1::Read);
  expectValidControl(InvalidWidth);

  InvalidWidth.Exit.Size = 1;
  expectInvalidControl(InvalidWidth, "invalid-access-width");

  InvalidWidth.Exit.Size = 0;
  InvalidWidth.Exit.Detail1 = 1;
  expectInvalidControl(InvalidWidth, "reserved");

  RuntimeControlBlockV1 Fault = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  Fault.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  Fault.Exit.Fault = RuntimeMemoryFaultKindV1::Unmapped;
  Fault.Exit.Address = 0;
  Fault.Exit.Size = 1;
  setMemoryFaultDetails(Fault, RuntimeMemoryAccessKindV1::Read);
  expectValidControl(Fault);

  Fault.Exit.Size = 3;
  expectInvalidControl(Fault, "memory-fault");

  Fault.Exit.Size = 1;
  Fault.Exit.Detail0 |= uint64_t{1} << 8;
  expectInvalidControl(Fault, "reserved");

  setMemoryFaultDetails(Fault, RuntimeMemoryAccessKindV1::Read);
  Fault.ScalarResult = 1;
  expectInvalidControl(Fault, "scalar");

  RuntimeControlBlockV1 Mismatch =
      makeRuntimeControlBlockV1(CodeInvalidationPolicy::ValidateBeforeDispatch);
  Mismatch.ExpectedGeneration = 7;
  Mismatch.ObservedGeneration = 8;
  Mismatch.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  Mismatch.Exit.Fault = RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch;
  Mismatch.Exit.Address = 0;
  Mismatch.Exit.Size = 1;
  setMemoryFaultDetails(Mismatch, RuntimeMemoryAccessKindV1::Execute, 0, 7, 8);
  expectValidControl(Mismatch);

  Mismatch.Exit.Detail1 = 7;
  Mismatch.ObservedGeneration = 7;
  expectInvalidControl(Mismatch, "equal generations");

  Mismatch.Exit.Detail1 = 8;
  Mismatch.ObservedGeneration = 9;
  expectInvalidControl(Mismatch, "generation mirror");

  RuntimeControlBlockV1 Overflow = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  Overflow.ObservedGeneration = std::numeric_limits<uint64_t>::max();
  Overflow.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  Overflow.Exit.Fault = RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow;
  Overflow.Exit.Address = 0;
  Overflow.Exit.Size = 8;
  setMemoryFaultDetails(Overflow, RuntimeMemoryAccessKindV1::Write, 0, 0,
                        std::numeric_limits<uint64_t>::max());
  expectValidControl(Overflow);

  --Overflow.Exit.Detail1;
  --Overflow.ObservedGeneration;
  expectInvalidControl(Overflow, "incoherent generations");

  Overflow.Exit.Detail1 = std::numeric_limits<uint64_t>::max();
  Overflow.ObservedGeneration = std::numeric_limits<uint64_t>::max();
  Overflow.CodeInvalidation =
      static_cast<uint32_t>(CodeInvalidationPolicy::RejectExecutableWrites);
  expectInvalidControl(Overflow, "generation overflow");

  RuntimeControlBlockV1 Rejected =
      makeRuntimeControlBlockV1(CodeInvalidationPolicy::RejectExecutableWrites);
  Rejected.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  Rejected.Exit.Fault = RuntimeMemoryFaultKindV1::ExecutableWriteRejected;
  Rejected.Exit.Size = 1;
  setMemoryFaultDetails(Rejected, RuntimeMemoryAccessKindV1::Write);
  expectValidControl(Rejected);

  Rejected.CodeInvalidation = static_cast<uint32_t>(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  expectInvalidControl(Rejected, "policy");

  RuntimeControlBlockV1 PolicyViolation = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  PolicyViolation.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  PolicyViolation.Exit.Fault = RuntimeMemoryFaultKindV1::PolicyViolation;
  PolicyViolation.Exit.Size = 1;
  setMemoryFaultDetails(PolicyViolation, RuntimeMemoryAccessKindV1::Execute);
  expectValidControl(PolicyViolation);

  PolicyViolation.CodeInvalidation =
      static_cast<uint32_t>(CodeInvalidationPolicy::ValidateBeforeDispatch);
  expectInvalidControl(PolicyViolation, "policy");
}

TEST(RuntimeABI, ValidatesPackedMemoryFaultAccessAndReservedBits) {
  RuntimeControlBlockV1 Fault = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  Fault.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  Fault.Exit.Fault = RuntimeMemoryFaultKindV1::Unmapped;
  Fault.Exit.Address = 0x1234;
  Fault.Exit.Size = 4;
  RuntimeMemoryFaultDetailsV1 Details;
  Details.Access = RuntimeMemoryAccessKindV1::Read;
  Details.RequiredAlignment = 4;
  const RuntimeMemoryFaultDetailEncodingV1 Encoding = llvm::cantFail(
      packRuntimeMemoryFaultDetailsV1(Fault.Exit.Fault, Details));
  Fault.Exit.Detail0 = Encoding.Detail0;
  Fault.Exit.Detail1 = Encoding.Detail1;
  expectValidControl(Fault);

  RuntimeControlBlockV1 ExecuteRange = Fault;
  ExecuteRange.Exit.Size = 17;
  setMemoryFaultDetails(ExecuteRange, RuntimeMemoryAccessKindV1::Execute);
  expectValidControl(ExecuteRange);

  RuntimeControlBlockV1 NonScalarRead = Fault;
  NonScalarRead.Exit.Size = 3;
  expectInvalidControl(NonScalarRead, "access size");

  RuntimeControlBlockV1 EmptyExecute = Fault;
  EmptyExecute.Exit.Fault = RuntimeMemoryFaultKindV1::InvalidAccessWidth;
  EmptyExecute.Exit.Size = 0;
  setMemoryFaultDetails(EmptyExecute, RuntimeMemoryAccessKindV1::Execute);
  expectValidControl(EmptyExecute);

  RuntimeControlBlockV1 UnknownAccess = Fault;
  UnknownAccess.Exit.Detail0 =
      (UnknownAccess.Exit.Detail0 & ~uint64_t{0xff}) | uint64_t{0x7f};
  expectInvalidControl(UnknownAccess, "access kind");

  RuntimeControlBlockV1 Reserved = Fault;
  Reserved.Exit.Detail0 |= uint64_t{1} << 8;
  expectInvalidControl(Reserved, "reserved");

  Reserved = Fault;
  Reserved.Exit.Detail1 = 1;
  expectInvalidControl(Reserved, "reserved");

  RuntimeControlBlockV1 InvalidAlignment = Fault;
  InvalidAlignment.Exit.Detail0 = (uint64_t{3} << 32) | uint64_t{1};
  expectInvalidControl(InvalidAlignment, "alignment");

  RuntimeControlBlockV1 CrossRegion = Fault;
  CrossRegion.Exit.Fault = RuntimeMemoryFaultKindV1::CrossRegion;
  CrossRegion.Exit.Size = 1;
  CrossRegion.Exit.Detail0 =
      llvm::cantFail(packRuntimeMemoryFaultDetailsV1(
                         RuntimeMemoryFaultKindV1::Unmapped,
                         {RuntimeMemoryAccessKindV1::Execute, 0, 0, 0}))
          .Detail0;
  expectInvalidControl(CrossRegion, "cross-region");

  RuntimeControlBlockV1 Rejected =
      makeRuntimeControlBlockV1(CodeInvalidationPolicy::RejectExecutableWrites);
  Rejected.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
  Rejected.Exit.Fault = RuntimeMemoryFaultKindV1::ExecutableWriteRejected;
  Rejected.Exit.Size = 1;
  Rejected.Exit.Detail0 =
      llvm::cantFail(packRuntimeMemoryFaultDetailsV1(
                         RuntimeMemoryFaultKindV1::Unmapped,
                         {RuntimeMemoryAccessKindV1::Read, 0, 0, 0}))
          .Detail0;
  expectInvalidControl(Rejected, "executable-write-rejected");

  RuntimeControlBlockV1 Aligned = Fault;
  Aligned.Exit.Fault = RuntimeMemoryFaultKindV1::Misaligned;
  Aligned.Exit.Address = 0x1000;
  Aligned.Exit.Size = 1;
  const RuntimeMemoryFaultDetailEncodingV1 MisalignedEncoding =
      llvm::cantFail(packRuntimeMemoryFaultDetailsV1(
          RuntimeMemoryFaultKindV1::Misaligned,
          {RuntimeMemoryAccessKindV1::Read, 4, 0, 0}));
  Aligned.Exit.Detail0 = MisalignedEncoding.Detail0;
  Aligned.Exit.Detail1 = MisalignedEncoding.Detail1;
  expectInvalidControl(Aligned, "aligned address");
}

TEST(GuestMemoryRuntime, OwnsABoundedIndexAndHandlesVirtualAddressZero) {
  GuestState State = stateWithMemory({
      {0,
       MemoryPermission::Read | MemoryPermission::Write,
       3,
       {0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x90}},
  });

  GuestMemoryRuntimeConfig TooFewRegions;
  TooFewRegions.Limits.MaxRegions = 0;
  TooFewRegions.Limits.MaxBytes = 7;
  llvm::Expected<std::unique_ptr<GuestMemoryRuntime>> Limited =
      GuestMemoryRuntime::create(State, TooFewRegions);
  ASSERT_FALSE(static_cast<bool>(Limited));
  EXPECT_NE(llvm::toString(Limited.takeError()).find("guest-memory byte limit"),
            std::string::npos);

  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);
  State.Memory[0].Bytes.assign(8, 0);

  GuestMemoryAccessResult Load32 =
      Runtime->loadScalar(0, GuestScalarWidth::I32);
  ASSERT_EQ(Load32.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Load32.Value, 0x12345678u);

  GuestMemoryAccessResult Load64 =
      Runtime->loadScalar(0, GuestScalarWidth::I64);
  ASSERT_EQ(Load64.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Load64.Value, 0x90abcdef12345678ULL);

  GuestMemoryAccessResult Store16 =
      Runtime->storeScalar(1, GuestScalarWidth::I16, 0xa1b2);
  ASSERT_EQ(Store16.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Runtime->loadScalar(1, GuestScalarWidth::I8).Value, 0xb2u);
  EXPECT_EQ(Runtime->loadScalar(2, GuestScalarWidth::I8).Value, 0xa1u);
  EXPECT_EQ(Runtime->regionCount(), 1u);
  EXPECT_EQ(Runtime->guestMemoryBytes(), 8u);
}

TEST(GuestMemoryRuntime,
     FetchesInstructionBytesWithExactCrossRegionGenerationBindings) {
  const GuestState State = stateWithMemory({
      {0x1000, MemoryPermission::Execute, 11, {0xaa, 0xbb}},
      {0x1002, MemoryPermission::Execute, 22, {0xc0, 0xc1, 0xc2}},
      {0x1005, MemoryPermission::Execute, 33, {0xd0, 0xd1, 0xd2, 0xd3, 0xd4}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);
  std::array<uint8_t, 9> Bytes = {};

  const GuestInstructionFetchResult Fetch =
      Runtime->fetchInstructionBytes(0x1001, Bytes);

  EXPECT_EQ(Fetch.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_FALSE(Fetch.Fault.has_value());
  EXPECT_EQ(Bytes, (std::array<uint8_t, 9>{0xbb, 0xc0, 0xc1, 0xc2, 0xd0, 0xd1,
                                           0xd2, 0xd3, 0xd4}));
  ASSERT_EQ(Fetch.Bindings.size(), 3u);
  EXPECT_EQ(Fetch.Bindings[0].Address, 0x1001u);
  EXPECT_EQ(Fetch.Bindings[0].Size, 1u);
  EXPECT_EQ(Fetch.Bindings[0].Generation, 11u);
  EXPECT_EQ(Fetch.Bindings[1].Address, 0x1002u);
  EXPECT_EQ(Fetch.Bindings[1].Size, 3u);
  EXPECT_EQ(Fetch.Bindings[1].Generation, 22u);
  EXPECT_EQ(Fetch.Bindings[2].Address, 0x1005u);
  EXPECT_EQ(Fetch.Bindings[2].Size, 5u);
  EXPECT_EQ(Fetch.Bindings[2].Generation, 33u);
}

TEST(GuestMemoryRuntime, InstructionFetchFaultsAreTypedAndTransactional) {
  const GuestState State = stateWithMemory({
      {0x1000, MemoryPermission::Execute, 1, {0x11}},
      {0x1002, MemoryPermission::Execute, 2, {0x22, 0x23}},
      {0x1004, MemoryPermission::Read, 3, {0x33, 0x34, 0x35}},
      {std::numeric_limits<uint64_t>::max(),
       MemoryPermission::Execute,
       4,
       {0xfe}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);

  expectFetchFault(Runtime->fetchInstructionBytes(0x1000, {}),
                   RuntimeMemoryFaultKindV1::InvalidAccessWidth, 0x1000, 0);
  const RuntimeControlBlockV1 Empty = Runtime->snapshotControlBlock();
  EXPECT_EQ(Empty.Exit.Size, 0u);
  expectSnapshotFaultDetails(Empty,
                             RuntimeMemoryFaultKindV1::InvalidAccessWidth,
                             RuntimeMemoryAccessKindV1::Execute, 0);

  std::array<uint8_t, 1> LastByte = {};
  const GuestInstructionFetchResult Boundary = Runtime->fetchInstructionBytes(
      std::numeric_limits<uint64_t>::max(), LastByte);
  ASSERT_EQ(Boundary.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(LastByte[0], 0xfeu);
  ASSERT_EQ(Boundary.Bindings.size(), 1u);
  EXPECT_EQ(Boundary.Bindings[0].Address, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(Boundary.Bindings[0].Size, 1u);
  EXPECT_EQ(Boundary.Bindings[0].Generation, 4u);

  std::array<uint8_t, 3> OverflowBytes = {0xa1, 0xa2, 0xa3};
  expectFetchFault(Runtime->fetchInstructionBytes(
                       std::numeric_limits<uint64_t>::max() - 1, OverflowBytes),
                   RuntimeMemoryFaultKindV1::AddressOverflow,
                   std::numeric_limits<uint64_t>::max() - 1, 3);
  EXPECT_EQ(OverflowBytes, (std::array<uint8_t, 3>{0xa1, 0xa2, 0xa3}));
  const RuntimeControlBlockV1 Overflow = Runtime->snapshotControlBlock();
  EXPECT_EQ(Overflow.Exit.Size, 3u);
  expectSnapshotFaultDetails(Overflow,
                             RuntimeMemoryFaultKindV1::AddressOverflow,
                             RuntimeMemoryAccessKindV1::Execute, 0);

  std::array<uint8_t, 3> GapBytes = {0xb1, 0xb2, 0xb3};
  expectFetchFault(Runtime->fetchInstructionBytes(0x1000, GapBytes),
                   RuntimeMemoryFaultKindV1::Unmapped, 0x1000, 3);
  EXPECT_EQ(GapBytes, (std::array<uint8_t, 3>{0xb1, 0xb2, 0xb3}));
  const RuntimeControlBlockV1 Gap = Runtime->snapshotControlBlock();
  EXPECT_EQ(Gap.Exit.Size, 3u);
  expectSnapshotFaultDetails(Gap, RuntimeMemoryFaultKindV1::Unmapped,
                             RuntimeMemoryAccessKindV1::Execute, 0);

  std::array<uint8_t, 5> DeniedBytes = {0xc1, 0xc2, 0xc3, 0xc4, 0xc5};
  expectFetchFault(Runtime->fetchInstructionBytes(0x1002, DeniedBytes),
                   RuntimeMemoryFaultKindV1::PermissionDenied, 0x1002, 5);
  EXPECT_EQ(DeniedBytes,
            (std::array<uint8_t, 5>{0xc1, 0xc2, 0xc3, 0xc4, 0xc5}));
  const RuntimeControlBlockV1 Denied = Runtime->snapshotControlBlock();
  EXPECT_EQ(Denied.Exit.Size, 5u);
  expectSnapshotFaultDetails(Denied, RuntimeMemoryFaultKindV1::PermissionDenied,
                             RuntimeMemoryAccessKindV1::Execute, 0);
}

TEST(GuestMemoryRuntime, FaultsRemainPreciseAcrossRegionBoundaries) {
  GuestState State = stateWithMemory({
      {0x1000, MemoryPermission::Read, 0, {0x01, 0x02}},
      {0x1002, MemoryPermission::Read, 0, {0x03, 0x04}},
      {0x2000, MemoryPermission::Write, 0, {0, 0, 0, 0}},
      {std::numeric_limits<uint64_t>::max(), MemoryPermission::Read, 0, {0xaa}},
  });

  GuestMemoryRuntimeConfig RegionLimited;
  RegionLimited.Limits.MaxRegions = 3;
  llvm::Expected<std::unique_ptr<GuestMemoryRuntime>> TooManyRegions =
      GuestMemoryRuntime::create(State, RegionLimited);
  ASSERT_FALSE(static_cast<bool>(TooManyRegions));
  EXPECT_NE(llvm::toString(TooManyRegions.takeError())
                .find("guest-memory region limit"),
            std::string::npos);

  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);

  GuestMemoryAccessResult Overflow = Runtime->loadScalar(
      std::numeric_limits<uint64_t>::max(), GuestScalarWidth::I16);
  expectFault(Overflow, RuntimeMemoryFaultKindV1::AddressOverflow);
  EXPECT_EQ(Overflow.Fault->Exit.AccessWidthBits, 16u);

  GuestMemoryAccessResult CrossRegionLoad =
      Runtime->loadScalar(0x1000, GuestScalarWidth::I32);
  ASSERT_EQ(CrossRegionLoad.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(CrossRegionLoad.Value, 0x04030201u);

  GuestMemoryAccessResult Unmapped =
      Runtime->loadScalar(0x1800, GuestScalarWidth::I8);
  expectFault(Unmapped, RuntimeMemoryFaultKindV1::Unmapped);

  GuestMemoryAccessResult Misaligned =
      Runtime->loadScalar(0x1001, GuestScalarWidth::I8, 4);
  expectFault(Misaligned, RuntimeMemoryFaultKindV1::Misaligned);
  EXPECT_EQ(Misaligned.Fault->Exit.RequiredAlignment, 4u);

  GuestMemoryAccessResult InvalidAlignment =
      Runtime->loadScalar(0x1000, GuestScalarWidth::I8, 3);
  expectFault(InvalidAlignment, RuntimeMemoryFaultKindV1::InvalidAlignment);

  GuestMemoryAccessResult ReadDenied =
      Runtime->loadScalar(0x2000, GuestScalarWidth::I8);
  expectFault(ReadDenied, RuntimeMemoryFaultKindV1::PermissionDenied);

  GuestMemoryAccessResult WriteDenied =
      Runtime->storeScalar(0x1000, GuestScalarWidth::I8, 0xff);
  expectFault(WriteDenied, RuntimeMemoryFaultKindV1::PermissionDenied);
  expectValidControl(Runtime->snapshotControlBlock());
}

TEST(GuestMemoryRuntime, ScalarLoadsAndStoresTraverseAdjacentDataRegions) {
  const GuestState State = stateWithMemory({
      {0x2000, MemoryPermission::Read | MemoryPermission::Write, 7, {0x78}},
      {0x2001,
       MemoryPermission::Read | MemoryPermission::Write,
       8,
       {0x56, 0x34}},
      {0x2003, MemoryPermission::Read | MemoryPermission::Write, 9, {0x12}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);

  GuestMemoryAccessResult Load =
      Runtime->loadScalar(0x2000, GuestScalarWidth::I32, 4);
  ASSERT_EQ(Load.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Load.Value, 0x12345678u);

  GuestMemoryAccessResult Store =
      Runtime->storeScalar(0x2000, GuestScalarWidth::I32, 0xa1b2c3d4, 4);
  ASSERT_EQ(Store.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Runtime->loadScalar(0x2000, GuestScalarWidth::I8).Value, 0xd4u);
  EXPECT_EQ(Runtime->loadScalar(0x2001, GuestScalarWidth::I8).Value, 0xc3u);
  EXPECT_EQ(Runtime->loadScalar(0x2002, GuestScalarWidth::I8).Value, 0xb2u);
  EXPECT_EQ(Runtime->loadScalar(0x2003, GuestScalarWidth::I8).Value, 0xa1u);
  EXPECT_EQ(Runtime->generationForAddress(0x2000), 7u);
  EXPECT_EQ(Runtime->generationForAddress(0x2001), 8u);
  EXPECT_EQ(Runtime->generationForAddress(0x2003), 9u);
}

TEST(GuestMemoryRuntime, CrossRegionStoresPreflightBeforeAnyByteChanges) {
  const GuestState State = stateWithMemory({
      {0x3000,
       MemoryPermission::Read | MemoryPermission::Write,
       0,
       {0x11, 0x22}},
      {0x3002, MemoryPermission::Read, 0, {0x33, 0x44}},
      {0x4000,
       MemoryPermission::Read | MemoryPermission::Write,
       0,
       {0x55, 0x66}},
      {0x4003, MemoryPermission::Read | MemoryPermission::Write, 0, {0x88}},
      {0x8000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       17,
       {0x99}},
      {0x8001, MemoryPermission::Read, 19, {0xaa}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);

  GuestMemoryAccessResult Denied =
      Runtime->storeScalar(0x3000, GuestScalarWidth::I32, 0xaabbccdd);
  expectFault(Denied, RuntimeMemoryFaultKindV1::PermissionDenied);
  EXPECT_EQ(Runtime->loadScalar(0x3000, GuestScalarWidth::I32).Value,
            0x44332211u);

  GuestMemoryAccessResult Gap =
      Runtime->storeScalar(0x4000, GuestScalarWidth::I32, 0xaabbccdd);
  expectFault(Gap, RuntimeMemoryFaultKindV1::CrossRegion);
  EXPECT_EQ(Runtime->loadScalar(0x4000, GuestScalarWidth::I16).Value, 0x6655u);
  EXPECT_EQ(Runtime->loadScalar(0x4003, GuestScalarWidth::I8).Value, 0x88u);

  GuestMemoryAccessResult ExecutablePrefix =
      Runtime->storeScalar(0x8000, GuestScalarWidth::I16, 0xccbb);
  expectFault(ExecutablePrefix, RuntimeMemoryFaultKindV1::PermissionDenied);
  EXPECT_EQ(Runtime->loadScalar(0x8000, GuestScalarWidth::I16).Value, 0xaa99u);
  EXPECT_EQ(Runtime->generationForAddress(0x8000), 17u);
  EXPECT_EQ(Runtime->generationForAddress(0x8001), 19u);
  expectValidControl(Runtime->snapshotControlBlock());
}

TEST(GuestMemoryRuntime,
     StoresAcrossMultipleExecutableOwnersFailClosedForEveryPolicy) {
  const GuestState State = stateWithMemory({
      {0x5000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       7,
       {0x11}},
      {0x5001,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       9,
       {0x22}},
  });

  for (CodeInvalidationPolicy Policy :
       {CodeInvalidationPolicy::RejectExecutableWrites,
        CodeInvalidationPolicy::InvalidateOnExecutableWrite,
        CodeInvalidationPolicy::ValidateBeforeDispatch}) {
    std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State, Policy);
    GuestMemoryAccessResult Store =
        Runtime->storeScalar(0x5000, GuestScalarWidth::I16, 0xbbaa);
    expectFault(Store, RuntimeMemoryFaultKindV1::CrossRegion);
    expectValidControl(Runtime->snapshotControlBlock());
    EXPECT_EQ(Runtime->loadScalar(0x5000, GuestScalarWidth::I16).Value,
              0x2211u);
    EXPECT_EQ(Runtime->generationForAddress(0x5000), 7u);
    EXPECT_EQ(Runtime->generationForAddress(0x5001), 9u);
  }
}

TEST(GuestMemoryRuntime,
     CrossRegionStoreWithOneExecutableOwnerPreservesInvalidationSemantics) {
  const GuestState State = stateWithMemory({
      {0x6000, MemoryPermission::Read | MemoryPermission::Write, 3, {0x11}},
      {0x6001,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       7,
       {0x22}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);

  GuestMemoryAccessResult Store =
      Runtime->storeScalar(0x6000, GuestScalarWidth::I16, 0xbbaa);
  ASSERT_EQ(Store.Status, GuestMemoryAccessStatus::SelfModification);
  ASSERT_TRUE(Store.SelfModification.has_value());
  EXPECT_EQ(Store.SelfModification->Address, 0x6000u);
  EXPECT_EQ(Store.SelfModification->Size, 2u);
  EXPECT_EQ(Store.SelfModification->OldGeneration, 7u);
  EXPECT_EQ(Store.SelfModification->NewGeneration, 8u);
  EXPECT_EQ(Runtime->loadScalar(0x6000, GuestScalarWidth::I16).Value, 0xbbaau);
  EXPECT_EQ(Runtime->generationForAddress(0x6000), 3u);
  EXPECT_EQ(Runtime->generationForAddress(0x6001), 8u);
}

TEST(GuestMemoryRuntime,
     FetchBindingsSupportExactPerRegionGenerationRevalidation) {
  const GuestState State = stateWithMemory({
      {0x7000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       5,
       {0x11, 0x22}},
      {0x7002,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       8,
       {0x33, 0x44}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      createRuntime(State, CodeInvalidationPolicy::ValidateBeforeDispatch);
  std::array<uint8_t, 4> Bytes = {};
  const GuestInstructionFetchResult Fetch =
      Runtime->fetchInstructionBytes(0x7000, Bytes);
  ASSERT_EQ(Fetch.Status, GuestMemoryAccessStatus::Completed);
  ASSERT_EQ(Fetch.Bindings.size(), 2u);

  GuestMemoryAccessResult Store =
      Runtime->storeScalar(0x7002, GuestScalarWidth::I8, 0xaa);
  ASSERT_EQ(Store.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Runtime
                ->validateExecutableGeneration(Fetch.Bindings[0].Address,
                                               Fetch.Bindings[0].Generation)
                .Status,
            GuestMemoryAccessStatus::Completed);
  GuestMemoryAccessResult Stale = Runtime->validateExecutableGeneration(
      Fetch.Bindings[1].Address, Fetch.Bindings[1].Generation);
  expectFault(Stale, RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch);
  EXPECT_EQ(Stale.Fault->ExpectedGeneration, 8u);
  EXPECT_EQ(Stale.Fault->ObservedGeneration, 9u);
}

TEST(GuestMemoryRuntime, SnapshotsRoundTripEveryMemoryFaultDetailDomain) {
  const GuestState State = stateWithMemory({
      {0x1000, MemoryPermission::Read, 0, {0, 0, 0, 0, 0, 0, 0, 0}},
      {0x2000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       7,
       {0, 0, 0, 0, 0, 0, 0, 0}},
      {0x3000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       std::numeric_limits<uint64_t>::max(),
       {0, 0, 0, 0, 0, 0, 0, 0}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      createRuntime(State, CodeInvalidationPolicy::ValidateBeforeDispatch);

  expectFault(Runtime->loadScalar(0x1800, GuestScalarWidth::I32, 4),
              RuntimeMemoryFaultKindV1::Unmapped);
  expectSnapshotFaultDetails(Runtime->snapshotControlBlock(),
                             RuntimeMemoryFaultKindV1::Unmapped,
                             RuntimeMemoryAccessKindV1::Read, 4);

  expectFault(Runtime->storeScalar(0x1000, GuestScalarWidth::I16, 0, 2),
              RuntimeMemoryFaultKindV1::PermissionDenied);
  expectSnapshotFaultDetails(Runtime->snapshotControlBlock(),
                             RuntimeMemoryFaultKindV1::PermissionDenied,
                             RuntimeMemoryAccessKindV1::Write, 2);

  expectFault(Runtime->validateExecutableGeneration(0x1000, 0),
              RuntimeMemoryFaultKindV1::PermissionDenied);
  expectSnapshotFaultDetails(Runtime->snapshotControlBlock(),
                             RuntimeMemoryFaultKindV1::PermissionDenied,
                             RuntimeMemoryAccessKindV1::Execute, 0);

  expectFault(Runtime->loadScalar(0x1001, GuestScalarWidth::I8, 4),
              RuntimeMemoryFaultKindV1::Misaligned);
  expectSnapshotFaultDetails(Runtime->snapshotControlBlock(),
                             RuntimeMemoryFaultKindV1::Misaligned,
                             RuntimeMemoryAccessKindV1::Read, 4);

  expectFault(Runtime->storeScalar(0x1000, GuestScalarWidth::I8, 0, 3),
              RuntimeMemoryFaultKindV1::InvalidAlignment);
  expectSnapshotFaultDetails(Runtime->snapshotControlBlock(),
                             RuntimeMemoryFaultKindV1::InvalidAlignment,
                             RuntimeMemoryAccessKindV1::Write, 3);

  expectFault(Runtime->validateExecutableGeneration(0x2000, 6),
              RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch);
  expectSnapshotFaultDetails(
      Runtime->snapshotControlBlock(),
      RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch,
      RuntimeMemoryAccessKindV1::Execute, 0, 6, 7);

  expectFault(Runtime->storeScalar(0x3000, GuestScalarWidth::I8, 0, 8),
              RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow);
  expectSnapshotFaultDetails(
      Runtime->snapshotControlBlock(),
      RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow,
      RuntimeMemoryAccessKindV1::Write, 8, 0,
      std::numeric_limits<uint64_t>::max());
}

TEST(GuestMemoryRuntime, ExecutableWritesHonorEveryInvalidationPolicy) {
  const GuestState State = stateWithMemory({
      {0x4000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       7,
       {0x11, 0x22, 0x33, 0x44}},
  });

  std::unique_ptr<GuestMemoryRuntime> Reject =
      createRuntime(State, CodeInvalidationPolicy::RejectExecutableWrites);
  GuestMemoryAccessResult Rejected =
      Reject->storeScalar(0x4000, GuestScalarWidth::I8, 0xaa);
  expectFault(Rejected, RuntimeMemoryFaultKindV1::ExecutableWriteRejected);
  expectValidControl(Reject->snapshotControlBlock());
  EXPECT_EQ(Reject->loadScalar(0x4000, GuestScalarWidth::I8).Value, 0x11u);
  EXPECT_EQ(Reject->generationForAddress(0x4000), 7u);

  std::unique_ptr<GuestMemoryRuntime> Invalidate =
      createRuntime(State, CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  GuestMemoryAccessResult First =
      Invalidate->storeScalar(0x4000, GuestScalarWidth::I16, 0xbbaa);
  ASSERT_EQ(First.Status, GuestMemoryAccessStatus::SelfModification);
  ASSERT_TRUE(First.SelfModification.has_value());
  EXPECT_EQ(First.SelfModification->OldGeneration, 7u);
  EXPECT_EQ(First.SelfModification->NewGeneration, 8u);
  EXPECT_EQ(Invalidate->generationForAddress(0x4000), 8u);
  GuestMemoryAccessResult Second =
      Invalidate->storeScalar(0x4002, GuestScalarWidth::I8, 0xcc);
  ASSERT_EQ(Second.Status, GuestMemoryAccessStatus::SelfModification);
  EXPECT_EQ(Second.SelfModification->OldGeneration, 8u);
  EXPECT_EQ(Second.SelfModification->NewGeneration, 9u);
  const RuntimeControlBlockV1 Invalidated =
      Invalidate->snapshotControlBlock(0x4000);
  EXPECT_EQ(Invalidated.ScalarResult, 0u);
  EXPECT_EQ(Invalidated.ExpectedGeneration, 0u);
  EXPECT_EQ(Invalidated.ObservedGeneration, 9u);
  expectValidControl(Invalidated);
  expectFault(Invalidate->validateExecutableGeneration(0x4000, 9),
              RuntimeMemoryFaultKindV1::PolicyViolation);
  expectValidControl(Invalidate->snapshotControlBlock());

  std::unique_ptr<GuestMemoryRuntime> Validate =
      createRuntime(State, CodeInvalidationPolicy::ValidateBeforeDispatch);
  GuestMemoryAccessResult Deferred =
      Validate->storeScalar(0x4000, GuestScalarWidth::I8, 0xaa);
  ASSERT_EQ(Deferred.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(Validate->generationForAddress(0x4000), 8u);
  expectInvalidControl(Validate->snapshotControlBlock(0x4000, 7), "generation");
  expectInvalidControl(Validate->snapshotControlBlock(0x4000, 8),
                       "generation validation");
  GuestMemoryAccessResult Stale =
      Validate->validateExecutableGeneration(0x4000, 7);
  expectFault(Stale, RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch);
  EXPECT_EQ(Stale.Fault->ExpectedGeneration, 7u);
  EXPECT_EQ(Stale.Fault->ObservedGeneration, 8u);
  EXPECT_EQ(Validate->validateExecutableGeneration(0x4000, 8).Status,
            GuestMemoryAccessStatus::Completed);
  const RuntimeControlBlockV1 Validated =
      Validate->snapshotControlBlock(0x4000, 8);
  EXPECT_EQ(Validated.Flags,
            static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated));
  expectValidControl(Validated);
  expectInvalidControl(Validate->snapshotControlBlock(0x4001, 8),
                       "generation validation");
}

TEST(GuestMemoryRuntime,
     MemorySnapshotsOwnTheAuthoritativeBytesAndGenerationState) {
  GuestState State = stateWithMemory({
      {0x1000,
       MemoryPermission::Read | MemoryPermission::Write,
       3,
       {0x10, 0x20, 0x30, 0x40}},
      {0x2000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       7,
       {0x50, 0x60, 0x70, 0x80}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      createRuntime(State, CodeInvalidationPolicy::ValidateBeforeDispatch);

  // The runtime owns its initial bytes rather than aliasing the logical state.
  State.Memory[0].Bytes[0] = 0xff;
  EXPECT_EQ(Runtime->loadScalar(0x1000, GuestScalarWidth::I8).Value, 0x10u);

  ASSERT_EQ(Runtime->storeScalar(0x1001, GuestScalarWidth::I16, 0xbbaa).Status,
            GuestMemoryAccessStatus::Completed);
  const std::vector<GuestMemoryRegion> BeforeFailure =
      Runtime->snapshotMemoryRegions();
  expectFault(Runtime->storeScalar(0x1003, GuestScalarWidth::I16, 0xddcc),
              RuntimeMemoryFaultKindV1::CrossRegion);
  EXPECT_EQ(Runtime->storeScalar(0x2000, GuestScalarWidth::I8, 0xee).Status,
            GuestMemoryAccessStatus::Completed);

  std::vector<GuestMemoryRegion> Snapshot = Runtime->snapshotMemoryRegions();
  ASSERT_EQ(Snapshot.size(), 2u);
  EXPECT_EQ(Snapshot[0].Address, 0x1000u);
  EXPECT_EQ(Snapshot[0].Permissions,
            MemoryPermission::Read | MemoryPermission::Write);
  EXPECT_EQ(Snapshot[0].Generation, 3u);
  EXPECT_EQ(Snapshot[0].Bytes, (std::vector<uint8_t>{0x10, 0xaa, 0xbb, 0x40}));
  EXPECT_EQ(Snapshot[1].Address, 0x2000u);
  EXPECT_EQ(Snapshot[1].Generation, 8u);
  EXPECT_EQ(Snapshot[1].Bytes, (std::vector<uint8_t>{0xee, 0x60, 0x70, 0x80}));

  ASSERT_EQ(BeforeFailure.size(), 2u);
  EXPECT_EQ(BeforeFailure[0].Bytes,
            (std::vector<uint8_t>{0x10, 0xaa, 0xbb, 0x40}));
  EXPECT_EQ(BeforeFailure[0].Generation, 3u);
  EXPECT_EQ(BeforeFailure[1].Generation, 7u);

  Snapshot[0].Bytes[0] = 0;
  Snapshot[1].Generation = 0;
  const std::vector<GuestMemoryRegion> Independent =
      Runtime->snapshotMemoryRegions();
  EXPECT_EQ(Independent[0].Bytes[0], 0x10u);
  EXPECT_EQ(Independent[1].Generation, 8u);
}

TEST(GuestMemoryRuntime, GenerationOverflowFailsClosedBeforeMutation) {
  const GuestState State = stateWithMemory({
      {0x5000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       std::numeric_limits<uint64_t>::max(),
       {0x11}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(State);

  GuestMemoryAccessResult Store =
      Runtime->storeScalar(0x5000, GuestScalarWidth::I8, 0xaa);
  expectFault(Store, RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow);
  expectValidControl(Runtime->snapshotControlBlock());
  EXPECT_EQ(Runtime->loadScalar(0x5000, GuestScalarWidth::I8).Value, 0x11u);
  EXPECT_EQ(Runtime->generationForAddress(0x5000),
            std::numeric_limits<uint64_t>::max());
}

TEST(GuestMemoryRuntime, EmptyAnd32BitBoundaryRegionsStayChecked) {
  const GuestState EmptyState = stateWithMemory({});
  std::unique_ptr<GuestMemoryRuntime> Empty = createRuntime(EmptyState);
  EXPECT_EQ(Empty->regionCount(), 0u);
  EXPECT_EQ(Empty->guestMemoryBytes(), 0u);
  expectFault(Empty->loadScalar(0, GuestScalarWidth::I8),
              RuntimeMemoryFaultKindV1::Unmapped);

  GuestState State32 =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_32));
  State32.Memory.push_back({std::numeric_limits<uint32_t>::max(),
                            MemoryPermission::Read,
                            0,
                            {0xaa}});
  std::unique_ptr<GuestMemoryRuntime> Runtime32 =
      llvm::cantFail(GuestMemoryRuntime::create(State32));
  GuestMemoryAccessResult LastByte = Runtime32->loadScalar(
      std::numeric_limits<uint32_t>::max(), GuestScalarWidth::I8);
  ASSERT_EQ(LastByte.Status, GuestMemoryAccessStatus::Completed);
  EXPECT_EQ(LastByte.Value, 0xaau);
  expectFault(Runtime32->loadScalar(std::numeric_limits<uint32_t>::max(),
                                    GuestScalarWidth::I16),
              RuntimeMemoryFaultKindV1::AddressOverflow);
  expectFault(Runtime32->loadScalar(uint64_t{1} << 32, GuestScalarWidth::I8),
              RuntimeMemoryFaultKindV1::AddressOverflow);
}

TEST(GuestMemoryRuntime, SnapshotsClearStaleResultFields) {
  const GuestState State = stateWithMemory({
      {0x4000,
       MemoryPermission::Read | MemoryPermission::Write |
           MemoryPermission::Execute,
       7,
       {0x11, 0x22, 0x33, 0x44}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(
      State, CodeInvalidationPolicy::ValidateBeforeDispatch, 5, 0);

  ASSERT_EQ(Runtime->loadScalar(0x4000, GuestScalarWidth::I32).Status,
            GuestMemoryAccessStatus::Completed);
  RuntimeControlBlockV1 Load = Runtime->snapshotControlBlock(0x4000, 0);
  EXPECT_EQ(Load.ScalarResult, 0x44332211u);
  EXPECT_EQ(Load.ExpectedGeneration, 0u);
  EXPECT_EQ(Load.ObservedGeneration, 0u);
  EXPECT_EQ(Load.Flags, 0u);
  expectInvalidControl(Load, "generation validation");

  GuestMemoryAccessResult Mismatch =
      Runtime->validateExecutableGeneration(0x4000, 6);
  expectFault(Mismatch, RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch);
  RuntimeControlBlockV1 Fault = Runtime->snapshotControlBlock(0x4000, 99);
  EXPECT_EQ(Fault.ScalarResult, 0u);
  EXPECT_EQ(Fault.ExpectedGeneration, 6u);
  EXPECT_EQ(Fault.ObservedGeneration, 7u);
  expectValidControl(Fault);

  ASSERT_EQ(Runtime->poll(1, 0).Status, RuntimePollStatus::Continue);
  RuntimeControlBlockV1 Continue = Runtime->snapshotControlBlock(0x4000, 0);
  EXPECT_EQ(Continue.ScalarResult, 0u);
  EXPECT_EQ(Continue.ExpectedGeneration, 0u);
  EXPECT_EQ(Continue.ObservedGeneration, 0u);
  EXPECT_EQ(Continue.Exit.Kind, RuntimeABIExitKindV1::None);
  EXPECT_EQ(Continue.Flags, 0u);
  expectInvalidControl(Continue, "generation validation");

  ASSERT_EQ(Runtime->validateExecutableGeneration(0x4000, 7).Status,
            GuestMemoryAccessStatus::Completed);
  RuntimeControlBlockV1 Validated = Runtime->snapshotControlBlock(0x4000, 7);
  EXPECT_EQ(Validated.Flags,
            static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated));
  expectValidControl(Validated);

  ASSERT_EQ(Runtime->poll().Status, RuntimePollStatus::Continue);
  RuntimeControlBlockV1 AfterPoll = Runtime->snapshotControlBlock(0x4000, 7);
  EXPECT_EQ(AfterPoll.Flags, 0u);
  expectInvalidControl(AfterPoll, "generation validation");

  ASSERT_EQ(Runtime->validateExecutableGeneration(0x4000, 7).Status,
            GuestMemoryAccessStatus::Completed);

  Runtime->requestCancellation();
  RuntimeControlBlockV1 Cancelled = Runtime->snapshotControlBlock(0x4000, 99);
  EXPECT_EQ(Cancelled.CancellationRequested, 1u);
  EXPECT_EQ(Cancelled.Exit.Kind, RuntimeABIExitKindV1::Cancelled);
  EXPECT_EQ(Cancelled.ScalarResult, 0u);
  EXPECT_EQ(Cancelled.ExpectedGeneration, 0u);
  EXPECT_EQ(Cancelled.ObservedGeneration, 0u);
  EXPECT_EQ(Cancelled.Flags, 0u);
  expectValidControl(Cancelled);
}

TEST(GuestMemoryRuntime, AtomicCountersSaturateWithoutWrapping) {
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      createRuntime(stateWithMemory({}));
  const uint64_t Maximum = std::numeric_limits<uint64_t>::max();

  RuntimePollResult First = Runtime->poll(Maximum - 1, Maximum - 2);
  EXPECT_EQ(First.Status, RuntimePollStatus::Continue);
  RuntimePollResult Saturated = Runtime->poll(10, 20);
  EXPECT_EQ(Saturated.Status, RuntimePollStatus::Continue);
  EXPECT_EQ(Saturated.InstructionCount, Maximum);
  EXPECT_EQ(Saturated.BlockCount, Maximum);
  RuntimePollResult Stable = Runtime->poll(1, 1);
  EXPECT_EQ(Stable.InstructionCount, Maximum);
  EXPECT_EQ(Stable.BlockCount, Maximum);
  expectValidControl(Runtime->snapshotControlBlock());
}

TEST(GuestMemoryRuntime, CancellationAndBudgetCountersAreAtomic) {
  const GuestState State = stateWithMemory({
      {0x1000, MemoryPermission::Read, 0, {0}},
  });
  std::unique_ptr<GuestMemoryRuntime> Runtime = createRuntime(
      State, CodeInvalidationPolicy::InvalidateOnExecutableWrite, 3, 2);

  RuntimePollResult First = Runtime->poll(1, 1);
  EXPECT_EQ(First.Status, RuntimePollStatus::Continue);
  RuntimePollResult InstructionLimit = Runtime->poll(2, 0);
  ASSERT_EQ(InstructionLimit.Status, RuntimePollStatus::BudgetExhausted);
  ASSERT_TRUE(InstructionLimit.Budget.has_value());
  EXPECT_EQ(InstructionLimit.Budget->Kind,
            TranslationBudgetKind::GuestInstructions);
  EXPECT_EQ(InstructionLimit.Budget->Limit, 3u);
  EXPECT_EQ(InstructionLimit.Budget->Observed, 3u);

  std::thread Canceller([&] { Runtime->requestCancellation(); });
  Canceller.join();
  RuntimePollResult Cancelled = Runtime->poll();
  EXPECT_EQ(Cancelled.Status, RuntimePollStatus::Cancelled);
  EXPECT_TRUE(Runtime->cancellationRequested());

  const RuntimeControlBlockV1 Snapshot =
      Runtime->snapshotControlBlock(0x1234, 9);
  EXPECT_EQ(Snapshot.CurrentPC, 0x1234u);
  EXPECT_EQ(Snapshot.ExpectedGeneration, 0u);
  EXPECT_EQ(Snapshot.InstructionCount, 3u);
  EXPECT_EQ(Snapshot.BlockCount, 1u);
  EXPECT_EQ(Snapshot.CancellationRequested, 1u);
  EXPECT_EQ(Snapshot.Exit.Kind, RuntimeABIExitKindV1::Cancelled);
  EXPECT_FALSE(static_cast<bool>(validateRuntimeControlBlockV1(Snapshot)));
}

} // namespace
