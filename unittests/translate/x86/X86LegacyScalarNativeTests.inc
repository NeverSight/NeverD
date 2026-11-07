//===- X86LegacyScalarNativeTests.inc - Original protected scalars --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent KVM scalar and original page-fault-handler execution.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyScalarNativeTest : public X86LegacyScalarNativeFixture {};

TEST_P(X86LegacyScalarNativeTest, ArithmeticRegistersCarryBorrowAndFlags) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Op = 0; Op != 8; ++Op)
        for (uint32_t A : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u,
                           0xffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu})
          for (uint32_t B : {0u, 1u, 0xffffffffu, 0x80000000u})
            for (unsigned CF : {0u, 1u}) {
              std::vector<uint8_t> Bytes;
              if (Width != 1 && (Width == 4) != Code32)
                Bytes.push_back(0x66);
              Bytes.push_back(Op * 8 + (Width == 1 ? 0 : 1));
              Bytes.push_back(0xc8); // r/m = A, source = C.
              ASSERT_NO_FATAL_FAILURE(
                  compare(Bytes, Code32, true, 3, 0, A, B, 0x202 | CF));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, LoadsStoresExtensionsAndAddressing) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned Form = 0; Form != 9; ++Form) {
            std::vector<uint8_t> B;
            if (Width != 1 && (Width == 4) != Code32)
              B.push_back(0x66);
            if (Address32 != Code32)
              B.push_back(0x67);
            if (Form == 0 || Form == 1) {
              B.push_back((Form ? 0x88 : 0x8a) + (Width != 1));
              B.push_back(7); // [EDI] or [BX].
            } else if (Form == 2) {
              B.push_back(Width == 1 ? 0xc6 : 0xc7);
              B.push_back(7);
              for (unsigned I = 0; I != Width; ++I)
                B.push_back(0xa5 + I);
            } else if (Form == 3 || Form == 4) {
              B.push_back((Form == 3 ? 0xa0 : 0xa2) + (Width != 1));
              B.push_back(0);
              B.push_back(0xc1);
              if (Address32) {
                B.push_back(0);
                B.push_back(0);
              }
            } else if (Form == 5 || Form == 6) {
              if (Width == 1)
                continue;
              B.push_back(0x0f);
              B.push_back(Form == 5 ? 0xbe : 0xb6);
              B.push_back(7);
            } else {
              if (Width == 1)
                continue;
              B.push_back(Form == 7 ? 0x8d : 0x8b);
              B.push_back(Address32 ? 0x44 : 0x40);
              if (Address32)
                B.push_back(0x73); // [EBX+ESI*2+0].
              B.push_back(0);
            }
            ASSERT_NO_FATAL_FAILURE(compare(B, Code32, true, CPL));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, UnaryStackAndStackPointerAliases) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (bool Stack32 : {false, true})
        for (unsigned CPL : {0u, 3u})
          for (const std::vector<uint8_t> Body : {std::vector<uint8_t>{0x40},
                                                  {0x48},
                                                  {0xf7, 0xd0},
                                                  {0xf7, 0xd8},
                                                  {0x54},
                                                  {0x50},
                                                  {0x58},
                                                  {0x5c},
                                                  {0x6a, 0xff},
                                                  {0xff, 0x37},
                                                  {0x8f, 0x04, 0x24}}) {
            auto B = Body;
            if ((Body == std::vector<uint8_t>{0xff, 0x37} || Body[0] == 0x8f) &&
                !Code32)
              B.insert(B.begin(), 0x67);
            if (Operand32 != Code32)
              B.insert(B.begin(), 0x66);
            ASSERT_NO_FATAL_FAILURE(
                compare(B, Code32, Stack32, CPL, 0, 0x7fffffffu, 0, 0xa97));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, OperandFaultsAndUndefinedFlagsPublication) {
  for (unsigned CPL : {0u, 3u})
    for (unsigned Case : {1u, 2u, 3u, 4u, 5u, 9u, 16u})
      for (const std::vector<uint8_t> B : {std::vector<uint8_t>{0x8b, 7},
                                           {0x89, 7},
                                           {0x83, 7, 1},
                                           {0x83, 0x3f, 1},
                                           {0x83, 0x27, 0},
                                           {0xff, 0x37},
                                           {0x8f, 7},
                                           {0xf7, 0x17},
                                           {0xf7, 0x1f},
                                           {0x50}})
        ASSERT_NO_FATAL_FAILURE(
            compare(B, true, true, CPL, Case, 0x12345678, 0x80, 0xa97));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, AlignmentAndPagingPriority) {
  const char *Bound = std::getenv("NEVERD_TEST_LEGACY_ALIGNMENT");
  if (!Bound || std::strcmp(Bound, "before"))
    GTEST_SKIP() << "Bind the qualified scalar #AC-before-paging processor";
  for (unsigned Case : {7u, 8u})
    for (const std::vector<uint8_t> B : {std::vector<uint8_t>{0x8b, 7},
                                         {0x89, 7},
                                         {0x83, 7, 1},
                                         {0x83, 0x3f, 1},
                                         {0xff, 0x37},
                                         {0x8f, 7}})
      ASSERT_NO_FATAL_FAILURE(
          compare(B, true, true, 3, Case, 0x12345678, 0x80, 0x40202));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, NearBranchesCallsReturnsAndEveryCondition) {
  if (!Calls)
    GTEST_SKIP() << "Bind the processor's CALL stack publication profile";
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (bool Stack32 : {false, true}) {
        const unsigned N = Operand32 ? 4 : 2;
        for (unsigned Kind = 0; Kind != 7; ++Kind) {
          std::vector<uint8_t> B;
          if (Code32 != Operand32)
            B.push_back(0x66);
          if (Kind <= 1) {
            const uint32_t Displacement = 0x4000 - (Code + B.size() + 1 + N);
            B.push_back(Kind ? 0xe8 : 0xe9);
            for (unsigned I = 0; I != N; ++I)
              B.push_back(Displacement >> (I * 8));
          } else if (Kind <= 4) {
            B.push_back(0xff);
            B.push_back(Kind == 2 ? 0xd0 : Kind == 3 ? 0xe0 : 0x17);
          } else {
            B.push_back(Kind == 5 ? 0xc3 : 0xc2);
            if (Kind == 6) {
              B.push_back(7);
              B.push_back(0);
            }
          }
          ASSERT_NO_FATAL_FAILURE(compare(B, Code32, Stack32, 3, 11, 0x4000));
        }
        for (unsigned CC = 0; CC != 16; ++CC)
          for (unsigned Mask = 0; Mask != 32; ++Mask) {
            std::vector<uint8_t> B;
            if (Code32 != Operand32)
              B.push_back(0x66);
            const uint32_t D = 0x4000 - (Code + B.size() + 2 + N);
            B.push_back(0x0f);
            B.push_back(0x80 | CC);
            for (unsigned I = 0; I != N; ++I)
              B.push_back(D >> (I * 8));
            const unsigned Flags = 0x202 | (Mask & 1) | ((Mask & 2) << 1) |
                                   ((Mask & 4) << 4) | ((Mask & 8) << 4) |
                                   ((Mask & 16) << 7);
            ASSERT_NO_FATAL_FAILURE(
                compare(B, Code32, Stack32, 3, 0, 0x12345678, 0x80, Flags));
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, OriginalPageFaultHandlerRepairsAndReturns) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (bool Code32 : {false, true})
      for (bool Stack32 : {false, true}) {
        std::vector<uint8_t> B{0x8b, 7};
        ASSERT_NO_FATAL_FAILURE(compare(B, Code32, Stack32, CPL, 12));
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, BranchLimitsPrecedeOrFollowStackFaults) {
  if (!Calls)
    GTEST_SKIP() << "Bind the processor's CALL stack publication profile";
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (unsigned Case : {14u, 15u})
        for (const std::vector<uint8_t> Body :
             {std::vector<uint8_t>{0xff, 0xd0}, {0xff, 0xe0}, {0xc3}, {0xe8}}) {
          auto B = Body;
          if (Operand32 != Code32)
            B.insert(B.begin(), 0x66);
          if (Body[0] == 0xe8) {
            const unsigned Width = Operand32 ? 4 : 2;
            const uint32_t Relative = 0x4000 - (Code + B.size() + Width);
            for (unsigned I = 0; I != Width; ++I)
              B.push_back(Relative >> (I * 8));
          }
          ASSERT_NO_FATAL_FAILURE(compare(B, Code32, true, 3, Case, 0x4000));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, HighByteRegistersAndExtensions) {
  for (bool Code32 : {false, true})
    for (unsigned Register = 0; Register != 8; ++Register)
      for (unsigned Target = 0; Target != 8; ++Target)
        for (uint8_t Opcode : {0x00, 0x10, 0x18, 0x28, 0x38, 0x88, 0x8a})
          ASSERT_NO_FATAL_FAILURE(compare(
              {Opcode, uint8_t(0xc0 | Register << 3 | Target)}, Code32));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, WordStackWrapAndPopMemoryPostIncrement) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      std::vector<uint8_t> Push{0x54};
      if (Operand32 != Code32)
        Push.insert(Push.begin(), 0x66);
      ASSERT_NO_FATAL_FAILURE(compare(Push, Code32, false, 3, 13));
      std::vector<uint8_t> Pop{0x8f, 0x04, 0x24};
      if (!Code32)
        Pop.insert(Pop.begin(), 0x67);
      if (Operand32 != Code32)
        Pop.insert(Pop.begin(), 0x66);
      ASSERT_NO_FATAL_FAILURE(compare(Pop, Code32, false, 3, 17));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, ShortBranchesAndImmediateEncodings) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      const unsigned N = Operand32 ? 4 : 2;
      for (unsigned CC = 0; CC != 17; ++CC)
        for (unsigned Flags : {0x202u, 0xad7u}) {
          std::vector<uint8_t> B;
          if (Operand32 != Code32)
            B.push_back(0x66);
          const unsigned Relative = 0x40 - B.size() - 2;
          B.push_back(CC == 16 ? 0xeb : 0x70 | CC);
          B.push_back(Relative);
          ASSERT_NO_FATAL_FAILURE(compare(B, Code32, true, 3, 0, 0x12345678,
                                          0x80, Flags, Code + 0x40));
        }
      for (unsigned Op = 0; Op != 8; ++Op)
        for (uint32_t Value : {0u, 1u, 0x7fu, 0x80u, 0xffu})
          for (unsigned Form = 0; Form != 3; ++Form) {
            std::vector<uint8_t> B;
            if (Operand32 != Code32)
              B.push_back(0x66);
            B.push_back(Form == 0 ? Op * 8 + 5 : Form == 1 ? 0x81 : 0x83);
            if (Form)
              B.push_back(0xc0 | Op << 3);
            for (unsigned I = 0; I != (Form == 2 ? 1 : N); ++I)
              B.push_back(Value >> (I * 8));
            ASSERT_NO_FATAL_FAILURE(
                compare(B, Code32, true, 3, 0, 0x80000000, 0x80, 0x203));
          }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyScalars, X86LegacyScalarNativeTest,
                         testing::Values(2u, 3u));
