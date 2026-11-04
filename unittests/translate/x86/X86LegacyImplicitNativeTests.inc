//===- X86LegacyImplicitNativeTests.inc - Original implicit operands ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// BSWAP, XLAT and LEAVE against original protected instructions and faults.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyImplicitNativeTest : public X86LegacyScalarNativeFixture {};

TEST_P(X86LegacyImplicitNativeTest, ByteSwapEveryDwordRegisterPreservesFlags) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Reg = 0; Reg != 8; ++Reg)
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint32_t Flags : {0x202u, 0xed7u})
            for (uint32_t Value :
                 {0u, 1u, 0x80000000u, 0x12345678u, 0x80ff00a5u, UINT32_MAX}) {
              std::vector<uint8_t> Bytes{0x67, 0x64};
              if (!Code32)
                Bytes.push_back(0x66);
              Bytes.insert(Bytes.end(), {0x0f, uint8_t(0xc8 + Reg)});
              std::array<uint32_t, 8> Inputs;
              for (unsigned I = 0; I != Inputs.size(); ++I)
                Inputs[I] = Value ^ (I * 0x11111111u);
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 0,
                                              Value, 0x80, Flags, 0x4000,
                                              0x22334455, Code, Inputs));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyImplicitNativeTest, TableLookupEveryUnsignedByteIndex) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (unsigned Index = 0; Index != 256; ++Index) {
          std::vector<uint8_t> Bytes{0x66}; // Operand size has no effect.
          if (Address32 != Code32)
            Bytes.push_back(0x67);
          Bytes.push_back(0xd7);
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Index & 1, CPL, 0,
                                          0xabcd1200 | Index, 0x87654321,
                                          Index & 1 ? 0xed7 : 0x202));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyImplicitNativeTest, TableLookupSegmentAndExactByteFaults) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u})
          for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65}) {
            std::vector<uint8_t> Bytes;
            if (Address32 != Code32)
              Bytes.push_back(0x67);
            if (Prefix)
              Bytes.push_back(Prefix);
            Bytes.push_back(0xd7);
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, Case,
                                            0x12345600, 0x80, 0x40ed7));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyImplicitNativeTest, TableLookupWrapsBeforeAddingSegmentBase) {
  for (bool Code32 : {false, true})
    for (unsigned CPL = 0; CPL != 4; ++CPL)
      for (unsigned Index : {0u, 1u, 0x7fu, 0x80u, 0xffu})
        for (unsigned Case : {0u, 3u}) {
          std::array<uint32_t, 8> Inputs{0xabcdef00 | Index,
                                         0x12345678,
                                         0x98765432,
                                         0xa5a5ff80,
                                         0xd100,
                                         0xc100,
                                         0,
                                         0xc100};
          std::vector<uint8_t> Bytes{0x64};
          if (Code32)
            Bytes.push_back(0x67);
          Bytes.push_back(0xd7);
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, Case, 0,
                                          0x80, 0xed7, 0x4000, 0x22334455, Code,
                                          Inputs));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyImplicitNativeTest, LeaveWidthsFaultsAndReadOnlyFrame) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 13u, 16u})
            for (uint32_t Flags : {0x202u, 0x40ed7u}) {
              std::vector<uint8_t> Bytes{0x67, 0x64};
              if ((Width == 4) != Code32)
                Bytes.push_back(0x66);
              Bytes.push_back(0xc9);
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, Case,
                                              0x87654321, 0x80, Flags));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyImplicitNativeTest, LeaveWordStackKeepsHighESPAndWraps) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (uint32_t Frame : {0xabcdc100u, 0xabcdfffcu, 0xabcdfffeu}) {
          std::array<uint32_t, 8> Inputs{0x12345678, 0x80,  0x22334455, 0xc100,
                                         0xa5a5d100, Frame, 0,          0xc100};
          std::vector<uint8_t> Bytes;
          if ((Width == 4) != Code32)
            Bytes.push_back(0x66);
          Bytes.push_back(0xc9);
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, false, CPL, 0,
                                          0x87654321, 0x80, 0xed7, 0x4000,
                                          0x22334455, Code, Inputs));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyImplicitNativeTest, OriginalPageHandlerRepairsAndRetries) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (uint8_t Opcode : {0xc9, 0xd7})
        for (unsigned CPL = 0; CPL != 4; ++CPL) {
          std::vector<uint8_t> Bytes;
          if (!Code32 && Opcode == 0xc9)
            Bytes.push_back(0x66);
          Bytes.push_back(Opcode);
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 12,
                                          0x12345600, 0x80, 0xed7));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyImplicit, X86LegacyImplicitNativeTest,
                         testing::Values(2u, 3u));
