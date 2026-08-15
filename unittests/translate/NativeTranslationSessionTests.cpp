//===- NativeTranslationSessionTests.cpp - Native execution tests -------===//

#include "NativeTranslationSessionInternal.h"
#include "gtest/gtest.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/NativeTranslationSession.h"
#include "neverd/translate/ResolvedHostTarget.h"
#include "neverd/translate/RuntimeGuestState.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

constexpr uint64_t EntryPC = 0x401000;
constexpr uint64_t StackAddress = 0x700000;
constexpr uint64_t ReturnTarget = 0x500123;
constexpr uint64_t SecondBlockPC = EntryPC + 6;

TranslationOptions
nativeOptions(CodeInvalidationPolicy Invalidation =
                  CodeInvalidationPolicy::ValidateBeforeDispatch) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::JIT;
  Options.Target = HostTarget();
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  Options.LLVMLevel = LLVMOptimizationLevel::O2;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = Invalidation;
  return Options;
}

bool hasNativeAArch64Target() {
  llvm::Expected<ResolvedHostTarget> Host = resolveHostTarget(nativeOptions());
  if (!Host) {
    ADD_FAILURE() << llvm::toString(Host.takeError());
    return false;
  }
  return Host->architecture() == GuestArchitecture::AArch64;
}

void setRegister(GuestState &State, RegisterID ID, uint64_t Value) {
  ASSERT_FALSE(
      static_cast<bool>(setRegisterValue(State, ID, llvm::APInt(64, Value))));
}

GuestState executionState(bool IncludeStack = true) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          7,
                          {0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3}});
  if (IncludeStack) {
    std::vector<uint8_t> Stack(16, 0);
    for (unsigned Byte = 0; Byte != 8; ++Byte)
      Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
    State.Memory.push_back({StackAddress,
                            MemoryPermission::Read | MemoryPermission::Write, 0,
                            std::move(Stack)});
  }
  setRegister(State, 4, StackAddress);
  setRegister(State, 7, 41);
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState directBranchReturnState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC,
       MemoryPermission::Read | MemoryPermission::Execute,
       /*Generation=*/11,
       {0x48, 0x83, 0xc0, 0x01, 0xeb, 0x00, 0x48, 0x83, 0xc0, 0x02, 0xc3}});
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != 8; ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write,
                          /*Generation=*/0, std::move(Stack)});
  setRegister(State, 4, StackAddress);
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState conditionalBranchReturnState(bool ArithmeticProducesZero) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/18,
                          {0x48, 0x83, 0xc0, 0x01, 0x74, 0x05, 0x48, 0x83, 0xc0,
                           0x01, 0xc3, 0x48, 0x83, 0xc0, 0x05, 0xc3}});
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != 8; ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write,
                          /*Generation=*/0, std::move(Stack)});
  setRegister(State, 4, StackAddress);
  setRegister(State, 0,
              ArithmeticProducesZero ? std::numeric_limits<uint64_t>::max()
                                     : 0);
  setRegister(State, 16, EntryPC);
  setRegister(State, 17, ArithmeticProducesZero ? 0 : uint64_t{1} << 6);
  return State;
}

GuestState singleFlagConditionalBranchReturnState(uint8_t Opcode,
                                                  uint64_t FlagMask,
                                                  bool FlagSet) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/19,
                          {Opcode, 0x05, 0x48, 0x83, 0xc0, 0x01, 0xc3, 0x48,
                           0x83, 0xc0, 0x05, 0xc3}});
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != 8; ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write,
                          /*Generation=*/0, std::move(Stack)});
  setRegister(State, 4, StackAddress);
  setRegister(State, 16, EntryPC);
  setRegister(State, 17, FlagSet ? FlagMask : 0);
  return State;
}

GuestState multiFlagConditionalBranchReturnState(uint8_t Opcode,
                                                 uint64_t EFlags) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/20,
                          {Opcode, 0x05, 0x48, 0x83, 0xc0, 0x01, 0xc3, 0x48,
                           0x83, 0xc0, 0x05, 0xc3}});
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != 8; ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write,
                          /*Generation=*/0, std::move(Stack)});
  setRegister(State, 4, StackAddress);
  setRegister(State, 16, EntryPC);
  setRegister(State, 17, EFlags);
  return State;
}

GuestState compareTestConditionalBranchReturnState(bool UseTest,
                                                   bool BranchTaken) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC,
       MemoryPermission::Read | MemoryPermission::Execute,
       /*Generation=*/21,
       {0x48, static_cast<uint8_t>(UseTest ? 0x85 : 0x39), 0xd8, // test/cmp
        0x74, 0x05, 0x48, 0x83, 0xc1, 0x01, 0xc3,                // fallthrough
        0x48, 0x83, 0xc1, 0x05, 0xc3}});                         // taken
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != 8; ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write,
                          /*Generation=*/0, std::move(Stack)});
  const uint64_t Left = UseTest ? 0xf0 : 0x123456789abcdef0ULL;
  const uint64_t Right =
      UseTest ? (BranchTaken ? 0x0f : 0xff) : (BranchTaken ? Left : Left + 1);
  setRegister(State, 0, Left);
  setRegister(State, 3, Right);
  setRegister(State, 4, StackAddress);
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState
negativeImmediateCompareTestConditionalBranchReturnState(bool UseTest,
                                                         bool BranchTaken) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  std::vector<uint8_t> Code;
  if (UseTest) {
    // test rax, -2147483648 (REX.W F7 /0 id)
    Code = {0x48, 0xf7, 0xc0, 0x00, 0x00, 0x00, 0x80};
  } else {
    // cmp rax, -1 (REX.W 83 /7 ib)
    Code = {0x48, 0x83, 0xf8, 0xff};
  }
  Code.insert(Code.end(),
              {0x74, 0x05, 0x48, 0x83, 0xc1, 0x01, 0xc3, // fallthrough
               0x48, 0x83, 0xc1, 0x05, 0xc3});           // taken
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/22, std::move(Code)});
  std::vector<uint8_t> Stack(16, 0);
  for (unsigned Byte = 0; Byte != 8; ++Byte)
    Stack[Byte] = static_cast<uint8_t>(ReturnTarget >> (Byte * 8));
  State.Memory.push_back({StackAddress,
                          MemoryPermission::Read | MemoryPermission::Write,
                          /*Generation=*/0, std::move(Stack)});
  const uint64_t Operand =
      UseTest ? (BranchTaken ? 1 : uint64_t{1} << 63)
              : (BranchTaken ? std::numeric_limits<uint64_t>::max() : 0);
  setRegister(State, 0, Operand);
  setRegister(State, 4, StackAddress);
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState selfLoopState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/12,
                          {0x48, 0x83, 0xc0, 0x01, 0xeb, 0xfa}});
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState directBranchUnsupportedState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC,
       MemoryPermission::Read | MemoryPermission::Execute,
       /*Generation=*/13,
       {0x48, 0x83, 0xc0, 0x01, 0xeb, 0x00, 0x06, 0x06, 0x06, 0x06, 0x06,
        0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06}});
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState loweringUnsupportedState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/15,
                          {0x90, 0xc3}}); // nop; ret
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState directBranchLoweringUnsupportedState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/16,
                          {0x48, 0x83, 0xc0, 0x01, 0xeb, 0x00, 0x90, 0xc3}});
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState prefixedLoweringUnsupportedState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC,
       MemoryPermission::Read | MemoryPermission::Execute,
       /*Generation=*/17,
       {0x48, 0x89, 0xf8, 0x90, 0xc3}}); // mov rax, rdi; nop; ret
  setRegister(State, 7, 41);
  setRegister(State, 16, EntryPC);
  return State;
}

