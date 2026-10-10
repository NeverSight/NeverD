//===- WindowsRegistrationFrameLayoutTests.cpp - PE32 frame projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

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
