//===- X86LegacyStatusNativeTests.inc - Original implicit scalar state ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original sign extensions and AH/carry/direction flags across code widths.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyStatusNativeTest : public X86LegacyScalarNativeFixture {};

TEST_P(X86LegacyStatusNativeTest, ImplicitSignExtensionsPreserveFlags) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (uint8_t Opcode : {0x98, 0x99})
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (uint32_t Flags : {0x202u, 0xed7u})
              for (uint32_t Value :
                   {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu,
                    0x7fffffffu, 0x80000000u, 0xabcd0081u, UINT32_MAX}) {
                std::vector<uint8_t> Bytes{0x67, 0x64};
                if ((Width == 4) != Code32)
                  Bytes.push_back(0x66);
                Bytes.push_back(Opcode);
                ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 0,
                                                Value, 0x12345678, Flags));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStatusNativeTest, EveryAHImageAndStatusFlagPreservation) {
  for (bool Code32 : {false, true})
    for (uint8_t Opcode : {0x9e, 0x9f})
      for (unsigned AH = 0; AH != 256; ++AH)
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint32_t Flags : {0x202u, 0xed7u}) {
            std::vector<uint8_t> Bytes;
            if (Flags == 0xed7)
              Bytes = {0x66, 0x67, 0x2e};
            Bytes.push_back(Opcode);
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, AH & 1, CPL, 0,
                                            0xabcd005a | (AH << 8), 0x12345678,
                                            Flags | (AH & 0xd5)));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStatusNativeTest, CarryAndDirectionAreIndependentOfPrivilege) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (uint8_t Opcode : {0xf5, 0xf8, 0xf9, 0xfc, 0xfd})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint32_t Flags :
               {0x202u, 0x203u, 0x602u, 0x603u, 0xad6u, 0xad7u, 0xed6u, 0xed7u})
            for (bool Prefix : {false, true}) {
              std::vector<uint8_t> Bytes;
              if (Prefix)
                Bytes = {0x66, 0x67, 0x64};
              Bytes.push_back(Opcode);
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 0,
                                              0x12345678, 0x87654321, Flags));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyStatus, X86LegacyStatusNativeTest,
                         testing::Values(2u, 3u));
