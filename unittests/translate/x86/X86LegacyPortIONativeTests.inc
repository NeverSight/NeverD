//===- X86LegacyPortIONativeTests.inc - Original protected port oracle ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// KVM-intercepted original IN/OUT/INS/OUTS with independent device tapes.
/// Included by the native paging translation unit after the common fixture.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyPortIONativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    PortIO = X86PortIOProfile::InputBeforeWriteFragments;
    ExtraMemoryChecks = {{0xc000, 0x180}, {0xe000, 0x180}, {0x1e000, 0x180},
                         {0xeff0, 0x20},  {0x20000, 0x20}, {0x2fff0, 0x20}};
  }

  static std::vector<uint8_t> encoding(unsigned Kind, unsigned Width,
                                       bool Code32, bool Address32,
                                       unsigned Segment = 0) {
    std::vector<uint8_t> B;
    if (Kind >= 6)
      B.push_back(0xf3);
    if (Segment)
      B.push_back(Segment);
    if (Address32 != Code32)
      B.push_back(0x67);
    if (Width != 1 && (Width == 4) != Code32)
      B.push_back(0x66);
    B.push_back((Kind >= 4   ? 0x6c
                 : Kind >= 2 ? 0xe4
                             : 0xec) |
                (Kind & 1) * 2 | (Width != 1));
    if (Kind == 2 || Kind == 3)
      B.push_back(0xf3); // Immediate port data is not a REP prefix.
    return B;
  }

  static std::array<uint32_t, 8> registers(bool Address32, uint32_t Count) {
    return {0x12345678,
            Count | (Address32 ? 0 : 0xabcd0000),
            0x987602f9,
            0x11112222,
            0xd100,
            0x33334444,
            0xc100u | (Address32 ? 0 : 0x12340000),
            0xe100u | (Address32 ? 0 : 0x56780000)};
  }

  void permissions(kvm_sregs &Special) {
    Special.tr.limit = 0x3000;
    put(0x5866, 0x100, 2);
    put(0x7900, 0xff, 1);
  }

  void TearDown() override {
    RecordProperty("kvm_port_comparisons", Comparisons);
    RecordProperty("kvm_input_batch_restart_discrepancies",
                   PortBatchDiscrepancies);
    RecordProperty(
        "oracle",
        "KVM intercepted port emulation and independent synthetic device");
    X86LegacyScalarNativeFixture::TearDown();
  }
};

