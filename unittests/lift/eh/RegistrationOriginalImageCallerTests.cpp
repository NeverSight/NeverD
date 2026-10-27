//===- RegistrationOriginalImageCallerTests.cpp - Scoped source inputs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind layout-dependent SEH results to actual authored caller invocations.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationOriginalImage.h"
#include "RegistrationSEH4SharedTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationSEHBoundCall.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace {
using namespace neverd;
using namespace neverd::registration_state;

BinaryImage callerImage(uint32_t Base, uint16_t Count) {
  auto Image = registration_test::sharedSEH4Image(Base, false);
  auto &Text = Image.Segments.front().Data;
  std::copy_n(Text.begin(), 12, Text.begin() + 0x300);
  writeLE<uint32_t>(Text.data() + 0x308, uint32_t(0x100 - 0x30c));
  const uint8_t Callee[] = {
      0x8b, 0x45, 8,    0x0f, 0xb7, 0, // Read the actual image argument.
      0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0xe8, 0, 0, 0, 0, 0xc3};
  std::copy(std::begin(Callee), std::end(Callee), Text.begin() + 0x30c);
  writeLE<uint32_t>(Text.data() + 0x31a, uint32_t(0x150 - 0x31e));
  std::fill(Text.begin() + 12, Text.begin() + 0x80, 0xcc);
  const uint8_t Parent[] = {
      0x68, 0,    0,    0,    0, // PUSH original header address.
      0xe8, 0,    0,    0,    0, // Invoke the retained SEH helper.
      0x83, 0xc4, 4,    0x83, 0xf8, 5,
      0x74, 2,    0x89, 0x28, // Another result exposes the private EBP.
      0xe8, 0,    0,    0,    0,    0xc3};
  std::copy(std::begin(Parent), std::end(Parent), Text.begin() + 12);
  writeLE<uint32_t>(Text.data() + 13, Base + 0x86);
  writeLE<uint32_t>(Text.data() + 18, uint32_t(0x300 - 22));
  writeLE<uint32_t>(Text.data() + 33, uint32_t(0x150 - 37));
  Image.BaseRelocations.push_back(
      {Base + 0x100d, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
  Image.DataAddressRelocOperands[Base + 0x100d] = {Base + 0x86, Base + 0x86, 4,
                                                   Base};
  Image.Symbols.push_back(Symbol::makeFunc(Base + 0x1300));
  Segment Header;
  Header.VA = Base;
  Header.Size = Header.FileSz = 0x400;
  Header.Flags = SegmentFlags::Readable;
  Header.Data.resize(Header.Size);
  writeLE<uint16_t>(Header.Data.data(), 0x5a4d);
  writeLE<uint32_t>(Header.Data.data() + 0x3c, 0x80);
  writeLE<uint32_t>(Header.Data.data() + 0x80, 0x4550);
  writeLE<uint16_t>(Header.Data.data() + 0x86, Count);
  writeLE<uint16_t>(Header.Data.data() + 0x88, Count);
  Section HeaderSection;
  HeaderSection.VA = Base;
  HeaderSection.Size = HeaderSection.FileSz = Header.Size;
  HeaderSection.Flags = Header.Flags;
  Image.Segments.push_back(std::move(Header));
  Image.Sections.push_back(std::move(HeaderSection));
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  return Image;
}

TEST(RegistrationOriginalImageCaller, ActualCellsOwnEveryDependentFact) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (uint16_t Count : {uint16_t(5), uint16_t(6)}) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Count);
      auto Image = callerImage(Base, Count);
      auto Low = registration_test::liftSharedSEH4(Image);
      ASSERT_TRUE(Low.RegistrationStates);
      EXPECT_FALSE(Low.RegistrationStates->Complete);
      size_t Work = 0;
      const auto Proof = analyzeOriginalImageSEHCaller(Image, Low, Work);
      EXPECT_EQ(Proof.States.Complete, Count == 5);
      EXPECT_EQ(Proof.InvocationsComplete, Count == 5);
      ASSERT_EQ(Proof.Invocations.size(), 1u);
      const auto &Invocation = Proof.Invocations.begin()->second;
      EXPECT_EQ(Invocation.Call.Address, Base + 0x1011);
      EXPECT_EQ(Invocation.Call.TargetVA, Base + 0x1300);
      ASSERT_EQ(Invocation.Arguments.size(), 1u);
      EXPECT_EQ(Invocation.Arguments.begin()->first,
                Invocation.Call.StackOffset);
      EXPECT_EQ(Invocation.Arguments.begin()->second,
                FrameValue::imageAddress(Base + 0x86));
      EXPECT_EQ(Invocation.ReturnValue, Count);
      EXPECT_FALSE(Invocation.Call.ConstantReturn);
      EXPECT_FALSE(Low.RegistrationStates->Complete);
      const auto Ordinary = registration_test::liftSharedSEH4(Image);
      ASSERT_TRUE(Ordinary.RegistrationStates);
      EXPECT_FALSE(Ordinary.RegistrationStates->Complete);
      if (Count == 5) {
        // Even moving these scoped facts by hand cannot authorize the
        // observed native result without a separate invocation consumer.
        Low.RegistrationStates = Proof.States;
        const auto Med =
            LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
        EXPECT_FALSE(getX86RegistrationSEHBoundCalls(Med));
      }
    }
}

