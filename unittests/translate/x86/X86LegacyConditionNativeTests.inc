//===- X86LegacyConditionNativeTests.inc - Original condition execution --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original conditional operands, loop widths and restartable faults.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyConditionNativeTest : public X86LegacyScalarNativeFixture {};

TEST_P(X86LegacyConditionNativeTest, AllConditionsAndFlagImages) {
  for (bool Code32 : {false, true})
    for (unsigned CC = 0; CC != 16; ++CC)
      for (unsigned Image = 0; Image != 32; ++Image) {
        const uint32_t Flags = 0x612 | (Image & 1) | ((Image & 2) << 1) |
                               ((Image & 4) << 4) | ((Image & 8) << 4) |
                               ((Image & 16) << 7);
        ASSERT_NO_FATAL_FAILURE(
            compare({0x66, 0x67, 0x64, 0x0f, uint8_t(0x90 | CC), 0xc4}, Code32,
                    Image & 1, Image & 3, 0, 0x12345678, 0x87654321, Flags));
        for (bool Operand32 : {false, true}) {
          std::vector<uint8_t> Bytes;
          if (Operand32 != Code32)
            Bytes.push_back(0x66);
          Bytes.insert(Bytes.end(), {0x0f, uint8_t(0x40 | CC), 0xc1});
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Image & 1, Image & 3,
                                          0, 0x12345678, 0x87654321, Flags));
        }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyConditionNativeTest, RegistersAliasesAndIgnoredSetRegField) {
  for (bool Code32 : {false, true})
    for (unsigned Destination = 0; Destination != 8; ++Destination)
      for (unsigned Source = 0; Source != 8; ++Source)
        for (uint32_t Flags : {0x202u, 0xed7u}) {
          ASSERT_NO_FATAL_FAILURE(
              compare({0x0f, 0x94, uint8_t(0xc0 | Source << 3 | Destination)},
                      Code32, true, 3, 0, 0x12345678, 0x87654321, Flags));
          for (bool Operand32 : {false, true}) {
            std::vector<uint8_t> Bytes;
            if (Operand32 != Code32)
              Bytes.push_back(0x66);
            Bytes.insert(
                Bytes.end(),
                {0x0f, 0x44, uint8_t(0xc0 | Destination << 3 | Source)});
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, 3, 0,
                                            0x12345678, 0x87654321, Flags));
          }
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyConditionNativeTest, MemoryConditionsRetainAccessAndFaults) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned CC : {0u, 4u, 6u, 12u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
            for (uint32_t Flags : {0x40202u, 0x40ed7u}) {
              std::vector<uint8_t> Prefix;
              if (Address32 != Code32)
                Prefix.push_back(0x67);
              if (Case == 6)
                Prefix.push_back(0x36);
              const uint8_t ModRM = Address32 ? 3 : 7; // [EBX] / [BX].
              auto Set = Prefix;
              Set.insert(Set.end(), {0x66, 0x0f, uint8_t(0x90 | CC), ModRM});
              ASSERT_NO_FATAL_FAILURE(compare(Set, Code32, true, CPL, Case,
                                              0x12345678, 0x87654321, Flags));
              for (bool Operand32 : {false, true}) {
                auto Move = Prefix;
                if (Operand32 != Code32)
                  Move.push_back(0x66);
                Move.insert(Move.end(), {0x0f, uint8_t(0x40 | CC), ModRM});
                ASSERT_NO_FATAL_FAILURE(compare(Move, Code32, true, CPL, Case,
                                                0x12345678, 0x87654321, Flags));
              }
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyConditionNativeTest,
       OriginalPageRepairRetriesConditionalAccess) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (uint32_t Flags : {0x202u, 0xed7u})
          for (uint8_t Opcode : {0x44, 0x94}) {
            std::vector<uint8_t> Bytes;
            if (Address32 != Code32)
              Bytes.push_back(0x67);
            Bytes.insert(Bytes.end(),
                         {0x0f, Opcode, uint8_t(Address32 ? 3 : 7)});
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, 12,
                                            0x12345678, 0x87654321, Flags));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyConditionNativeTest, LoopCountsWidthsFlagsAndDirections) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (bool Address32 : {false, true})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint32_t Count : {0u, 1u, 2u, 0xffffu, 0x10000u, 0x10001u,
                                 0xabcd0000u, UINT32_MAX})
            for (uint8_t Opcode : {0xe0, 0xe1, 0xe2, 0xe3})
              for (uint32_t Flags : {0x202u, 0xed7u})
                for (int Direction : {-1, 1}) {
                  std::vector<uint8_t> Bytes;
                  if (Operand32 != Code32)
                    Bytes.push_back(0x66);
                  if (Address32 != Code32)
                    Bytes.push_back(0x67);
                  const int Displacement = Direction * 0x40 - Bytes.size() - 2;
                  Bytes.insert(Bytes.end(), {Opcode, uint8_t(Displacement)});
                  ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Count & 1, CPL,
                                                  0, 0x12345678, Count, Flags,
                                                  Code + Direction * 0x40));
                }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyConditionNativeTest, LoopHighPCAndTargetFaults) {
  for (uint32_t PC : {uint32_t(Code), 0x21000u})
    for (bool Code32 : {false, true})
      for (bool Operand32 : {false, true})
        for (bool Address32 : {false, true})
          for (uint8_t Opcode : {0xe0, 0xe1, 0xe2, 0xe3})
            for (uint32_t Count : {0u, 2u, 0x10000u})
              for (uint32_t Flags : {0x202u, 0xed7u})
                for (unsigned Case : {0u, 19u}) {
                  std::vector<uint8_t> Bytes;
                  if (Operand32 != Code32)
                    Bytes.push_back(0x66);
                  if (Address32 != Code32)
                    Bytes.push_back(0x67);
                  const unsigned Displacement = 0x40 - Bytes.size() - 2;
                  Bytes.insert(Bytes.end(), {Opcode, uint8_t(Displacement)});
                  const uint32_t Target =
                      Operand32 ? PC + 0x40 : uint16_t(PC + 0x40);
                  ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, 3, Case,
                                                  0x12345678, Count, Flags,
                                                  Target, 0x22334455, PC));
                }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyConditions, X86LegacyConditionNativeTest,
                         testing::Values(2u, 3u));
