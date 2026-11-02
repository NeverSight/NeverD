//===- WindowsRegistrationFrameLayoutTests.cpp - PE32 frame projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"

using namespace neverd;

TEST(WindowsRegistrationFrameLayout, RequiresPhysicalAlignmentAndBounds) {
  const RegistrationFrameCoordinate Direct{-4, 1, 0}, Aligned{-16, 64, -20};
  auto Frame = projectX86RegistrationFrame(Direct, 256, 256, 16);
  ASSERT_TRUE(Frame);
  EXPECT_EQ(Frame->Establisher, 252u);
  EXPECT_EQ(Frame->runtimeOffset(-32, 4), 220u);
  Frame = projectX86RegistrationFrame(Aligned, 256, 256, 64);
  ASSERT_TRUE(Frame);
  EXPECT_EQ(Frame->Establisher, 172u);
  EXPECT_EQ(Frame->runtimeOffset(-108, 4), 64u);
  EXPECT_EQ(Frame->runtimeOffset(-172, 4), 0u);
  EXPECT_EQ(Frame->runtimeOffset(84, 0), 256u);
  EXPECT_FALSE(Frame->runtimeOffset(-173, 4));
  EXPECT_FALSE(Frame->runtimeOffset(84, 1));
  EXPECT_FALSE(Frame->runtimeOffset(INT32_MAX, 4));
  EXPECT_FALSE(Frame->runtimeOffset(0, UINT64_MAX));
  EXPECT_FALSE(projectX86RegistrationFrame(Aligned, 256, 256, 16));
  EXPECT_FALSE(projectX86RegistrationFrame(Aligned, 256, 16, 64));
  EXPECT_FALSE(projectX86RegistrationFrame(Aligned, 256, 257, 64));
  EXPECT_FALSE(projectX86RegistrationFrame(Aligned, UINT32_MAX, 256, 64));
  EXPECT_FALSE(projectX86RegistrationFrame({-16, 3, 0}, 256, 256, 64));
  EXPECT_FALSE(projectX86RegistrationFrame({-16, 0, 0}, 256, 256, 64));
}

TEST(WindowsRegistrationFrameLayout, PreservesEveryProvedPhysicalResidue) {
  for (uint32_t Alignment = 4; Alignment <= 128; Alignment *= 2)
    for (uint32_t Offset = 256; Offset != 512; Offset += 4) {
      const RegistrationFrameCoordinate Coordinate{-16, Alignment, -20};
      const auto Layout =
          projectX86RegistrationFrame(Coordinate, 1024, Offset, Alignment);
      ASSERT_TRUE(Layout);
      for (uint32_t Base : {0x10000u, 0x7ffffc00u, 0xfffffc00u})
        EXPECT_EQ(Base + Layout->Establisher,
                  ((Base + Offset - 16) & -Alignment) - 20);
    }
  EXPECT_EQ(alignX86RegistrationOffset(-4, 64, uint32_t(-64)), -64);
  EXPECT_FALSE(alignX86RegistrationOffset(INT64_MIN, 64, uint32_t(-64)));
  EXPECT_FALSE(alignX86RegistrationOffset(4, 63, uint32_t(-32)));
}

TEST(WindowsRegistrationFrameLayout, RequiresCompleteStackAlignedCoordinates) {
  ExceptionFunction EH;
  EH.CodeRange = {0x401000, 0x401100};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.Cxx.emplace();
  auto &Chain = EH.Registration.emplace();
  Chain.RegistrationOffset = -12;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -1;
  Chain.RealignedFrame =
      RegistrationRealignedFrame{4, 0x401003, 16, 72, -72, 0};
  RegistrationStateAnalysis State;
  State.Complete = State.RegistrationLifetimeComplete =
      State.ChainOperationsComplete = true;
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto Changed = EH;
    auto Proof = State;
    auto &Frame = *Changed.Registration->RealignedFrame;
    if (Mutation == 1)
      Frame.BaseOffset -= 4;
    if (Mutation == 2)
      Frame.SavedParentFrameOffset = -20;
    if (Mutation == 3)
      ++Frame.DefinitionVA;
    if (Mutation == 4)
      Frame.Alignment = 3;
    if (Mutation == 5)
      Frame.BaseRegister = 7;
    if (Mutation == 6)
      Proof.Complete = false;
    if (Mutation == 7)
      Changed.Cxx->TryBlocks.emplace_back();
    const auto Coordinate =
        realignedRegistrationFrameCoordinate(Changed, &Proof);
    ASSERT_EQ(bool(Coordinate), Mutation == 0);
    if (Coordinate) {
      EXPECT_EQ(Coordinate->EntryOffset, -4);
      EXPECT_EQ(Coordinate->Alignment, 16u);
      EXPECT_EQ(Coordinate->AlignedOffset, 0);
    }
  }
}
