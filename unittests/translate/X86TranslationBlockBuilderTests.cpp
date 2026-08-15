//===- X86TranslationBlockBuilderTests.cpp - Exact x86 block tests -------===//

#include "gtest/gtest.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/GuestState.h"
#include "neverd/translate/X86TranslationBlockBuilder.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

std::unique_ptr<GuestMemoryRuntime>
runtimeWithCode(uint64_t Address, uint64_t Generation,
                std::initializer_list<uint8_t> Bytes) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({Address,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          Generation, std::vector<uint8_t>(Bytes)});
  return llvm::cantFail(GuestMemoryRuntime::create(State));
}

std::unique_ptr<GuestMemoryRuntime>
runtimeWithRegions(std::vector<GuestMemoryRegion> Regions) {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory = std::move(Regions);
  return llvm::cantFail(GuestMemoryRuntime::create(State));
}

void expectInvalidDescriptor(llvm::Error Error) {
  EXPECT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
}

TEST(X86TranslationBlockBuilder, BuildsMovAddRetBlock) {
  constexpr uint64_t EntryPC = 0x1000;
  std::unique_ptr<GuestMemoryRuntime> Runtime = runtimeWithCode(
      EntryPC, 7, {0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
      Builder->build(*Runtime, EntryPC);
  ASSERT_TRUE(static_cast<bool>(BlockOrErr))
      << llvm::toString(BlockOrErr.takeError());
  const TranslationBlockDescriptorV1 &Block = *BlockOrErr;

  EXPECT_EQ(Block.Header.Magic, kTranslationBlockDescriptorMagicV1);
  EXPECT_EQ(Block.Header.Version, kTranslationBlockDescriptorVersionV1);
  EXPECT_EQ(Block.Header.Size, kTranslationBlockDescriptorHeaderSizeV1);
  EXPECT_EQ(Block.Header.EntryPC, EntryPC);
  EXPECT_EQ(Block.Header.FallthroughPC, EntryPC + 8);
  EXPECT_EQ(Block.Header.Terminator, TranslationBlockTerminatorKindV1::Return);
  EXPECT_EQ(Block.Header.GuestInstructionCount, 3u);
  EXPECT_EQ(Block.Header.GuestByteCount, 8u);
  EXPECT_EQ(Block.Bytes, (std::vector<uint8_t>{0x48, 0x89, 0xf8, 0x48, 0x83,
                                               0xc0, 0x01, 0xc3}));

  ASSERT_EQ(Block.InstructionBoundaries.size(), 3u);
  EXPECT_EQ(Block.InstructionBoundaries[0].Address, EntryPC);
  EXPECT_EQ(Block.InstructionBoundaries[0].Size, 3u);
  EXPECT_EQ(Block.InstructionBoundaries[1].Address, EntryPC + 3);
  EXPECT_EQ(Block.InstructionBoundaries[1].Size, 4u);
  EXPECT_EQ(Block.InstructionBoundaries[2].Address, EntryPC + 7);
  EXPECT_EQ(Block.InstructionBoundaries[2].Size, 1u);
  EXPECT_EQ(Block.InstructionBoundaries[2].Control,
            LowInstructionControl::Return);
  EXPECT_FALSE(Block.InstructionBoundaries[2].Immediate.has_value());

  ASSERT_EQ(Block.GenerationBindings.size(), 1u);
  EXPECT_EQ(Block.GenerationBindings[0].Address, EntryPC);
  EXPECT_EQ(Block.GenerationBindings[0].Size, 8u);
  EXPECT_EQ(Block.GenerationBindings[0].Generation, 7u);
  if (llvm::Error Error = validateTranslationBlockDescriptorV1(Block))
    ADD_FAILURE() << llvm::toString(std::move(Error));
}

TEST(X86TranslationBlockBuilder, StopsAtDirectBranchWithExactTarget) {
  constexpr uint64_t EntryPC = 0x2000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 3, {0xeb, 0x05, 0xcc});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::DirectBranch);
  EXPECT_EQ(Block.Header.FallthroughPC, EntryPC + 2);
  EXPECT_TRUE(hasTranslationBlockDescriptorFlag(
      Block.Header.Flags, TranslationBlockDescriptorFlagV1::HasStaticTarget));
  EXPECT_EQ(Block.Header.StaticTargetPC, EntryPC + 7);
  EXPECT_EQ(Block.Header.GuestInstructionCount, 1u);
  EXPECT_EQ(Block.Bytes, (std::vector<uint8_t>{0xeb, 0x05}));
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Block.InstructionBoundaries.front().Control,
            LowInstructionControl::Branch);
  ASSERT_TRUE(Block.InstructionBoundaries.front().Immediate.has_value());
  EXPECT_EQ(*Block.InstructionBoundaries.front().Immediate, EntryPC + 7);
}