GuestState directBranchFetchFaultState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/14,
                          {0x48, 0x83, 0xc0, 0x01, 0xeb, 0x00}});
  setRegister(State, 16, EntryPC);
  return State;
}

uint64_t registerValue(const GuestState &State, RegisterID ID) {
  const GuestRegisterValue *Register = findRegisterValue(State, ID);
  EXPECT_NE(Register, nullptr);
  return Register ? Register->Value.getZExtValue() : 0;
}

void expectCreateError(
    llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> Session,
    NativeTranslationSessionErrorCode Expected) {
  ASSERT_FALSE(static_cast<bool>(Session));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Session.takeError(), [&](const NativeTranslationSessionError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(), Expected) << Error.detail().str();
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

void expectSessionError(llvm::Error Error,
                        NativeTranslationSessionErrorCode Expected) {
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Error), [&](const NativeTranslationSessionError &Failure) {
        SawTypedError = true;
        EXPECT_EQ(Failure.code(), Expected) << Failure.detail().str();
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

llvm::Error generatedCodeBudgetError(uint64_t Observed = 2,
                                     uint64_t Limit = 1) {
  return llvm::make_error<TranslationObjectRequestError>(
      TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded,
      "generated-code budget", std::nullopt, std::nullopt, std::nullopt,
      TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded, Observed,
      Limit, /*GuestInstructionCount=*/3);
}

llvm::Error builderBudgetError(uint64_t GuestPC = EntryPC + 7) {
  return llvm::make_error<TranslationObjectRequestError>(
      TranslationObjectRequestErrorCode::InstructionBudgetExceeded,
      "instruction budget", std::nullopt,
      X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      GuestPC);
}

llvm::Error unsupportedBuilderError(X86TranslationBlockBuilderErrorCode Code,
                                    uint64_t GuestPC = EntryPC + 3) {
  return llvm::make_error<TranslationObjectRequestError>(
      TranslationObjectRequestErrorCode::BlockConstructionFailed,
      "unsupported instruction", std::nullopt, Code, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, GuestPC);
}

llvm::Error
loweringError(TranslationBlockLoweringErrorCode Code =
                  TranslationBlockLoweringErrorCode::UnsupportedBlockShape,
              uint64_t GuestPC = EntryPC) {
  return llvm::make_error<TranslationObjectRequestError>(
      TranslationObjectRequestErrorCode::BlockLoweringFailed,
      "unsupported block shape", std::nullopt, std::nullopt, Code, std::nullopt,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, GuestPC);
}

TEST(NativeTranslationSession, RequiresNativeJITOptions) {
  TranslationOptions AOT = nativeOptions();
  AOT.Mode = TranslationMode::AOT;
  AOT.Target.Kind = HostTargetKind::Explicit;
  AOT.Target.Architecture = GuestArchitecture::AArch64;
  AOT.Target.Triple = "aarch64-unknown-linux-gnu";
  expectCreateError(NativeTranslationSessionV1::create(AOT, executionState()),
                    NativeTranslationSessionErrorCode::InvalidRequest);
}

TEST(NativeTranslationSession,
     ValidatesDirectBranchStatusAgainstTheManifestAndRuntimeRIP) {
  TranslationBlockDescriptorV1 Descriptor;
  Descriptor.Header.Terminator = TranslationBlockTerminatorKindV1::DirectBranch;
  Descriptor.Header.Flags = TranslationBlockDescriptorFlagV1::HasStaticTarget;
  Descriptor.Header.StaticTargetPC = EntryPC + 0x20;
  Descriptor.InstructionBoundaries.push_back(
      neverd::LowInstructionBoundary{.Address = EntryPC + 4});

  RuntimeGuestStateX86_64V1 RuntimeState;
  RuntimeState.RIP = Descriptor.Header.StaticTargetPC;
  llvm::Expected<BlockExitV1> Exit = detail::translateNativeBlockExitV1(
      static_cast<uint32_t>(BlockExitKindV1::DirectBranch), Descriptor,
      RuntimeState);
  ASSERT_TRUE(static_cast<bool>(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_EQ(Exit->Kind, BlockExitKindV1::DirectBranch);
  EXPECT_EQ(Exit->PC, EntryPC + 4);
  EXPECT_EQ(Exit->NextPC, 0u);
  EXPECT_EQ(Exit->TargetPC, EntryPC + 0x20);

  ++RuntimeState.RIP;
  llvm::Expected<BlockExitV1> Mismatched = detail::translateNativeBlockExitV1(
      static_cast<uint32_t>(BlockExitKindV1::DirectBranch), Descriptor,
      RuntimeState);
  ASSERT_FALSE(static_cast<bool>(Mismatched));
  EXPECT_NE(llvm::toString(Mismatched.takeError()).find("static target"),
            std::string::npos);
}

TEST(NativeTranslationSession,
     ValidatesAResolvedConditionalBranchAgainstBothManifestSuccessors) {
  TranslationBlockDescriptorV1 Descriptor;
  Descriptor.Header.Terminator =
      TranslationBlockTerminatorKindV1::ConditionalBranch;
  Descriptor.Header.Flags = TranslationBlockDescriptorFlagV1::HasStaticTarget;
  Descriptor.Header.FallthroughPC = EntryPC + 2;
  Descriptor.Header.StaticTargetPC = EntryPC + 7;
  Descriptor.InstructionBoundaries.push_back(
      neverd::LowInstructionBoundary{.Address = EntryPC});

  for (uint64_t SelectedPC :
       {Descriptor.Header.FallthroughPC, Descriptor.Header.StaticTargetPC}) {
    RuntimeGuestStateX86_64V1 RuntimeState;
    RuntimeState.RIP = SelectedPC;
    llvm::Expected<BlockExitV1> Exit = detail::translateNativeBlockExitV1(
        static_cast<uint32_t>(BlockExitKindV1::DirectBranch), Descriptor,
        RuntimeState);
    ASSERT_TRUE(static_cast<bool>(Exit)) << llvm::toString(Exit.takeError());
    EXPECT_EQ(Exit->Kind, BlockExitKindV1::DirectBranch);
    EXPECT_EQ(Exit->PC, EntryPC);
    EXPECT_EQ(Exit->TargetPC, SelectedPC);
  }

  RuntimeGuestStateX86_64V1 ForgedState;
  ForgedState.RIP = EntryPC + 3;
  llvm::Expected<BlockExitV1> Forged = detail::translateNativeBlockExitV1(
      static_cast<uint32_t>(BlockExitKindV1::DirectBranch), Descriptor,
      ForgedState);
  ASSERT_FALSE(static_cast<bool>(Forged));
  EXPECT_NE(llvm::toString(Forged.takeError()).find("conditional successor"),
            std::string::npos);

  RuntimeGuestStateX86_64V1 ValidState;
  ValidState.RIP = Descriptor.Header.StaticTargetPC;
  for (BlockExitKindV1 WrongKind :
       {BlockExitKindV1::Continue, BlockExitKindV1::Return}) {
    llvm::Expected<BlockExitV1> Wrong = detail::translateNativeBlockExitV1(
        static_cast<uint32_t>(WrongKind), Descriptor, ValidState);
    ASSERT_FALSE(static_cast<bool>(Wrong));
    EXPECT_NE(llvm::toString(Wrong.takeError()).find("manifest"),
              std::string::npos);
  }

  Descriptor.Header.FallthroughPC = Descriptor.Header.StaticTargetPC;
  llvm::Expected<BlockExitV1> Degenerate = detail::translateNativeBlockExitV1(
      static_cast<uint32_t>(BlockExitKindV1::DirectBranch), Descriptor,
      ValidState);
  ASSERT_TRUE(static_cast<bool>(Degenerate))
      << llvm::toString(Degenerate.takeError());
  EXPECT_EQ(Degenerate->TargetPC, Descriptor.Header.StaticTargetPC);
}

TEST(NativeTranslationSession,
     ClassifiesOnlyOneTypedObjectBudgetErrorAsAResult) {
  detail::NativeTranslationObjectFailureV1 Sole =
      detail::classifyNativeTranslationObjectFailureV1(
          generatedCodeBudgetError());
  ASSERT_TRUE(Sole.SoleCode.has_value());
  EXPECT_EQ(*Sole.SoleCode,
            TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded);
  EXPECT_FALSE(Sole.BuilderCode.has_value());
  EXPECT_FALSE(Sole.BuilderGuestPC.has_value());
  EXPECT_FALSE(Sole.BuilderMemoryFaultDetails.has_value());

  detail::NativeTranslationObjectFailureV1 BuilderBudget =
      detail::classifyNativeTranslationObjectFailureV1(builderBudgetError());
  ASSERT_TRUE(BuilderBudget.SoleCode.has_value());
  EXPECT_EQ(*BuilderBudget.SoleCode,
            TranslationObjectRequestErrorCode::InstructionBudgetExceeded);
  EXPECT_EQ(BuilderBudget.BuilderCode,
            X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded);
  EXPECT_EQ(BuilderBudget.BuilderGuestPC, EntryPC + 7);
  EXPECT_FALSE(BuilderBudget.BuilderMemoryFaultDetails.has_value());

  detail::NativeTranslationObjectFailureV1 Unlifted =
      detail::classifyNativeTranslationObjectFailureV1(unsupportedBuilderError(
          X86TranslationBlockBuilderErrorCode::UnliftedInstruction));
  EXPECT_EQ(Unlifted.SoleCode,
            TranslationObjectRequestErrorCode::BlockConstructionFailed);
  EXPECT_EQ(Unlifted.BuilderCode,
            X86TranslationBlockBuilderErrorCode::UnliftedInstruction);
  EXPECT_EQ(Unlifted.BuilderGuestPC, EntryPC + 3);
  EXPECT_FALSE(Unlifted.BuilderMemoryFaultDetails.has_value());

  detail::NativeTranslationObjectFailureV1 UnsupportedShape =
      detail::classifyNativeTranslationObjectFailureV1(loweringError());
  EXPECT_EQ(UnsupportedShape.SoleCode,
            TranslationObjectRequestErrorCode::BlockLoweringFailed);
  EXPECT_EQ(UnsupportedShape.LoweringCode,
            TranslationBlockLoweringErrorCode::UnsupportedBlockShape);
  EXPECT_EQ(UnsupportedShape.LoweringGuestPC, EntryPC);
  EXPECT_FALSE(UnsupportedShape.BuilderCode.has_value());
  EXPECT_FALSE(UnsupportedShape.BuilderGuestPC.has_value());
  std::optional<detail::NativeTranslationUnsupportedInstructionV1>
      PublishedUnsupported =
          detail::classifyNativeTranslationUnsupportedInstructionV1(
              UnsupportedShape);
  ASSERT_TRUE(PublishedUnsupported.has_value());
  EXPECT_EQ(PublishedUnsupported->GuestPC, EntryPC);
  EXPECT_EQ(PublishedUnsupported->Code,
            static_cast<uint64_t>(
                TranslationObjectRequestErrorCode::BlockLoweringFailed));
  EXPECT_EQ(PublishedUnsupported->Subcode,
            static_cast<uint64_t>(
                TranslationBlockLoweringErrorCode::UnsupportedBlockShape));

  for (TranslationBlockLoweringErrorCode InfrastructureCode :
       {TranslationBlockLoweringErrorCode::InvalidDescriptor,
        TranslationBlockLoweringErrorCode::UnsupportedOperation,
        TranslationBlockLoweringErrorCode::InvalidOperand,
        TranslationBlockLoweringErrorCode::UnsupportedRegister,
        TranslationBlockLoweringErrorCode::UndefinedTemporary,
        TranslationBlockLoweringErrorCode::InvalidControlFlow,
        TranslationBlockLoweringErrorCode::IRVerificationFailed}) {
    SCOPED_TRACE(static_cast<unsigned>(InfrastructureCode));
    detail::NativeTranslationObjectFailureV1 Infrastructure =
        detail::classifyNativeTranslationObjectFailureV1(
            loweringError(InfrastructureCode));
    EXPECT_FALSE(detail::classifyNativeTranslationUnsupportedInstructionV1(
                     Infrastructure)
                     .has_value());
  }

  detail::NativeTranslationObjectFailureV1 WithUnhandled =
      detail::classifyNativeTranslationObjectFailureV1(llvm::joinErrors(
          generatedCodeBudgetError(),
          llvm::createStringError(llvm::errc::io_error,
                                  "independent infrastructure failure")));
  EXPECT_FALSE(WithUnhandled.SoleCode.has_value());
  EXPECT_FALSE(WithUnhandled.BuilderCode.has_value());
  EXPECT_FALSE(WithUnhandled.BuilderGuestPC.has_value());
  EXPECT_FALSE(WithUnhandled.BuilderMemoryFaultDetails.has_value());
  EXPECT_NE(WithUnhandled.Detail.find("independent infrastructure failure"),
            std::string::npos);

  detail::NativeTranslationObjectFailureV1 BuilderWithUnhandled =
      detail::classifyNativeTranslationObjectFailureV1(llvm::joinErrors(
          unsupportedBuilderError(
              X86TranslationBlockBuilderErrorCode::UnliftedInstruction),
          llvm::createStringError(llvm::errc::io_error,
                                  "independent builder failure")));
  EXPECT_FALSE(BuilderWithUnhandled.SoleCode.has_value());
  EXPECT_FALSE(BuilderWithUnhandled.BuilderCode.has_value());
  EXPECT_FALSE(BuilderWithUnhandled.BuilderGuestPC.has_value());
  EXPECT_FALSE(BuilderWithUnhandled.BuilderMemoryFaultDetails.has_value());

  detail::NativeTranslationObjectFailureV1 Repeated =
      detail::classifyNativeTranslationObjectFailureV1(llvm::joinErrors(
          generatedCodeBudgetError(), generatedCodeBudgetError(3, 1)));
  EXPECT_FALSE(Repeated.SoleCode.has_value());
}

TEST(NativeTranslationSession,
     AcceptedCancellationWinsTheSuccessfulCommitLinearizationPoint) {
  detail::NativeTranslationRunControlV1 Control;
  Control.Running = true;

  detail::requestNativeTranslationCancellationV1(Control);
  bool CancellationWon = false;
  unsigned PublishedSideEffect = 0;
  ASSERT_FALSE(static_cast<bool>(detail::finalizeNativeTranslationRunV1(
      Control, [&](bool CancellationWins) {
        CancellationWon = CancellationWins;
        PublishedSideEffect = 42;
        return llvm::Error::success();
      })));

  EXPECT_TRUE(CancellationWon);
  EXPECT_EQ(PublishedSideEffect, 42u);
  EXPECT_FALSE(Control.Running);
  EXPECT_FALSE(Control.CancellationPending);
  EXPECT_FALSE(Control.ActiveRuntime);
}

TEST(NativeTranslationSession,
     CancellationAfterADirectBranchReportsTheCommittedTargetPC) {
  TranslationOptions Options = nativeOptions();
  GuestState State = directBranchReturnState();
  GuestMemoryRuntimeConfig Config;
  Config.CodeInvalidation = Options.CodeInvalidation;
  std::unique_ptr<GuestMemoryRuntime> Memory =
      llvm::cantFail(GuestMemoryRuntime::create(State, Config));
  Memory->requestCancellation();

  llvm::Expected<TranslationResult> Result =
      detail::makeNativeTranslationCancelledResultV1(
          Options, EntryPC, SecondBlockPC, *Memory,
          /*GuestInstructions=*/2, /*BlocksTranslated=*/1,
          /*GeneratedCodeBytes=*/64);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->StartPC, EntryPC);
  EXPECT_EQ(Result->Exit.Reason, TranslationStopReason::Cancelled);
  EXPECT_EQ(Result->Exit.PC, SecondBlockPC);
  EXPECT_EQ(Result->Exit.NextPC, SecondBlockPC);
  EXPECT_EQ(Result->GuestInstructions, 2u);
  EXPECT_EQ(Result->BlocksTranslated, 1u);
  EXPECT_EQ(Result->GeneratedCodeBytes, 64u);
  EXPECT_FALSE(static_cast<bool>(validateTranslationResult(*Result, Options)));
}

TEST(NativeTranslationSession,
     CancellationAfterTheSuccessfulCommitLinearizationPointIsIgnored) {
  detail::NativeTranslationRunControlV1 Control;
  Control.Running = true;

  std::mutex GateMutex;
  std::condition_variable Gate;
  bool CommitEntered = false;
  bool RequestStarted = false;
  bool CancellationWon = true;
  bool CommitFailed = false;

  std::thread CommitThread([&] {
    if (llvm::Error Error = detail::finalizeNativeTranslationRunV1(
            Control, [&](bool CancellationWins) {
              CancellationWon = CancellationWins;
              std::unique_lock Lock(GateMutex);
              CommitEntered = true;
              Gate.notify_all();
              Gate.wait(Lock, [&] { return RequestStarted; });
              return llvm::Error::success();
            })) {
      CommitFailed = true;
      llvm::consumeError(std::move(Error));
    }
  });

  {
    std::unique_lock Lock(GateMutex);
    Gate.wait(Lock, [&] { return CommitEntered; });
  }
  std::thread RequestThread([&] {
    {
      std::lock_guard Lock(GateMutex);
      RequestStarted = true;
    }
    Gate.notify_all();
    detail::requestNativeTranslationCancellationV1(Control);
  });

  CommitThread.join();
  RequestThread.join();
  EXPECT_FALSE(CommitFailed);
  EXPECT_FALSE(CancellationWon);
  EXPECT_FALSE(Control.Running);
  EXPECT_FALSE(Control.CancellationPending);
  EXPECT_FALSE(Control.ActiveRuntime);
}

TEST(NativeTranslationSession,
     ExecutesAuditedReturnBlockAndCommitsStateTransactionally) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(), executionState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  std::unique_ptr<NativeTranslationSessionV1> Session =
      std::move(*SessionOrErr);

  llvm::Expected<TranslationResult> ResultOrErr = Session->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
  EXPECT_EQ(ResultOrErr->StartPC, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC + 7);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, Session->options())));

  EXPECT_EQ(registerValue(Session->state(), 0), 42u);
  EXPECT_EQ(registerValue(Session->state(), 4), StackAddress + 8);
  EXPECT_EQ(registerValue(Session->state(), 16), ReturnTarget);
  ASSERT_EQ(Session->state().Memory.size(), 2u);
  EXPECT_EQ(Session->state().Memory.front().Generation, 7u);
}

