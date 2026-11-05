//===- X86LegacyStringNativeTests.inc - Original segmented strings -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native REP widths, segments, partial faults, repairs and IRQ/NMI boundaries.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyStringNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    ExtraMemoryChecks = {{0xc000, 512},  {0xe000, 512},  {0xcfe0, 64},
                         {0xefe0, 64},   {0x1c000, 512}, {0x1e000, 512},
                         {0x3c000, 512}, {0x3e000, 512}};
  }

  static std::vector<uint8_t> form(unsigned Kind, unsigned Width, bool Code32,
                                   bool Address32, uint8_t Segment = 0) {
    std::vector<uint8_t> Bytes;
    if (Kind < 7)
      Bytes.push_back(Kind == 4 || Kind == 6 ? 0xf2 : 0xf3);
    if ((Width == 4) != Code32 && Width != 1)
      Bytes.push_back(0x66);
    if (Address32 != Code32)
      Bytes.push_back(0x67);
    if (Segment)
      Bytes.push_back(Segment);
    const unsigned Operation = Kind >= 7   ? Kind - 7
                               : Kind == 4 ? 3
                               : Kind >= 5 ? 4
                                           : Kind;
    constexpr uint8_t Opcodes[]{0xa4, 0xaa, 0xac, 0xa6, 0xae};
    Bytes.push_back(Opcodes[Operation] | unsigned(Width != 1));
    return Bytes;
  }

  void string(unsigned Kind, unsigned Width, bool Code32, bool Address32,
              unsigned CPL, uint32_t Count, bool Backward,
              unsigned Scenario = 0, uint8_t Prefix = 0) {
    SCOPED_TRACE(testing::Message()
                 << "string kind=" << Kind << " width=" << Width
                 << " address32=" << Address32 << " count=" << Count
                 << " scenario=" << Scenario << " prefix=" << unsigned(Prefix));
    const bool DestinationOnly =
        Kind == 1 || Kind == 5 || Kind == 6 || Kind == 8 || Kind == 11;
    const bool NotEqual = Kind == 4 || Kind == 6;
    uint32_t Source = 0xc080, Destination = 0xe080;
    if (Scenario == 1 || Scenario == 2 || Scenario == 3) {
      if (DestinationOnly)
        Destination = Backward ? 0xd000 + Width : 0xd000 - 2 * Width;
      else
        Source = Backward ? 0xd000 + Width : 0xd000 - 2 * Width;
    }
    if (Scenario == 4) // Competing faults in both operands.
      Source = 0xc100, Destination = 0xe100;
    if (Scenario == 9 || Scenario == 10)
      Source = 0xc101, Destination = 0xe101;
    if (Scenario == 11 || Scenario == 13) // Fault within one element.
      Source = 0xcfff, Destination = 0xefff;
    const uint32_t High = Address32 ? 0 : 0x12340000;
    std::array<uint32_t, 8> Inputs{0x11111111,
                                   High | Count,
                                   0x87654321,
                                   0xabcdef01,
                                   Scenario == 13 ? 0xb800u : 0xf800u,
                                   0x23456789,
                                   High | Source,
                                   (High << 1) | Destination};
    uint32_t Flags = 0xad7 | (Backward ? 0x400 : 0);
    if (Scenario == 9 || Scenario == 10)
      Flags |= 0x40000;
    const unsigned Case = Scenario == 3 ? 23 : 0;
    auto Bytes = form(Kind, Width, Code32, Address32, Prefix);
    ASSERT_NO_FATAL_FAILURE(compare(
        Bytes, Code32, !Backward, CPL, Case, 0x11111111, Count, Flags, 0x4000,
        0x87654321, Code, Inputs, [&](kvm_sregs &Special, kvm_regs &) {
          const unsigned PW = GetParam() == 2 ? 4 : 8;
          const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
          for (unsigned Base : {0u, 0x10000u, 0x30000u}) {
            std::fill(RAM + Base + 0xc000, RAM + Base + 0xc000 + 0x1000, 0x11);
            std::fill(RAM + Base + 0xe000, RAM + Base + 0xe000 + 0x1000,
                      NotEqual ? 0x22 : 0x11);
          }
          std::fill(RAM + Source - 64, RAM + Source + 64, 0x11);
          std::fill(RAM + Destination - 64, RAM + Destination + 64,
                    NotEqual ? 0x22 : 0x11);
          if (Scenario == 1 || Scenario == 3) {
            const uint32_t Page = Backward ? 0xc000 : 0xd000;
            put(PT + (Page >> 12) * PW, Page | 6, PW);
          }
          if (Scenario == 2) {
            auto &Cache = DestinationOnly ? Special.es : Special.ds;
            Cache.g = 0;
            if (Backward) {
              Cache.type |= 4; // Expand down: cfff and below are invalid.
              Cache.limit = 0xcfff;
            } else {
              Cache.limit = 0xcfff;
            }
          }
          if (Scenario == 4 || Scenario == 10) {
            put(PT + 12 * PW, 0xc006, PW);
            put(PT + 14 * PW, 0xe006, PW);
          }
          if (Scenario == 5) {
            Special.ds.base = Special.fs.base = Special.gs.base = 0x10000;
            Special.es.base = 0x30000;
          }
          if (Scenario == 6) {
            Special.ds.unusable = Special.es.unusable = Special.fs.unusable =
                Special.gs.unusable = 1;
          }
          if (Scenario == 7) {
            Special.ds.type &= ~2u;
            Special.es.type &= ~2u;
          }
          if (Scenario == 8) {
            put(PT + 12 * PW, 0xc005, PW);
            put(PT + 14 * PW, 0xe005, PW);
          }
          if (Scenario == 11) {
            Special.ds.g = Special.es.g = 0;
            Special.ds.limit = 0xcfff;
            Special.es.limit = 0xefff;
          }
          if (Scenario == 13) {
            put(PT + 13 * PW, 0xd006, PW);
            put(PT + 15 * PW, 0xf006, PW);
          }
          if (Scenario == 12 && Kind >= 3 && Kind < 7) {
            const uint32_t End =
                Backward ? Destination - Width : Destination + Width;
            std::fill(RAM + End, RAM + End + Width, NotEqual ? 0x11 : 0x22);
          }
        }));
  }
};