TEST(X86TranslationBlockBuilder, RecordsConditionalTargetAndFallthrough) {
  constexpr uint64_t EntryPC = 0x3000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 4, {0x75, 0x02, 0xcc});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::ConditionalBranch);
  EXPECT_EQ(Block.Header.FallthroughPC, EntryPC + 2);
  EXPECT_EQ(Block.Header.StaticTargetPC, EntryPC + 4);
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Block.InstructionBoundaries.front().ControlFlags,
      LowInstructionControlFlag::Conditional));
}

TEST(X86TranslationBlockBuilder, ClassifiesLoopFamilyAsConditionalBranches) {
  constexpr uint64_t EntryPC = 0x3800;
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  for (const uint8_t Opcode : {uint8_t{0xe0}, uint8_t{0xe1}, uint8_t{0xe2}}) {
    std::unique_ptr<GuestMemoryRuntime> Runtime = runtimeWithRegions({
        {EntryPC,
         MemoryPermission::Read | MemoryPermission::Execute,
         4,
         {Opcode, 0xfe}},
    });
    TranslationBlockDescriptorV1 Block =
        llvm::cantFail(Builder->build(*Runtime, EntryPC));
    EXPECT_EQ(Block.Header.Terminator,
              TranslationBlockTerminatorKindV1::ConditionalBranch);
    EXPECT_EQ(Block.Header.FallthroughPC, EntryPC + 2);
    EXPECT_EQ(Block.Header.StaticTargetPC, EntryPC);
    ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
    EXPECT_EQ(Block.InstructionBoundaries.front().Control,
              LowInstructionControl::Branch);
    EXPECT_TRUE(hasLowInstructionControlFlag(
        Block.InstructionBoundaries.front().ControlFlags,
        LowInstructionControlFlag::Conditional));
  }
}

TEST(X86TranslationBlockBuilder, StopsAtIndirectBranch) {
  constexpr uint64_t EntryPC = 0x4000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 5, {0xff, 0xe0, 0xcc});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::IndirectBranch);
  EXPECT_EQ(Block.Header.FallthroughPC, EntryPC + 2);
  EXPECT_FALSE(hasTranslationBlockDescriptorFlag(
      Block.Header.Flags, TranslationBlockDescriptorFlagV1::HasStaticTarget));
  EXPECT_EQ(Block.Header.StaticTargetPC, 0u);
  EXPECT_EQ(Block.Bytes, (std::vector<uint8_t>{0xff, 0xe0}));
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Block.InstructionBoundaries.front().ControlFlags,
      LowInstructionControlFlag::Indirect));
}

TEST(X86TranslationBlockBuilder, ClassifiesGetPcEncodingAsDirectCall) {
  constexpr uint64_t EntryPC = 0x5000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 6, {0xe8, 0x00, 0x00, 0x00, 0x00, 0xcc});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator,
            TranslationBlockTerminatorKindV1::DirectCall);
  EXPECT_EQ(Block.Header.FallthroughPC, EntryPC + 5);
  EXPECT_EQ(Block.Header.StaticTargetPC, EntryPC + 5);
  EXPECT_EQ(Block.Header.GuestInstructionCount, 1u);
  EXPECT_EQ(Block.Bytes, (std::vector<uint8_t>{0xe8, 0x00, 0x00, 0x00, 0x00}));
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Block.InstructionBoundaries.front().Control,
            LowInstructionControl::Call);
  ASSERT_TRUE(Block.InstructionBoundaries.front().Immediate.has_value());
  EXPECT_EQ(*Block.InstructionBoundaries.front().Immediate, EntryPC + 5);
}