TEST(NativeTranslationSession, DispatchesDirectBranchIntoASecondAuditedBlock) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
  EXPECT_EQ(ResultOrErr->StartPC, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC + 4);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 3u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
}

TEST(NativeTranslationSession,
     DispatchesBothSelectedSuccessorsOfAZeroFlagBranch) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  for (bool ArithmeticProducesZero : {false, true}) {
    SCOPED_TRACE(ArithmeticProducesZero);
    llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
        NativeTranslationSessionV1::create(
            nativeOptions(),
            conditionalBranchReturnState(ArithmeticProducesZero));
    ASSERT_TRUE(static_cast<bool>(SessionOrErr))
        << llvm::toString(SessionOrErr.takeError());

    llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
    ASSERT_TRUE(static_cast<bool>(ResultOrErr))
        << llvm::toString(ResultOrErr.takeError());
    EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
    EXPECT_EQ(ResultOrErr->StartPC, EntryPC);
    EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
    EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
    EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
    EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0),
              ArithmeticProducesZero ? 5u : 2u);
    EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
    EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
    EXPECT_FALSE(static_cast<bool>(
        validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  }
}

TEST(NativeTranslationSession,
     ExecutesCompareAndTestBeforeDispatchingBothBranchSuccessors) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  for (bool UseTest : {false, true}) {
    for (bool BranchTaken : {false, true}) {
      SCOPED_TRACE(UseTest ? "test" : "cmp");
      SCOPED_TRACE(BranchTaken);
      GuestState Initial =
          compareTestConditionalBranchReturnState(UseTest, BranchTaken);
      const uint64_t InitialRAX = registerValue(Initial, 0);
      const uint64_t InitialRBX = registerValue(Initial, 3);
      llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
          NativeTranslationSessionV1::create(nativeOptions(),
                                             std::move(Initial));
      ASSERT_TRUE(static_cast<bool>(SessionOrErr))
          << llvm::toString(SessionOrErr.takeError());

      llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
      ASSERT_TRUE(static_cast<bool>(ResultOrErr))
          << llvm::toString(ResultOrErr.takeError());
      EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
      EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
      EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
      EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), InitialRAX);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 3), InitialRBX);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 1),
                BranchTaken ? 5u : 1u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
      EXPECT_FALSE(static_cast<bool>(
          validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
    }
  }
}