TEST(RegistrationOriginalImageCaller, FreshSourceInputsKeepTheirOwnIdentity) {
  for (uint32_t Base : {0x400000u, 0x18000000u})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Base << " " << Mutation);
      auto Image = callerImage(Base, 5);
      auto &Text = Image.Segments.front().Data;
      if (Mutation == 0) {
        writeLE<uint32_t>(Text.data() + 13, Base + 0x88);
        Image.DataAddressRelocOperands[Base + 0x100d] = {Base + 0x88,
                                                         Base + 0x88, 4, Base};
      } else if (Mutation == 1) {
        Image.BaseRelocations.clear();
        Image.DataAddressRelocOperands.clear();
      } else {
        std::fill(Text.begin() + 12, Text.begin() + 17, 0x90);
        Image.BaseRelocations.clear();
        Image.DataAddressRelocOperands.clear();
        if (Mutation == 3)
          Text[12] = 0x50; // PUSH an unknown EAX, not the former image value.
        if (Mutation == 4) {
          Text[12] = 0x66;
          Text[13] = 0x68; // A two-byte PUSH cannot initialize an i32 input.
          Text[14] = 0x86;
          Text[15] = 0;
        }
      }
      Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(Image);
      const auto Low = registration_test::liftSharedSEH4(Image);
      size_t Work = 0;
      const auto Proof = analyzeOriginalImageSEHCaller(Image, Low, Work);
      EXPECT_EQ(Proof.InvocationsComplete, Mutation == 0);
      if (Mutation != 0) {
        EXPECT_TRUE(Proof.Invocations.empty());
        continue;
      }
      ASSERT_EQ(Proof.Invocations.size(), 1u);
      const auto &Invocation = Proof.Invocations.begin()->second;
      ASSERT_EQ(Invocation.Arguments.size(), 1u);
      EXPECT_EQ(Invocation.Arguments.begin()->second,
                FrameValue::imageAddress(Base + 0x88));
      EXPECT_EQ(Invocation.ReturnValue, 5u);
      EXPECT_FALSE(Invocation.Call.ConstantReturn);
      EXPECT_TRUE(Invocation.ReadAddressIdentities.at({Base + 0x88, 2}));
    }
}

TEST(RegistrationOriginalImageCaller, PlansAndPropagationShareOneBudget) {
  auto Image = callerImage(0x400000, 5);
  const auto Low = registration_test::liftSharedSEH4(Image);
  size_t Required = 0;
  ASSERT_TRUE(
      analyzeOriginalImageSEHCaller(Image, Low, Required).InvocationsComplete);
  ASSERT_GT(Required, 0u);
  for (size_t Available : {size_t{0}, Required - 1, Required}) {
    size_t Work = limits::kMaxRegistrationEHStateWork - Available;
    const auto Proof = analyzeOriginalImageSEHCaller(Image, Low, Work);
    EXPECT_EQ(Proof.InvocationsComplete, Available == Required);
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationOriginalImageCaller, ChangedInputsRevokeOnePhysicalRecipe) {
  for (uint32_t Base : {0x400000u, 0x18000000u}) {
    auto Image = callerImage(Base, 5);
    auto &Text = Image.Segments.front().Data;
    // Revisit the same physical CALL with a different exact image argument.
    // The two words happen to contain the same value; that coincidence
    // cannot merge their invocation identities.
    Text[25] = 0x68;
    writeLE<uint32_t>(Text.data() + 26, Base + 0x88);
    Text[30] = 0xeb;
    Text[31] = uint8_t(17 - 32);
    Image.BaseRelocations.push_back(
        {Base + 0x101a, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    Image.DataAddressRelocOperands[Base + 0x101a] = {Base + 0x88, Base + 0x88,
                                                     4, Base};
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto Low = registration_test::liftSharedSEH4(Image);
    size_t Work = 0;
    const auto Proof = analyzeOriginalImageSEHCaller(Image, Low, Work);
    EXPECT_FALSE(Proof.InvocationsComplete);
    EXPECT_FALSE(Proof.Invocations.empty());
  }
}

TEST(RegistrationOriginalImageCaller, EarlierOrdinaryVisitCannotDisappear) {
  for (uint32_t Base : {0x400000u, 0x18000000u}) {
    auto Image = callerImage(Base, 5);
    auto &Text = Image.Segments.front().Data;
    // The helper returns its argument. The first invocation returns an image
    // identity, so only the ordinary proof can describe it. A later visit at
    // a distinct SEH state returns scalar 37 from the same physical CALL.
    std::fill(Text.begin() + 0x30f, Text.begin() + 0x312, 0x90);
    const uint8_t Tail[] = {0x83,
                            0x7d,
                            0xfc,
                            0xfe,
                            0x75,
                            11,
                            0xc7,
                            0x45,
                            0xfc,
                            0,
                            0,
                            0,
                            0,
                            0x6a,
                            37,
                            0xeb,
                            uint8_t(17 - 42),
                            0xc7,
                            0x45,
                            0xfc,
                            0xfe,
                            0xff,
                            0xff,
                            0xff,
                            0xe8,
                            0,
                            0,
                            0,
                            0,
                            0xc3};
    std::copy(std::begin(Tail), std::end(Tail), Text.begin() + 25);
    writeLE<uint32_t>(Text.data() + 50, uint32_t(0x150 - 54));
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto Low = registration_test::liftSharedSEH4(Image);
    size_t Work = 0;
    const auto Proof = analyzeOriginalImageSEHCaller(Image, Low, Work);
    ASSERT_TRUE(Proof.States.Complete);
    ASSERT_EQ(Proof.Invocations.size(), 1u);
    EXPECT_EQ(Proof.Invocations.begin()->second.ReturnValue, 37u);
    EXPECT_FALSE(Proof.InvocationsComplete);
  }
}
} // namespace
