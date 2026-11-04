//===- X86LegacyArplNativeTests.inc - Original protected RPL adjustment ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original ARPL register aliases, conditional accesses and restart frames.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyArplNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    CheckOperandPageEffects = true;
  }
};

TEST_P(X86LegacyArplNativeTest, RegisterAliasesPreserveEveryOtherBit) {
  for (bool Code32 : {false, true})
    for (bool Override : {false, true})
      for (unsigned Destination = 0; Destination != 8; ++Destination)
        for (unsigned Source = 0; Source != 8; ++Source)
          for (unsigned DestRPL = 0; DestRPL != 4; ++DestRPL)
            for (unsigned SourceRPL = 0; SourceRPL != 4; ++SourceRPL)
              for (uint32_t Flags : {0x202u, 0xed7u}) {
                std::vector<uint8_t> Bytes{0x67, 0x65};
                if (Override)
                  Bytes.push_back(0x66);
                Bytes.insert(Bytes.end(),
                             {0x63, uint8_t(0xc0 | Source << 3 | Destination)});
                std::array<uint32_t, 8> Registers{};
                for (unsigned I = 0; I != 8; ++I)
                  Registers[I] = 0xabcd0000u | (I << 4) | DestRPL;
                Registers[Source] = 0xfedc8000u | (Source << 4) | SourceRPL;
                ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, 3, 0, 0, 0,
                                                Flags, 0x4000, 0, Code,
                                                Registers));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyArplNativeTest,
       MemoryRPLUsesWordAndAddressWidthsIndependently) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Override : {false, true})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint8_t Segment : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
            for (unsigned DestRPL = 0; DestRPL != 4; ++DestRPL)
              for (unsigned SourceRPL = 0; SourceRPL != 4; ++SourceRPL) {
                std::vector<uint8_t> Bytes;
                if (Override)
                  Bytes.push_back(0x66);
                if (Code32 != Address32)
                  Bytes.push_back(0x67);
                if (Segment)
                  Bytes.push_back(Segment);
                Bytes.insert(Bytes.end(),
                             {0x63, uint8_t(Address32 ? 0x0b : 0x0f)});
                ASSERT_NO_FATAL_FAILURE(compare(
                    Bytes, Code32, true, CPL,
                    Segment && Segment != 0x2e && Segment != 0x36 ? 3 : 0,
                    0x55aaabc0 | DestRPL, 0xfedc8000 | SourceRPL,
                    (DestRPL + SourceRPL) & 1 ? 0x202 : 0xed7));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyArplNativeTest,
       ReadFaultsPrecedeConditionalWriteFaultsAndRetry) {
  constexpr unsigned Cases[]{1, 2, 4, 5, 7, 8, 16, 12, 0, 1, 0, 0};
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Override : {false, true})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (bool Adjust : {false, true})
            for (unsigned Failure = 0; Failure != std::size(Cases); ++Failure) {
              std::vector<uint8_t> Bytes;
              if (Override)
                Bytes.push_back(0x66);
              if (Code32 != Address32)
                Bytes.push_back(0x67);
              Bytes.insert(Bytes.end(),
                           {0x63, uint8_t(Address32 ? 0x0b : 0x0f)});
              const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
                if (Failure == 8 || Failure == 9)
                  Special.ds.type &= ~2u;
                if (Failure == 10 || Failure == 11) {
                  Special.ds.g = 0;
                  Special.ds.limit = Failure == 10 ? 0xc100 : 0xc0ff;
                  if (Failure == 11)
                    Special.ds.type = 5; // Read-only expand-down segment.
                }
              };
              ASSERT_NO_FATAL_FAILURE(
                  compare(Bytes, Code32, true, CPL, Cases[Failure],
                          0x55aaabc0 | (Adjust ? 1 : 3), 0xfedc8003,
                          Failure == 4 || Failure == 5 ? 0x40202
                          : Adjust                     ? 0xe97
                                                       : 0xed7,
                          0x4000, 0, Code, {}, Configure));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyArplNativeTest, FinalMappedWordNeverTouchesTheNextPage) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (bool Override : {false, true})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (bool Adjust : {false, true}) {
            std::vector<uint8_t> Bytes;
            if (Override)
              Bytes.push_back(0x66);
            if (Code32 != Address32)
              Bytes.push_back(0x67);
            Bytes.insert(Bytes.end(), {0x63, uint8_t(Address32 ? 0x0b : 0x0f)});
            const std::array<uint32_t, 8> Registers{
                0x12345678, 0xfedc8003, 0x22334455, 0xcffe,
                0xd100,     0xc100,     0,          0xc100};
            const auto Configure = [&](kvm_sregs &, kvm_regs &) {
              put(0xcffc, 0xabc0cafe | (Adjust ? 1u : 3u) << 16, 4);
              const unsigned PW = GetParam() == 2 ? 4 : 8;
              const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
              put(PT + 13 * PW, 0xd006, PW);
            };
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, 0, 0, 0,
                                            0xed7, 0x4000, 0, Code, Registers,
                                            Configure));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyArpl, X86LegacyArplNativeTest,
                         testing::Values(2u, 3u));