TEST(NativeTranslationSession,
     ExecutesNegativeImmediateCompareAndTestThroughTheNativeDispatcher) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  for (bool UseTest : {false, true}) {
    for (bool BranchTaken : {false, true}) {
      SCOPED_TRACE(UseTest ? "test F7 /0" : "cmp 83 /7");
      SCOPED_TRACE(BranchTaken);
      GuestState Initial =
          negativeImmediateCompareTestConditionalBranchReturnState(UseTest,
                                                                   BranchTaken);
      const uint64_t InitialRAX = registerValue(Initial, 0);
      llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
          NativeTranslationSessionV1::create(nativeOptions(),
                                             std::move(Initial));
      ASSERT_TRUE(static_cast<bool>(SessionOrErr))
          << llvm::toString(SessionOrErr.takeError());

      llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
      ASSERT_TRUE(static_cast<bool>(ResultOrErr))
          << llvm::toString(ResultOrErr.takeError());
      const uint64_t ImmediateInstructionSize = UseTest ? 7 : 4;
      EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
      EXPECT_EQ(ResultOrErr->StartPC, EntryPC);
      EXPECT_EQ(ResultOrErr->Exit.PC,
                EntryPC + ImmediateInstructionSize + (BranchTaken ? 11 : 6));
      EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
      EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
      EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
      EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), InitialRAX);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 1),
                BranchTaken ? 5u : 1u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
      EXPECT_FALSE(static_cast<bool>(
          validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
    }
  }
}