TEST_P(X86LegacyPortIONativeTest, WidthsCountsDirectionsAndPrivileges) {
  for (unsigned Kind = 0; Kind != 8; ++Kind)
    for (unsigned Width : {1u, 2u, 4u})
      for (bool Code32 : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (bool DF : {false, true})
              for (unsigned Count : {0u, 1u, 3u}) {
                if (Kind < 6 && Count != 1)
                  continue;
                ASSERT_NO_FATAL_FAILURE(
                    compare(encoding(Kind, Width, Code32, Address32), Code32,
                            true, CPL, 0, 0, 0, 0x202 | (DF ? 0x400 : 0),
                            0x4000, 0, Code, registers(Address32, Count),
                            [&](kvm_sregs &Special, kvm_regs &) {
                              permissions(Special);
                              for (unsigned I = 0; I != 64; ++I)
                                RAM[0xc0e0 + I] = uint8_t(I * 17 + 7);
                            }));
                if (HasFailure())
                  return;
              }
}

TEST_P(X86LegacyPortIONativeTest, PermissionPriorityAndBitmapExtent) {
  for (unsigned Kind = 0; Kind != 8; ++Kind)
    for (unsigned Width : {1u, 2u, 4u})
      for (bool Code32 : {false, true})
        for (unsigned Case = 0; Case != 9; ++Case) {
          auto GPR = registers(true, Case == 1 ? 0 : 1);
          if (Case == 4)
            GPR[2] = 0xffff;
          if (Case == 5)
            GPR[2] = 0x2ff;
          const uint32_t Flags = Case == 2 ? 0x3202 : 0x202;
          ASSERT_NO_FATAL_FAILURE(compare(
              encoding(Kind, Width, Code32, true), Code32, true, 3, 0, 0, 0,
              Flags, 0x4000, 0, Code, GPR, [&](kvm_sregs &Special, kvm_regs &) {
                permissions(Special);
                const unsigned Port =
                    Kind == 2 || Kind == 3 ? 0xf3 : GPR[2] & 0xffff;
                if (Case < 3)
                  put(0x5900 + Port / 8, 1 << (Port & 7), 1);
                if (Case == 3)
                  Special.tr.limit = 0x66;
                if (Case == 5)
                  put(0x5900 + Port / 8 + 1, 2, 1);
                if (Case == 6)
                  put(0x5866, 0xffff, 2);
                if (Case == 7) {
                  Special.tr.type = 3;
                  // A 16-bit TSS uses SP0/SS0 at +2/+4 for #GP entry.
                  put(0x5802, 0xb000, 2);
                  put(0x5804, 16, 2);
                }
                if (Case == 8) {
                  put(0x5866, 0x2800, 2);
                  const unsigned PT = GetParam() == 2 ? 0x11000 : 0x12000;
                  put(PT + 8 * (GetParam() == 2 ? 4 : 8), 0x8006,
                      GetParam() == 2 ? 4 : 8);
                }
              }));
          if (HasFailure())
            return;
        }
}

TEST_P(X86LegacyPortIONativeTest, SourceOverridesAndNonflatSegments) {
  for (unsigned Kind : {4u, 5u, 6u, 7u})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Segment : {0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
        for (bool Code32 : {false, true})
          for (bool Address32 : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(compare(
                encoding(Kind, Width, Code32, Address32, Segment), Code32, true,
                3, 0, 0, 0, 0x202, 0x4000, 0, Code, registers(Address32, 3),
                [&](kvm_sregs &Special, kvm_regs &) {
                  permissions(Special);
                  Special.ds.base = Special.es.base = Special.fs.base =
                      Special.gs.base = 0x10000;
                  for (unsigned I = 0; I != 64; ++I) {
                    RAM[0xc100 + I] = uint8_t(I * 17 + 3);
                    RAM[0x1c100 + I] = uint8_t(I * 13 + 7);
                  }
                }));
            if (HasFailure())
              return;
          }
}

TEST_P(X86LegacyPortIONativeTest, OperandFaultsAndSplitInputStores) {
  for (unsigned Kind : {4u, 5u, 6u, 7u})
    for (unsigned Width : {1u, 2u, 4u})
      for (bool Code32 : {false, true})
        for (bool Address32 : {false, true})
          for (unsigned Case = 0; Case != 7; ++Case) {
            const bool Input = !(Kind & 1);
            auto GPR = registers(Address32, 1);
            if (Case == 1) {
              GPR[6] += 0xeff;
              GPR[7] += 0xeff;
            }
            ASSERT_NO_FATAL_FAILURE(compare(
                encoding(Kind, Width, Code32, Address32), Code32, true, 3, 0, 0,
                0, 0x202, 0x4000, 0, Code, GPR,
                [&](kvm_sregs &Special, kvm_regs &) {
                  permissions(Special);
                  const unsigned PT = GetParam() == 2 ? 0x11000 : 0x12000;
                  const unsigned PW = GetParam() == 2 ? 4 : 8;
                  auto &Segment = Input ? Special.es : Special.ds;
                  if (Case == 0 || Case == 1 || Case == 6) {
                    const unsigned Page = (Input ? 14 : 12) + (Case == 1);
                    put(PT + Page * PW, (Page << 12) | (Case == 6 ? 5 : 6), PW);
                  }
                  if (Case == 2)
                    Segment.unusable = 1;
                  if (Case == 3 || Case == 4) {
                    Segment.g = 0;
                    Segment.limit = uint16_t(GPR[Input ? 7 : 6]) +
                                    (Case == 4 ? Width - 1 : 0);
                  }
                  if (Case == 5)
                    Segment.type = 1; // Read-only data.
                }));
            if (HasFailure())
              return;
          }
}

TEST_P(X86LegacyPortIONativeTest,
       CompletedElementsSurviveHandlerRepairAndRetry) {
  for (unsigned Kind : {6u, 7u})
    for (unsigned Width : {1u, 2u, 4u})
      for (bool Code32 : {false, true})
        for (bool Address32 : {false, true}) {
          if (Kind == 6 && Width != 1)
            continue;
          auto GPR = registers(Address32, 2);
          GPR[6] = (GPR[6] & 0xffff0000) | (0xd000 - Width);
          GPR[7] = (GPR[7] & 0xffff0000) | (0xd000 - Width);
          ASSERT_NO_FATAL_FAILURE(compare(
              encoding(Kind, Width, Code32, Address32), Code32, true, 3, 23,
              0x12345678, 0, 0x202, 0x4000, 0, Code, GPR,
              [&](kvm_sregs &Special, kvm_regs &) { permissions(Special); }));
          if (HasFailure())
            return;
        }
}

TEST_P(X86LegacyPortIONativeTest, KVMInputBatchRestartIsReportedSeparately) {
  KnownPortBatchRestart = true;
  for (unsigned Kind : {6u})
    for (unsigned Width : {2u, 4u})
      for (bool Code32 : {false, true})
        for (bool Address32 : {false, true}) {
          auto GPR = registers(Address32, 2);
          GPR[6] = (GPR[6] & 0xffff0000) | (0xd000 - Width);
          GPR[7] = (GPR[7] & 0xffff0000) | (0xd000 - Width);
          ASSERT_NO_FATAL_FAILURE(compare(
              encoding(Kind, Width, Code32, Address32), Code32, true, 3, 23,
              0x12345678, 0, 0x202, 0x4000, 0, Code, GPR,
              [&](kvm_sregs &Special, kvm_regs &) { permissions(Special); }));
          if (HasFailure())
            return;
        }
}

TEST_P(X86LegacyPortIONativeTest, AddressWrapPrecedesSegmentBase) {
  for (unsigned Kind : {6u, 7u})
    for (bool Code32 : {false, true})
      for (bool DF : {false, true}) {
        auto GPR = registers(false, 2);
        GPR[6] = 0xabcd0000 | (DF ? 0 : 0xffff);
        GPR[7] = 0x56780000 | (DF ? 0 : 0xffff);
        ASSERT_NO_FATAL_FAILURE(
            compare(encoding(Kind, 1, Code32, false), Code32, true, 3, 0, 0, 0,
                    DF ? 0x602 : 0x202, 0x4000, 0, Code, GPR,
                    [&](kvm_sregs &Special, kvm_regs &) {
                      permissions(Special);
                      Special.ds.base = Special.es.base = 0x20000;
                      put(0x20000, 0x37, 1);
                      put(0x2ffff, 0x93, 1);
                    }));
        if (HasFailure())
          return;
      }
}

TEST_P(X86LegacyPortIONativeTest, LockFaultPrecedesPermissionsAndOperands) {
  for (unsigned Kind = 0; Kind != 6; ++Kind)
    for (unsigned Width : {1u, 2u, 4u})
      for (bool Code32 : {false, true}) {
        auto Bytes = encoding(Kind, Width, Code32, true);
        Bytes.insert(Bytes.begin(), 0xf0);
        ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, 3, 0, 0, 0, 0x202,
                                        0x4000, 0, Code, registers(true, 0)));
        if (HasFailure())
          return;
      }
}

TEST_P(X86LegacyPortIONativeTest,
       OutputProgressKeepsProducerSelectedIRQAndNMI) {
  for (auto Kind : {X86LegacyEventKind::Interrupt, X86LegacyEventKind::NMI})
    for (bool STI : {false, true})
      for (unsigned Form : {7u})
        for (unsigned Width : {1u, 2u, 4u})
          for (bool Code32 : {false, true}) {
            PendingEvent = Kind;
            STIShadow = STI;
            ASSERT_NO_FATAL_FAILURE(compare(
                encoding(Form, Width, Code32, true), Code32, true, 3, 0, 0, 0,
                0x202, 0x4000, 0, Code, registers(true, 4),
                [&](kvm_sregs &Special, kvm_regs &) { permissions(Special); }));
            if (HasFailure())
              return;
          }
}

INSTANTIATE_TEST_SUITE_P(LegacyPorts, X86LegacyPortIONativeTest,
                         testing::Values(2u, 3u));
