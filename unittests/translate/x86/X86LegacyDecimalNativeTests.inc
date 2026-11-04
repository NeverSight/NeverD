//===- X86LegacyDecimalNativeTests.inc - Original decimal CPU state ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent original bytes across both protected code widths and paging
/// formats. Only architecturally undefined bits are supplied by the CPU;
/// every defined result, fault frame and unrelated register remains checked.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyDecimalNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void bind(uint8_t Opcode) {
    DecimalUndefinedMask = Opcode == 0x37 || Opcode == 0x3f   ? 0x8c4
                           : Opcode == 0x27 || Opcode == 0x2f ? 0x800
                                                              : 0x811;
  }
};

TEST_P(X86LegacyDecimalNativeTest, PackedEveryALAndBothCarryInputs) {
  for (bool Code32 : {false, true})
    for (uint8_t Opcode : {0x27, 0x2f}) {
      bind(Opcode);
      for (unsigned AL = 0; AL != 256; ++AL)
        for (uint32_t Carry : {0u, 1u, 0x10u, 0x11u})
          ASSERT_NO_FATAL_FAILURE(
              compare({Opcode}, Code32, true, 3, 0, 0xabcd1200 | AL, 0x12345678,
                      0x602 | Carry | ((AL & 1) ? 0x8c4 : 0)));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDecimalNativeTest, AsciiEveryALAndHighByteCarryBoundaries) {
  for (bool Code32 : {false, true})
    for (uint8_t Opcode : {0x37, 0x3f}) {
      bind(Opcode);
      for (unsigned AH : {0u, 1u, 0x7fu, 0xffu})
        for (unsigned AL = 0; AL != 256; ++AL)
          for (uint32_t AF : {0u, 0x10u})
            ASSERT_NO_FATAL_FAILURE(compare(
                {Opcode}, Code32, true, 3, 0, 0xabcd0000 | (AH << 8) | AL,
                0x12345678, 0x602 | AF | ((AL & 1) ? 0x8c5 : 0)));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDecimalNativeTest, EveryImmediateBaseUsesOriginalAX) {
  for (bool Code32 : {false, true})
    for (uint8_t Opcode : {0xd4, 0xd5}) {
      bind(Opcode);
      for (unsigned Base = Opcode == 0xd4 ? 1 : 0; Base != 256; ++Base)
        for (unsigned AX :
             {0u, 1u, 0x007fu, 0x00ffu, 0x0100u, 0x7f80u, 0xff01u, 0xffffu})
          ASSERT_NO_FATAL_FAILURE(compare({Opcode, uint8_t(Base)}, Code32, true,
                                          3, 0, 0xabcd0000 | AX, 0x12345678,
                                          (Base & 1) ? 0xed7 : 0x202));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDecimalNativeTest, EveryALAcrossBinaryAndDecimalBases) {
  for (uint8_t Opcode : {0xd4, 0xd5}) {
    bind(Opcode);
    for (uint8_t Base : {1, 2, 7, 10, 16, 128, 255})
      for (unsigned AL = 0; AL != 256; ++AL)
        ASSERT_NO_FATAL_FAILURE(compare({Opcode, Base}, AL & 1, true, 3, 0,
                                        0xabcd0000 | ((255 - AL) << 8) | AL,
                                        0x12345678, (AL & 2) ? 0xed7 : 0x202));
  }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDecimalNativeTest, IgnoredPrefixesAndAllPrivileges) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (uint8_t Opcode : {0x27, 0x2f, 0x37, 0x3f, 0xd4, 0xd5}) {
          bind(Opcode);
          for (uint8_t Segment : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65}) {
            std::vector<uint8_t> Bytes{0x66, 0x67};
            if (Segment)
              Bytes.push_back(Segment);
            Bytes.push_back(Opcode);
            if (Opcode >= 0xd4)
              Bytes.push_back(13);
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 5,
                                            0xabcdfffe, 0x12345678, 0xed7));
          }
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDecimalNativeTest, ZeroAAMFaultAndOriginalHandlerRetry) {
  bind(0xd4);
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (uint32_t Flags : {0x202u, 0xed7u})
          for (unsigned Case : {0u, 20u})
            for (bool Prefix : {false, true}) {
              std::vector<uint8_t> Bytes;
              if (Prefix)
                Bytes = {0x66, 0x67, 0x65};
              Bytes.insert(Bytes.end(), {0xd4, 0});
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, Case,
                                              0xabcd1295, 0x12345678, Flags));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyDecimal, X86LegacyDecimalNativeTest,
                         testing::Values(2u, 3u));