TEST(NativeTranslationSession,
     DispatchesBothSelectedSuccessorsOfSingleFlagBranches) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  struct BranchCase {
    const char *Name;
    uint8_t Opcode;
    uint64_t FlagMask;
    bool TakenWhenSet;
  };
  constexpr std::array<BranchCase, 8> Cases = {{
      {"jo", 0x70, uint64_t{1} << 11, true},
      {"jno", 0x71, uint64_t{1} << 11, false},
      {"jb", 0x72, uint64_t{1}, true},
      {"jae", 0x73, uint64_t{1}, false},
      {"js", 0x78, uint64_t{1} << 7, true},
      {"jns", 0x79, uint64_t{1} << 7, false},
      {"jp", 0x7a, uint64_t{1} << 2, true},
      {"jnp", 0x7b, uint64_t{1} << 2, false},
  }};

  for (const BranchCase &Case : Cases) {
    for (bool FlagSet : {false, true}) {
      SCOPED_TRACE(Case.Name);
      SCOPED_TRACE(FlagSet);
      llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
          NativeTranslationSessionV1::create(
              nativeOptions(), singleFlagConditionalBranchReturnState(
                                   Case.Opcode, Case.FlagMask, FlagSet));
      ASSERT_TRUE(static_cast<bool>(SessionOrErr))
          << llvm::toString(SessionOrErr.takeError());

      llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
      ASSERT_TRUE(static_cast<bool>(ResultOrErr))
          << llvm::toString(ResultOrErr.takeError());
      EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
      EXPECT_EQ(ResultOrErr->StartPC, EntryPC);
      EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
      EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
      EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
      const bool IsTaken = FlagSet == Case.TakenWhenSet;
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), IsTaken ? 5u : 1u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
      EXPECT_FALSE(static_cast<bool>(
          validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
    }
  }
}