TEST(X86TranslationBlockBuilder, ResetsDecoderStateBetweenIndependentBlocks) {
  constexpr uint64_t CallPC = 0x12345000;
  constexpr uint64_t PopPC = 0x6800;
  std::unique_ptr<GuestMemoryRuntime> CallRuntime =
      runtimeWithCode(CallPC, 6, {0xe8, 0x00, 0x00, 0x00, 0x00});
  std::unique_ptr<GuestMemoryRuntime> PopRuntime =
      runtimeWithCode(PopPC, 7, {0x58, 0xc3});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 CallBlock =
      llvm::cantFail(Builder->build(*CallRuntime, CallPC));
  ASSERT_EQ(CallBlock.Header.Terminator,
            TranslationBlockTerminatorKindV1::DirectCall);

  TranslationBlockDescriptorV1 PopBlock =
      llvm::cantFail(Builder->build(*PopRuntime, PopPC));
  ASSERT_EQ(PopBlock.Header.Terminator,
            TranslationBlockTerminatorKindV1::Return);
  for (const LowOp &Op : PopBlock.Ops) {
    for (uint8_t I = 0; I < Op.NumInputs; ++I) {
      EXPECT_FALSE(Op.Inputs[I].isConst() && Op.Inputs[I].Offset == CallPC + 5)
          << "get-PC state leaked from a previously built block";
    }
  }
}

TEST(X86TranslationBlockBuilder, RecordsReturnImmediateExactly) {
  constexpr uint64_t EntryPC = 0x6000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 7, {0xc2, 0x34, 0x12});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator, TranslationBlockTerminatorKindV1::Return);
  EXPECT_TRUE(hasTranslationBlockDescriptorFlag(
      Block.Header.Flags,
      TranslationBlockDescriptorFlagV1::HasReturnImmediate));
  EXPECT_EQ(Block.Header.ReturnImmediate, 0x1234u);
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  ASSERT_TRUE(Block.InstructionBoundaries.front().Immediate.has_value());
  EXPECT_EQ(*Block.InstructionBoundaries.front().Immediate, 0x1234u);
}

TEST(X86TranslationBlockBuilder, DecodesOneByteInstructionAtMappedEnd) {
  constexpr uint64_t EntryPC = 0x7000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 8, {0xc3});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator, TranslationBlockTerminatorKindV1::Return);
  EXPECT_EQ(Block.Header.GuestByteCount, 1u);
  EXPECT_EQ(Block.Bytes, (std::vector<uint8_t>{0xc3}));
  ASSERT_EQ(Block.GenerationBindings.size(), 1u);
  EXPECT_EQ(Block.GenerationBindings.front().Address, EntryPC);
  EXPECT_EQ(Block.GenerationBindings.front().Size, 1u);
}

TEST(X86TranslationBlockBuilder, ClassifiesTrapFromDecoderMetadata) {
  constexpr uint64_t EntryPC = 0x7800;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 8, {0xcc});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  EXPECT_EQ(Block.Header.Terminator, TranslationBlockTerminatorKindV1::Opaque);
  ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
  EXPECT_EQ(Block.InstructionBoundaries.front().Control,
            LowInstructionControl::Terminator);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Block.InstructionBoundaries.front().ControlFlags,
      LowInstructionControlFlag::Terminator));
}

TEST(X86TranslationBlockBuilder,
     RetainsBindingsForInstructionSpanningExecutableRegions) {
  constexpr uint64_t EntryPC = 0x8000;
  std::unique_ptr<GuestMemoryRuntime> Runtime = runtimeWithRegions({
      {EntryPC,
       MemoryPermission::Read | MemoryPermission::Execute,
       11,
       {0x48, 0x83}},
      {EntryPC + 2,
       MemoryPermission::Read | MemoryPermission::Execute,
       12,
       {0xc0, 0x01, 0xc3}},
  });
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));
  ASSERT_EQ(Block.InstructionBoundaries.size(), 2u);
  EXPECT_EQ(Block.InstructionBoundaries[0].Address, EntryPC);
  EXPECT_EQ(Block.InstructionBoundaries[0].Size, 4u);
  EXPECT_EQ(Block.InstructionBoundaries[1].Address, EntryPC + 4);
  EXPECT_EQ(Block.InstructionBoundaries[1].Size, 1u);
  ASSERT_EQ(Block.GenerationBindings.size(), 2u);
  EXPECT_EQ(Block.GenerationBindings[0].Address, EntryPC);
  EXPECT_EQ(Block.GenerationBindings[0].Size, 2u);
  EXPECT_EQ(Block.GenerationBindings[0].Generation, 11u);
  EXPECT_EQ(Block.GenerationBindings[1].Address, EntryPC + 2);
  EXPECT_EQ(Block.GenerationBindings[1].Size, 3u);
  EXPECT_EQ(Block.GenerationBindings[1].Generation, 12u);
}