TEST_P(X86LegacyStringNativeTest, WidthsCountsFlagsAndConditionalTermination) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Kind = 0; Kind != 12; ++Kind)
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (bool Backward : {false, true})
              for (unsigned Count : {0u, 1u, 4u})
                ASSERT_NO_FATAL_FAILURE(string(Kind, Width, Code32, Address32,
                                               CPL, Count, Backward, 12));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, PartialFaultsAndOriginalPageRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Kind = 0; Kind != 7; ++Kind)
          for (unsigned CPL : {0u, 3u})
            for (bool Backward : {false, true})
              for (unsigned Scenario : {1u, 2u, 3u}) {
                if (Scenario == 3 && Backward)
                  continue; // Shared repair handler owns the d000 PTE.
                ASSERT_NO_FATAL_FAILURE(string(Kind, Width, Code32, Address32,
                                               CPL, 4, Backward, Scenario));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, SegmentPermissionsAndCompetingFaults) {
  const char *Bound = std::getenv("NEVERD_TEST_LEGACY_ALIGNMENT");
  if (!Bound || std::strcmp(Bound, "before"))
    GTEST_SKIP() << "Bind the qualified string #AC-before-paging processor";
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Kind = 0; Kind != 7; ++Kind)
          for (unsigned CPL : {0u, 3u})
            for (unsigned Scenario : {4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u})
              for (uint8_t Prefix : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
                ASSERT_NO_FATAL_FAILURE(string(Kind, Width, Code32, Address32,
                                               CPL, 4, false, Scenario,
                                               Prefix));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, ZeroCountSuppressesUnusableSegments) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned Kind = 0; Kind != 7; ++Kind)
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            ASSERT_NO_FATAL_FAILURE(
                string(Kind, Width, Code32, Address32, CPL, 0, false, 6));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, WordIndicesWrapBeforeAddingSegmentBases) {
  ExtraMemoryChecks = {
      {0x3ffe0, 64}, {0x30000, 32}, {0x5ffe0, 64}, {0x50000, 32}};
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Kind = 0; Kind != 7; ++Kind)
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (bool Backward : {false, true}) {
            const uint32_t Offset = Backward ? 0 : 0x10000 - Width;
            const std::array<uint32_t, 8> Inputs{0x11111111,
                                                 0xabcd0003,
                                                 0x87654321,
                                                 0xabcdef01,
                                                 0xf800,
                                                 0x23456789,
                                                 0x12340000 | Offset,
                                                 0x56780000 | Offset};
            ASSERT_NO_FATAL_FAILURE(compare(
                form(Kind, Width, Code32, false), Code32, !Backward, CPL, 0,
                0x11111111, 3, 0xad7 | (Backward ? 0x400 : 0), 0x4000,
                0x87654321, Code, Inputs, [&](kvm_sregs &Special, kvm_regs &) {
                  Special.ds.base = 0x30000;
                  Special.es.base = 0x50000;
                  std::fill(RAM + 0x30000, RAM + 0x40010, 0x11);
                  std::fill(RAM + 0x50000, RAM + 0x60010,
                            Kind == 4 || Kind == 6 ? 0x22 : 0x11);
                }));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, LockedStringsFaultBeforeAnyDataAccess) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Kind = 7; Kind != 12; ++Kind)
        for (unsigned CPL = 0; CPL != 4; ++CPL) {
          auto Bytes = form(Kind, Width, Code32, Code32);
          Bytes.insert(Bytes.begin(), 0xf0);
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, 1));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, OverlappingMovesObserveEarlierElements) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (int Distance : {-4, -1, 0, 1, 4})
          for (bool Backward : {false, true}) {
            const uint32_t High = Address32 ? 0 : 0xabcd0000;
            const std::array<uint32_t, 8> Inputs{
                0x11111111,    High | 8,
                0x87654321,    0xabcdef01,
                0xf800,        0x23456789,
                High | 0xc080, High | uint32_t(0xc080 + Distance)};
            ASSERT_NO_FATAL_FAILURE(compare(
                form(0, Width, Code32, Address32), Code32, !Backward, 3, 0,
                0x11111111, 8, 0xad7 | (Backward ? 0x400 : 0), 0x4000,
                0x87654321, Code, Inputs, [&](kvm_sregs &, kvm_regs &) {
                  for (unsigned I = 0; I != 256; ++I)
                    RAM[0xc000 + I] = I * 37 + 11;
                }));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStringNativeTest, PendingIRQAndNMIKeepOriginalRestartFrames) {
  for (auto Kind : {X86LegacyEventKind::Interrupt, X86LegacyEventKind::NMI})
    for (bool STI : {false, true}) {
      PendingEvent = Kind;
      STIShadow = STI;
      for (bool Code32 : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned Width : {1u, 2u, 4u})
            for (unsigned Instruction = 0; Instruction != 7; ++Instruction)
              for (unsigned CPL : {0u, 3u})
                for (unsigned Count : {0u, 1u, 4u})
                  ASSERT_NO_FATAL_FAILURE(string(Instruction, Width, Code32,
                                                 Address32, CPL, Count, false));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyStrings, X86LegacyStringNativeTest,
                         testing::Values(2u, 3u));