TEST(NativeTranslationSession,
     DispatchesBothSelectedSuccessorsOfEveryMultiFlagBranch) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  struct BranchCase {
    const char *Name;
    uint8_t Opcode;
    uint64_t TakenEFlags;
    uint64_t FallthroughEFlags;
  };
  constexpr std::array<BranchCase, 6> Cases = {{
      {"jbe", 0x76, uint64_t{1}, 0},
      {"ja", 0x77, 0, uint64_t{1} << 6},
      {"jl", 0x7c, uint64_t{1} << 7, 0},
      {"jge", 0x7d, 0, uint64_t{1} << 7},
      {"jle", 0x7e, uint64_t{1} << 6, 0},
      {"jg", 0x7f, 0, uint64_t{1} << 6},
  }};

  for (const BranchCase &Case : Cases) {
    for (bool Taken : {false, true}) {
      SCOPED_TRACE(Case.Name);
      SCOPED_TRACE(Taken);
      llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
          NativeTranslationSessionV1::create(
              nativeOptions(),
              multiFlagConditionalBranchReturnState(
                  Case.Opcode,
                  Taken ? Case.TakenEFlags : Case.FallthroughEFlags));
      ASSERT_TRUE(static_cast<bool>(SessionOrErr))
          << llvm::toString(SessionOrErr.takeError());

      llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
      ASSERT_TRUE(static_cast<bool>(ResultOrErr))
          << llvm::toString(ResultOrErr.takeError());
      EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
      EXPECT_EQ(ResultOrErr->StartPC, EntryPC);
      EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
      EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
      EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), Taken ? 5u : 1u);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
      EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
      EXPECT_FALSE(static_cast<bool>(
          validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
    }
  }
}

TEST(NativeTranslationSession,
     StopsAtTheDirectBranchTargetAtTheExactBlockBudget) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.BlockBudget = 1;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind, TranslationBudgetKind::Blocks);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, 1u);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, 1u);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     StopsAtTheDirectBranchTargetAtTheExactInstructionBudget) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.InstructionBudget = 2;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind,
            TranslationBudgetKind::GuestInstructions);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, 2u);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, 2u);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession, StopsASelfLoopAtTheThirdBlockBoundary) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.BlockBudget = 3;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, selfLoopState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind, TranslationBudgetKind::Blocks);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, 3u);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, 3u);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 6u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 3u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 3u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), EntryPC);
}

TEST(NativeTranslationSession,
     StopsBeforeTheNextBlockWhenGeneratedCodeReachesItsExactBudget) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions ProbeOptions = nativeOptions();
  ProbeOptions.BlockBudget = 1;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> ProbeOrErr =
      NativeTranslationSessionV1::create(ProbeOptions,
                                         directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(ProbeOrErr))
      << llvm::toString(ProbeOrErr.takeError());
  llvm::Expected<TranslationResult> ProbeResult = (*ProbeOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ProbeResult))
      << llvm::toString(ProbeResult.takeError());
  ASSERT_GT(ProbeResult->GeneratedCodeBytes, 0u);
  const uint64_t FirstBlockCodeBytes = ProbeResult->GeneratedCodeBytes;

  TranslationOptions Options = nativeOptions();
  Options.GeneratedCodeByteBudget = FirstBlockCodeBytes;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind,
            TranslationBudgetKind::GeneratedCodeBytes);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, FirstBlockCodeBytes);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, FirstBlockCodeBytes);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, FirstBlockCodeBytes);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     PassesTheRemainingInstructionBudgetToTheSecondBlock) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.InstructionBudget = 3;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind,
            TranslationBudgetKind::GuestInstructions);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, 3u);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, 3u);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC + 4);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC + 4);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     TerminalSecondBlockWinsAtExactInstructionAndBlockBudgets) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.InstructionBudget = 4;
  Options.BlockBudget = 2;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC + 4);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 3u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
}

TEST(NativeTranslationSession,
     TerminalSecondBlockWinsAtTheExactGeneratedCodeBudget) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> ProbeOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(ProbeOrErr))
      << llvm::toString(ProbeOrErr.takeError());
  llvm::Expected<TranslationResult> ProbeResult = (*ProbeOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ProbeResult))
      << llvm::toString(ProbeResult.takeError());
  ASSERT_EQ(ProbeResult->Exit.Reason, TranslationStopReason::Returned);
  ASSERT_GT(ProbeResult->GeneratedCodeBytes, 0u);

  TranslationOptions Options = nativeOptions();
  Options.GeneratedCodeByteBudget = ProbeResult->GeneratedCodeBytes;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC + 4);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, ReturnTarget);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, ProbeResult->GeneratedCodeBytes);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 3u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress + 8);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), ReturnTarget);
}