TEST(X86TranslationBlockBuilder, ReportsTypedTruncatedInstruction) {
  constexpr uint64_t EntryPC = 0x9000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 9, {0x48});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
      Builder->build(*Runtime, EntryPC);
  ASSERT_FALSE(static_cast<bool>(BlockOrErr));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      BlockOrErr.takeError(),
      [&](const X86TranslationBlockBuilderError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(),
                  X86TranslationBlockBuilderErrorCode::TruncatedInstruction);
        EXPECT_EQ(Error.guestPC(), EntryPC);
        ASSERT_TRUE(Error.fault().has_value());
        EXPECT_EQ(*Error.fault(), RuntimeMemoryFaultKindV1::Unmapped);
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

TEST(X86TranslationBlockBuilder, MapsFetchOverflowToGuestAddressOverflow) {
  constexpr uint64_t EntryPC = std::numeric_limits<uint64_t>::max();
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 9, {0x48});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());

  llvm::Expected<TranslationBlockDescriptorV1> BlockOrErr =
      Builder->build(*Runtime, EntryPC);
  ASSERT_FALSE(static_cast<bool>(BlockOrErr));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      BlockOrErr.takeError(),
      [&](const X86TranslationBlockBuilderError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(),
                  X86TranslationBlockBuilderErrorCode::GuestAddressOverflow);
        EXPECT_EQ(Error.guestPC(), EntryPC);
        ASSERT_TRUE(Error.fault().has_value());
        EXPECT_EQ(*Error.fault(), RuntimeMemoryFaultKindV1::AddressOverflow);
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

TEST(X86TranslationBlockBuilder, RejectsMalformedDescriptor) {
  constexpr uint64_t EntryPC = 0xa000;
  std::unique_ptr<GuestMemoryRuntime> Runtime =
      runtimeWithCode(EntryPC, 10, {0xc3});
  std::unique_ptr<X86TranslationBlockBuilder> Builder =
      llvm::cantFail(X86TranslationBlockBuilder::create());
  TranslationBlockDescriptorV1 Block =
      llvm::cantFail(Builder->build(*Runtime, EntryPC));

  TranslationBlockDescriptorV1 WrongVersion = Block;
  ++WrongVersion.Header.Version;
  expectInvalidDescriptor(validateTranslationBlockDescriptorV1(WrongVersion));

  TranslationBlockDescriptorV1 WrongSize = Block;
  ++WrongSize.Header.Size;
  expectInvalidDescriptor(validateTranslationBlockDescriptorV1(WrongSize));

  TranslationBlockDescriptorV1 UnknownFlags = Block;
  UnknownFlags.Header.Flags =
      static_cast<TranslationBlockDescriptorFlagV1>(1u << 31);
  expectInvalidDescriptor(validateTranslationBlockDescriptorV1(UnknownFlags));

  TranslationBlockDescriptorV1 InvalidLowIRSlice = Block;
  InvalidLowIRSlice.InstructionBoundaries.front().FirstOp =
      std::numeric_limits<uint64_t>::max();
  expectInvalidDescriptor(
      validateTranslationBlockDescriptorV1(InvalidLowIRSlice));

  TranslationBlockDescriptorV1 UnknownControlFlags = Block;
  UnknownControlFlags.InstructionBoundaries.front().ControlFlags |=
      LowInstructionControlFlag::NoReturn;
  expectInvalidDescriptor(
      validateTranslationBlockDescriptorV1(UnknownControlFlags));

  TranslationBlockDescriptorV1 UnexpectedImmediate = Block;
  UnexpectedImmediate.InstructionBoundaries.front().Immediate = EntryPC;
  expectInvalidDescriptor(
      validateTranslationBlockDescriptorV1(UnexpectedImmediate));

  TranslationBlockDescriptorV1 WrongMode = Block;
  WrongMode.InstructionBoundaries.front().Mode = InstructionMode::ARM;
  expectInvalidDescriptor(validateTranslationBlockDescriptorV1(WrongMode));
}

} // namespace