TEST(NativeTranslationSession,
     AggregatesASecondBlockGeneratedCodeOverrunAgainstTheRunBudget) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions FirstOptions = nativeOptions();
  FirstOptions.BlockBudget = 1;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> FirstOrErr =
      NativeTranslationSessionV1::create(FirstOptions,
                                         directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(FirstOrErr))
      << llvm::toString(FirstOrErr.takeError());
  llvm::Expected<TranslationResult> FirstResult = (*FirstOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(FirstResult))
      << llvm::toString(FirstResult.takeError());

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> FullOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(FullOrErr))
      << llvm::toString(FullOrErr.takeError());
  llvm::Expected<TranslationResult> FullResult = (*FullOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(FullResult))
      << llvm::toString(FullResult.takeError());
  ASSERT_GT(FullResult->GeneratedCodeBytes, FirstResult->GeneratedCodeBytes);
  const uint64_t RunLimit = FullResult->GeneratedCodeBytes - 1;

  TranslationOptions Options = nativeOptions();
  Options.GeneratedCodeByteBudget = RunLimit;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, directBranchReturnState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind,
            TranslationBudgetKind::GeneratedCodeBytes);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, RunLimit);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, FullResult->GeneratedCodeBytes);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 4u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 2u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, FullResult->GeneratedCodeBytes);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 4), StackAddress);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     ReportsGuestStackFaultAndCommitsCompletedPriorInstructions) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         executionState(false));
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  std::unique_ptr<NativeTranslationSessionV1> Session =
      std::move(*SessionOrErr);

  llvm::Expected<TranslationResult> ResultOrErr = Session->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::MemoryFault);
  ASSERT_TRUE(ResultOrErr->Exit.MemoryFault.has_value());
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Address, StackAddress);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Access, MemoryAccessKind::Read);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->AccessWidthBits, 64u);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, Session->options())));

  EXPECT_EQ(registerValue(Session->state(), 0), 42u);
  EXPECT_EQ(registerValue(Session->state(), 4), StackAddress);
  EXPECT_EQ(registerValue(Session->state(), 16), EntryPC + 7);
}

TEST(NativeTranslationSession,
     ReportsUnmappedInstructionFetchAsAnExactGuestMemoryFault) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  setRegister(State, 16, EntryPC);
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(), std::move(State));
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState((*SessionOrErr)->state()));

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::MemoryFault);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  ASSERT_TRUE(ResultOrErr->Exit.MemoryFault.has_value());
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Address, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Access, MemoryAccessKind::Execute);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->AccessWidthBits, 8u);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->RequiredAlignment, 0u);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 0u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 0u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState((*SessionOrErr)->state())),
            Before);
}

TEST(NativeTranslationSession,
     KeepsTheCommittedPCWhenFetchFailsAfterAnUnexecutedPrefix) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          /*Generation=*/18,
                          {0x48, 0x89, 0xf8}}); // mov rax, rdi
  setRegister(State, 7, 41);
  setRegister(State, 16, EntryPC);
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(), std::move(State));
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState((*SessionOrErr)->state()));

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::MemoryFault);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC + 3);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  ASSERT_TRUE(ResultOrErr->Exit.MemoryFault.has_value());
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Address, EntryPC + 3);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Access, MemoryAccessKind::Execute);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->AccessWidthBits, 8u);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->RequiredAlignment, 0u);
  EXPECT_FALSE(ResultOrErr->Exit.Trap.has_value());
  EXPECT_EQ(ResultOrErr->GuestInstructions, 0u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 0u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState((*SessionOrErr)->state())),
            Before);
}

TEST(NativeTranslationSession,
     ReportsInstructionExecutePermissionAsAnExactGuestMemoryFault) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC, MemoryPermission::Read, /*Generation=*/9, {0xc3}});
  setRegister(State, 16, EntryPC);
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(), std::move(State));
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::MemoryFault);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  ASSERT_TRUE(ResultOrErr->Exit.MemoryFault.has_value());
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Address, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Access, MemoryAccessKind::Execute);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->AccessWidthBits, 8u);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->RequiredAlignment, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
}

TEST(NativeTranslationSession,
     ReportsTheSecondUndecodableInstructionAsAnExactUnsupportedTrap) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back(
      {EntryPC,
       MemoryPermission::Read | MemoryPermission::Execute,
       /*Generation=*/10,
       {0x48, 0x89, 0xf8, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
        0x06, 0x06, 0x06, 0x06, 0x06, 0x06}});
  setRegister(State, 16, EntryPC);
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(), std::move(State));
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState((*SessionOrErr)->state()));

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason,
            TranslationStopReason::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC + 3);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  ASSERT_TRUE(ResultOrErr->Exit.Trap.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Trap->Kind,
            TranslationTrapKind::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.Trap->Code,
            static_cast<uint64_t>(
                X86TranslationBlockBuilderErrorCode::UndecodableInstruction));
  EXPECT_EQ(ResultOrErr->Exit.Trap->Subcode, 0u);
  EXPECT_EQ(ResultOrErr->Exit.Trap->Address, EntryPC + 3);
  EXPECT_FALSE(ResultOrErr->Exit.Trap->Restartable);
  EXPECT_FALSE(ResultOrErr->Exit.FallbackRequested);
  EXPECT_EQ(ResultOrErr->Exit.Diagnostic,
            "translation object request: block construction failed (x86-64 "
            "translation instruction is undecodable at guest PC 0x401003)");
  EXPECT_EQ(ResultOrErr->GuestInstructions, 0u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 0u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState((*SessionOrErr)->state())),
            Before);
}

TEST(NativeTranslationSession,
     ReportsAnUnsupportedLoweringSliceAtTheGuestBlockEntry) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         loweringUnsupportedState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState((*SessionOrErr)->state()));

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason,
            TranslationStopReason::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  ASSERT_TRUE(ResultOrErr->Exit.Trap.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Trap->Kind,
            TranslationTrapKind::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.Trap->Code,
            static_cast<uint64_t>(
                TranslationObjectRequestErrorCode::BlockLoweringFailed));
  EXPECT_EQ(ResultOrErr->Exit.Trap->Subcode,
            static_cast<uint64_t>(
                TranslationBlockLoweringErrorCode::UnsupportedBlockShape));
  EXPECT_EQ(ResultOrErr->Exit.Trap->Address, EntryPC);
  EXPECT_TRUE(ResultOrErr->Exit.Trap->Restartable);
  EXPECT_FALSE(ResultOrErr->Exit.FallbackRequested);
  EXPECT_FALSE(ResultOrErr->Exit.Diagnostic.empty());
  EXPECT_EQ(ResultOrErr->GuestInstructions, 0u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 0u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState((*SessionOrErr)->state())),
            Before);
}

TEST(NativeTranslationSession,
     DoesNotClaimThatAnUnexecutedSupportedPrefixIsRestartableAtTheFailure) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         prefixedLoweringUnsupportedState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState((*SessionOrErr)->state()));

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason,
            TranslationStopReason::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC + 3);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC);
  ASSERT_TRUE(ResultOrErr->Exit.Trap.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Trap->Address, EntryPC + 3);
  EXPECT_FALSE(ResultOrErr->Exit.Trap->Restartable);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 0u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 0u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState((*SessionOrErr)->state())),
            Before);
}

TEST(NativeTranslationSession,
     CommitsTheFirstBlockBeforeASecondBlockUnsupportedStop) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         directBranchUnsupportedState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason,
            TranslationStopReason::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  ASSERT_TRUE(ResultOrErr->Exit.Trap.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Trap->Kind,
            TranslationTrapKind::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.Trap->Code,
            static_cast<uint64_t>(
                X86TranslationBlockBuilderErrorCode::UndecodableInstruction));
  EXPECT_EQ(ResultOrErr->Exit.Trap->Address, SecondBlockPC);
  EXPECT_FALSE(ResultOrErr->Exit.FallbackRequested);
  EXPECT_FALSE(ResultOrErr->Exit.Diagnostic.empty());
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     CommitsTheFirstBlockBeforeASecondBlockLoweringSliceStop) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(
          nativeOptions(), directBranchLoweringUnsupportedState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason,
            TranslationStopReason::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  ASSERT_TRUE(ResultOrErr->Exit.Trap.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Trap->Kind,
            TranslationTrapKind::UnsupportedInstruction);
  EXPECT_EQ(ResultOrErr->Exit.Trap->Code,
            static_cast<uint64_t>(
                TranslationObjectRequestErrorCode::BlockLoweringFailed));
  EXPECT_EQ(ResultOrErr->Exit.Trap->Subcode,
            static_cast<uint64_t>(
                TranslationBlockLoweringErrorCode::UnsupportedBlockShape));
  EXPECT_EQ(ResultOrErr->Exit.Trap->Address, SecondBlockPC);
  EXPECT_TRUE(ResultOrErr->Exit.Trap->Restartable);
  EXPECT_FALSE(ResultOrErr->Exit.FallbackRequested);
  EXPECT_FALSE(ResultOrErr->Exit.Diagnostic.empty());
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     CommitsTheFirstBlockBeforeASecondBlockInstructionFetchFault) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(),
                                         directBranchFetchFaultState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::MemoryFault);
  EXPECT_EQ(ResultOrErr->Exit.PC, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, SecondBlockPC);
  ASSERT_TRUE(ResultOrErr->Exit.MemoryFault.has_value());
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Address, SecondBlockPC);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->Access, MemoryAccessKind::Execute);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->AccessWidthBits, 8u);
  EXPECT_EQ(ResultOrErr->Exit.MemoryFault->RequiredAlignment, 0u);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_GT(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));

  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 0), 1u);
  EXPECT_EQ(registerValue((*SessionOrErr)->state(), 16), SecondBlockPC);
}

TEST(NativeTranslationSession,
     StopsAtTheInstructionTranslationBudgetWithoutPublishingCode) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.InstructionBudget = 2;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, executionState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  std::unique_ptr<NativeTranslationSessionV1> Session =
      std::move(*SessionOrErr);
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState(Session->state()));

  llvm::Expected<TranslationResult> ResultOrErr = Session->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind,
            TranslationBudgetKind::GuestInstructions);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, 2u);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Observed, 2u);
  EXPECT_EQ(ResultOrErr->Exit.PC, EntryPC + 7);
  EXPECT_EQ(ResultOrErr->Exit.NextPC, EntryPC + 7);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 2u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 0u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes, 0u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, Session->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState(Session->state())), Before);
}

TEST(NativeTranslationSession,
     TerminalReturnWinsAtExactInstructionAndBlockBudgets) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.InstructionBudget = 3;
  Options.BlockBudget = 1;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, executionState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  EXPECT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::Returned);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
}

TEST(NativeTranslationSession,
     ReportsTheObservedGeneratedCodeBudgetWithoutPublishingCode) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  TranslationOptions Options = nativeOptions();
  Options.GeneratedCodeByteBudget = 1;
  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(Options, executionState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState((*SessionOrErr)->state()));

  llvm::Expected<TranslationResult> ResultOrErr = (*SessionOrErr)->run();
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  ASSERT_EQ(ResultOrErr->Exit.Reason, TranslationStopReason::BudgetExhausted);
  ASSERT_TRUE(ResultOrErr->Exit.Budget.has_value());
  EXPECT_EQ(ResultOrErr->Exit.Budget->Kind,
            TranslationBudgetKind::GeneratedCodeBytes);
  EXPECT_EQ(ResultOrErr->Exit.Budget->Limit, 1u);
  EXPECT_GT(ResultOrErr->Exit.Budget->Observed, 1u);
  EXPECT_EQ(ResultOrErr->GeneratedCodeBytes,
            ResultOrErr->Exit.Budget->Observed);
  EXPECT_EQ(ResultOrErr->GuestInstructions, 3u);
  EXPECT_EQ(ResultOrErr->BlocksTranslated, 1u);
  EXPECT_FALSE(static_cast<bool>(
      validateTranslationResult(*ResultOrErr, (*SessionOrErr)->options())));
  EXPECT_EQ(llvm::cantFail(serializeGuestState((*SessionOrErr)->state())),
            Before);
}

TEST(NativeTranslationSession, RejectsInvalidRestoreWithoutChangingState) {
  if (!hasNativeAArch64Target())
    GTEST_SKIP() << "native AArch64 execution is unavailable";

  llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>> SessionOrErr =
      NativeTranslationSessionV1::create(nativeOptions(), executionState());
  ASSERT_TRUE(static_cast<bool>(SessionOrErr))
      << llvm::toString(SessionOrErr.takeError());
  std::unique_ptr<NativeTranslationSessionV1> Session =
      std::move(*SessionOrErr);
  const std::vector<uint8_t> Before =
      llvm::cantFail(serializeGuestState(Session->state()));

  GuestState Wrong = executionState();
  Wrong.Architecture = GuestArchitecture::ARM32;
  expectSessionError(Session->restoreState(std::move(Wrong)),
                     NativeTranslationSessionErrorCode::StateCommitFailed);
  EXPECT_EQ(llvm::cantFail(serializeGuestState(Session->state())), Before);
}

} // namespace
