//===- X86LegacyFarNativeTests.inc - Original protected far transfers ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent CPU comparisons for far CALL/JMP and legacy call gates.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyFarNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
    ExtraMemoryChecks = {{0x7000, 256}, {0xa000, 4096}, {0xd000, 512}};
  }

  void TearDown() override { RecordProperty("cpu_comparisons", Comparisons); }

  static std::vector<uint8_t> immediate(bool Call, bool Code32, bool Wide,
                                        uint16_t Selector, uint32_t IP) {
    std::vector<uint8_t> Bytes;
    if (Code32 != Wide)
      Bytes.push_back(0x66);
    Bytes.push_back(Call ? 0x9a : 0xea);
    for (unsigned I = 0; I != (Wide ? 4u : 2u); ++I)
      Bytes.push_back(IP >> (I * 8));
    Bytes.push_back(Selector);
    Bytes.push_back(Selector >> 8);
    return Bytes;
  }

  static uint64_t gate(bool Wide, uint16_t Selector, uint32_t IP,
                       unsigned Parameters = 0, unsigned DPL = 3) {
    return uint16_t(IP) | uint64_t(Selector) << 16 |
           uint64_t(Parameters) << 32 |
           uint64_t(0x84 | (Wide ? 8 : 0) | DPL << 5) << 40 |
           uint64_t(IP >> 16) << 48;
  }
};

TEST_P(X86LegacyFarNativeTest, DirectWidthsPrivilegesAndAccessedWrites) {
  for (bool Call : {false, true})
    for (bool Code32 : {false, true})
      for (bool Wide : {false, true})
        for (bool Stack32 : {false, true})
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (bool Conforming : {false, true}) {
              const auto Bytes =
                  immediate(Call, Code32, Wide, 48 | CPL, 0x4000);
              const auto Configure = [&](kvm_sregs &, kvm_regs &) {
                put(0x7030, 0x00cf9a000000ffffULL | uint64_t(CPL) << 45 |
                                (Conforming ? uint64_t{4} << 40 : 0));
              };
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 0, 0,
                                              0, 0xed7, 0x4000, 0, Code, {},
                                              Configure));
            }
}

TEST_P(X86LegacyFarNativeTest, GateWidthsStackSwitchAndParameters) {
  for (bool Call : {false, true})
    for (bool Code32 : {false, true})
      for (bool Wide : {false, true})
        for (bool Gate32 : {false, true})
          for (bool Stack32 : {false, true})
            for (bool Inner : {false, true})
              for (unsigned Parameters : {0u, 1u, 3u, 31u}) {
                SCOPED_TRACE(testing::Message()
                             << "gate32=" << Gate32 << " inner=" << Inner
                             << " params=" << Parameters);
                const auto Bytes = immediate(Call, Code32, Wide, 59, 0x1234);
                const auto Configure = [&](kvm_sregs &, kvm_regs &) {
                  put(0x7038, gate(Gate32, Inner ? 8 : 27, 0x4000, Parameters));
                  for (unsigned I = 0; I != 32; ++I)
                    put(0xd100 + I * 4, 0xa5000000 | I * 0x12345, 4);
                };
                ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, 3, 0, 0,
                                                0, 0xed7, 0x4000, 0, Code, {},
                                                Configure));
              }
}

TEST_P(X86LegacyFarNativeTest, DescriptorAndLateTargetFaults) {
  for (bool Call : {false, true})
    for (bool Gate : {false, true})
      for (bool Wide : {false, true})
        for (unsigned Failure = 0; Failure != 11; ++Failure) {
          SCOPED_TRACE(testing::Message()
                       << "gate=" << Gate << " failure=" << Failure);
          uint16_t Selector = Gate ? 59 : 51;
          if (Failure == 0)
            Selector = 0;
          if (Failure == 1)
            Selector = 0x103;
          if (Failure == 2)
            Selector = 7;
          const auto Bytes = immediate(Call, true, Wide, Selector, 0x4000);
          const auto Configure = [&](kvm_sregs &, kvm_regs &) {
            uint64_t Descriptor = 0x00cffa000000ffffULL;
            if (Failure == 3)
              Descriptor &= ~(uint64_t{8} << 40); // Data, not code.
            if (Failure == 4)
              Descriptor &= ~(uint64_t{0x80} << 40);
            if (Failure == 5)
              Descriptor &= ~(uint64_t{3} << 45); // Inner nonconforming code.
            if (Failure == 6 || Failure == 7 || Failure == 8)
              Descriptor = 0x0040fa0000003fffULL;
            put(0x7030, Descriptor);
            put(0x7038, gate(Wide, 51, 0x4000));
            if (Failure == 7 || Failure == 8) {
              const unsigned PW = GetParam() == 2 ? 4 : 8;
              const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
              put(PT + 13 * PW, 0xd006, PW);
            }
            if (Gate && Failure == 9)
              put(0x7038, gate(Wide, 51, 0x4000, 0, 0));
            if (Gate && Failure == 10)
              put(0x7038, gate(Wide, 51, 0x4000) & ~(uint64_t{1} << 47));
          };
          ASSERT_NO_FATAL_FAILURE(compare(Bytes, true, true, 3, 0, 0, 0, 0xed7,
                                          0x4000, 0, Code, {}, Configure));
        }
}

TEST_P(X86LegacyFarNativeTest, GateTssAndStackWidthsRemainIndependent) {
  for (bool Gate32 : {false, true})
    for (bool Tss32 : {false, true})
      for (bool Stack32 : {false, true})
        for (unsigned TargetCPL : {0u, 1u, 2u})
          for (unsigned Parameters : {0u, 3u, 31u}) {
            SCOPED_TRACE(testing::Message()
                         << "gate32=" << Gate32 << " tss32=" << Tss32
                         << " stack32=" << Stack32 << " targetcpl=" << TargetCPL
                         << " params=" << Parameters);
            const auto Configure = [&](kvm_sregs &Special,
                                       kvm_regs &Registers) {
              Registers.rsp = 0xabcdd100u;
              Special.tr.type = Tss32 ? 11 : 3;
              put(0x7038, gate(Gate32, 48 | TargetCPL, 0x4000, Parameters));
              put(0x7030, 0x00cf9a000000ffffULL | uint64_t(TargetCPL) << 45);
              put(0x7040,
                  (Stack32 ? 0x00cf92000000ffffULL : 0x008f92000000ffffULL) |
                      uint64_t(TargetCPL) << 45);
              const uint32_t Tss =
                  0x5800 + (Tss32 ? 4 + TargetCPL * 8 : 2 + TargetCPL * 4);
              put(Tss, 0xb000, Tss32 ? 4 : 2);
              put(Tss + (Tss32 ? 4 : 2), 64 | TargetCPL, 2);
              for (unsigned I = 0; I != 32; ++I)
                put(0xd100 + I * 4, 0x87650000 | I * 0x123, 4);
            };
            ASSERT_NO_FATAL_FAILURE(compare(immediate(true, true, false, 59, 0),
                                            true, false, 3, 0, 0, 0, 0xed7,
                                            0x4000, 0, Code, {}, Configure));
          }
}

TEST_P(X86LegacyFarNativeTest, MemoryPointersUseCapturedAddressAndSegment) {
  for (bool Call : {false, true})
    for (bool Code32 : {false, true})
      for (bool Wide : {false, true})
        for (bool Address32 : {false, true})
          for (uint8_t Segment : {0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
            std::vector<uint8_t> Bytes{Segment};
            if (Code32 != Wide)
              Bytes.push_back(0x66);
            if (Code32 != Address32)
              Bytes.push_back(0x67);
            Bytes.insert(Bytes.end(), {0xff, uint8_t((Call ? 3 : 5) << 3 |
                                                     (Address32 ? 3 : 7))});
            const unsigned Case = Segment == 0x2e || Segment == 0x36 ? 0 : 3;
            const auto Configure = [&](kvm_sregs &, kvm_regs &) {
              const uint32_t Address = Case ? 0x1c100 : 0xc100;
              put(Address, 0x4000, Wide ? 4 : 2);
              put(Address + (Wide ? 4 : 2), 27, 2);
            };
            ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, false, 3, Case, 0, 0,
                                            0xed7, 0x4000, 0, Code, {},
                                            Configure));
          }
}

TEST_P(X86LegacyFarNativeTest, ParameterAndFramePageFaultOrdering) {
  for (bool Gate32 : {false, true})
    for (unsigned Failure = 0; Failure != 9; ++Failure) {
      SCOPED_TRACE(testing::Message()
                   << "gate32=" << Gate32 << " failure=" << Failure);
      const auto Configure = [&](kvm_sregs &, kvm_regs &Registers) {
        const unsigned Width = Gate32 ? 4 : 2;
        put(0x7038, gate(Gate32, 48, 0x4000, 3));
        // Keep ring-0 event entry usable if the call's ring-1 stack fails.
        put(0x7030, 0x00cfba000000ffffULL);
        put(0x7040, 0x00cfb2000000ffffULL);
        put(0x5808, 16, 2);
        put(0x5804, 0xe800, 4);
        put(0x580c, Failure == 5 ? 0xa000 + 2 * Width : 0xa800, 4);
        put(0x5810, 65, 2);
        Registers.rsp = 0xc000 - Width;
        for (unsigned I = 0; I != 3; ++I)
          put(Registers.rsp + I * Width, 0x98760000 | I * 0x123, Width);
        const unsigned PW = GetParam() == 2 ? 4 : 8;
        const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
        if (Failure == 0 || Failure == 2 || Failure == 4)
          put(PT + 12 * PW, 0xc006, PW);
        if (Failure == 1 || Failure == 2 || Failure == 3)
          put(PT + 10 * PW, 0xa005, PW);
        if (Failure == 3 || Failure == 4)
          put(0x7030, 0x0040ba0000003fffULL);
        if (Failure == 5)
          put(PT + 9 * PW, 0x9005, PW);
        if (Failure == 6)
          put(0x7040, 0x0040b20000a7ffULL);
        if (Failure == 7)
          put(0x5810, 0, 2);
        if (Failure == 8)
          put(0x7040, 0x00cf32000000ffffULL);
      };
      ASSERT_NO_FATAL_FAILURE(compare(immediate(true, true, true, 59, 0), true,
                                      true, 3, 0, 0, 0, 0xed7, 0x4000, 0, Code,
                                      {}, Configure));
    }
}

TEST_P(X86LegacyFarNativeTest, PointerFaultOrderAndOriginalHandlerRetry) {
  for (bool Call : {false, true})
    for (bool Wide : {false, true})
      for (bool Address32 : {false, true})
        for (unsigned Failure = 0; Failure != 8; ++Failure) {
          SCOPED_TRACE(testing::Message() << "failure=" << Failure);
          std::vector<uint8_t> Bytes;
          if (!Wide)
            Bytes.push_back(0x66);
          if (!Address32)
            Bytes.push_back(0x67);
          Bytes.insert(Bytes.end(), {0xff, uint8_t((Call ? 3 : 5) << 3 |
                                                   (Address32 ? 3 : 7))});
          const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
            const unsigned Width = Wide ? 4 : 2;
            const uint32_t Offset = Failure == 7 ? 0xc100 : 0xcfff;
            put(Offset, 0x4000, Width);
            put(Offset + Width, 27, 2);
            if (Failure == 1 || Failure == 2 || Failure == 4 || Failure == 6) {
              const unsigned PW = GetParam() == 2 ? 4 : 8;
              const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
              put(PT + 13 * PW, 0xd006, PW);
            }
            if (Failure == 2 || Failure == 3 || Failure == 6) {
              Special.ds.g = 0;
              Special.ds.limit = Offset + Width - 1;
            }
            if (Failure == 5)
              Special.ds.unusable = 1;
          };
          std::array<uint32_t, 8> Registers{};
          Registers[3] = Failure == 7 ? 0xc100 : 0xcfff;
          Registers[4] = 0xe800;
          // Case 12 runs an original PTE repair / ADD ESP / IRET / retry.
          ASSERT_NO_FATAL_FAILURE(
              compare(Bytes, true, true, 3, Failure == 7 ? 12 : 0, 0, 0, 0xed7,
                      0x4000, 0, Code, Registers, Configure));
        }
}

TEST_P(X86LegacyFarNativeTest, OverlappingParameterStacksCopyInExecutionOrder) {
  for (bool Wide : {false, true})
    for (int Delta : {-32, -8, 0, 4, 8, 16, 32}) {
      SCOPED_TRACE(testing::Message() << "wide=" << Wide << " delta=" << Delta);
      const auto Configure = [&](kvm_sregs &, kvm_regs &Registers) {
        const unsigned Width = Wide ? 4 : 2;
        put(0x7038, gate(Wide, 8, 0x4000, 5));
        Registers.rsp = 0xa800;
        put(0x5804, 0xa800 + Delta, 4);
        for (unsigned I = 0; I != 5; ++I)
          put(Registers.rsp + I * Width, 0x98760000 | I * 0x123, Width);
      };
      ASSERT_NO_FATAL_FAILURE(compare(immediate(true, true, !Wide, 59, 0), true,
                                      true, 3, 0, 0, 0, 0xed7, 0x4000, 0, Code,
                                      {}, Configure));
    }
}

TEST_P(X86LegacyFarNativeTest, PointerAddressWrapAndStackAliases) {
  for (bool Call : {false, true})
    for (bool Wide : {false, true})
      for (unsigned Form = 0; Form != 4; ++Form) {
        SCOPED_TRACE(testing::Message() << "wide=" << Wide << " form=" << Form);
        std::vector<uint8_t> Bytes;
        if (!Wide)
          Bytes.push_back(0x66);
        const uint8_t Extension = (Call ? 3 : 5) << 3;
        if (!Form)
          Bytes.insert(Bytes.end(), {0x67, 0xff, uint8_t(Extension | 7)});
        else if (Form == 1)
          Bytes.insert(Bytes.end(), {0xff, uint8_t(Extension | 4), 0x24});
        else if (Form == 2)
          Bytes.insert(Bytes.end(),
                       {0xff, uint8_t(0x44 | Extension), 0x24, 0xfc});
        else
          Bytes.insert(Bytes.end(), {0xff, uint8_t(0x45 | Extension), 0});
        std::array<uint32_t, 8> Registers{};
        Registers[3] = 0xabcdfffe;
        Registers[4] = Registers[5] = 0xd100;
        const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
          const unsigned Width = Wide ? 4 : 2;
          if (!Form) {
            Special.ds.base = 0x20000;
            put(0x2fffe, 0x4000, Width);
            put(0x20000 + uint16_t(0xfffe + Width), 27, 2);
            put(0x30000 + Width - 2, 0, 2);
          } else {
            const uint32_t Address = Form == 2 ? 0xd0fc : 0xd100;
            put(Address, 0x4000, Width);
            put(Address + Width, 27, 2);
          }
        };
        ASSERT_NO_FATAL_FAILURE(compare(Bytes, true, true, 3, 0, 0, 0, 0xed7,
                                        0x4000, 0, Code, Registers, Configure));
      }
}

TEST_P(X86LegacyFarNativeTest, ConformingAndLdtTargetsCheckPrivileges) {
  ExtraMemoryChecks.emplace_back(0x8000, 128);
  for (bool Call : {false, true})
    for (bool Gate : {false, true})
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (unsigned DPL = 0; DPL != 4; ++DPL)
          for (unsigned RPL = 0; RPL != 4; ++RPL)
            for (bool Conforming : {false, true}) {
              SCOPED_TRACE(testing::Message()
                           << "gate=" << Gate << " dpl=" << DPL
                           << " rpl=" << RPL << " conforming=" << Conforming);
              const uint16_t Selector = (Gate ? 12 : 20) | RPL;
              const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
                Special.ldt.base = 0x8000;
                Special.ldt.limit = 0x7f;
                Special.ldt.selector = 80;
                Special.ldt.type = 2;
                Special.ldt.present = 1;
                Special.ldt.unusable = 0;
                put(0x8010, 0x00cf9a000000ffffULL | uint64_t(DPL) << 45 |
                                (Conforming ? uint64_t{4} << 40 : 0));
                put(0x8008, gate(true, 23, 0x4000, 0, RPL));
                put(0x5804, 0xb000, 4);
                for (unsigned Level : {1u, 2u}) {
                  put(0x5804 + Level * 8, 0xb000, 4);
                  put(0x5808 + Level * 8, 80 + Level * 8 + Level, 2);
                  put(0x7000 + 80 + Level * 8,
                      0x00cf92000000ffffULL | uint64_t(Level) << 45);
                }
              };
              ASSERT_NO_FATAL_FAILURE(compare(
                  immediate(Call, true, true, Selector, 0x4000), true, true,
                  CPL, 0, 0, 0, 0xed7, 0x4000, 0, Code, {}, Configure));
            }
}

TEST_P(X86LegacyFarNativeTest, AlignmentOfPointerParametersAndReturnFrame) {
  for (bool Wide : {false, true})
    for (unsigned Form = 0; Form != 4; ++Form) {
      SCOPED_TRACE(testing::Message() << "wide=" << Wide << " form=" << Form);
      std::vector<uint8_t> Bytes;
      if (Form == 0) {
        if (!Wide)
          Bytes.push_back(0x66);
        Bytes.insert(Bytes.end(), {0xff, 0x1b});
      } else {
        Bytes = immediate(true, true, Wide, Form == 1 ? 27 : 59, 0x4000);
      }
      std::array<uint32_t, 8> Registers{};
      Registers[3] = 0xc101;
      Registers[4] = Form == 0 ? 0xd100 : 0xd101;
      const auto Configure = [&](kvm_sregs &, kvm_regs &) {
        put(0xc101, 0x4000, Wide ? 4 : 2);
        put(0xc101 + (Wide ? 4 : 2), 27, 2);
        put(0x7038, gate(Wide, Form == 2 ? 27 : 8, 0x4000, 3));
        for (unsigned I = 0; I != 4; ++I)
          put(0xd101 + I * 4, 0x98760000 | I * 0x123, 4);
      };
      ASSERT_NO_FATAL_FAILURE(compare(Bytes, true, true, 3, 0, 0, 0, 0x40202,
                                      0x4000, 0, Code, Registers, Configure));
    }
}

TEST_P(X86LegacyFarNativeTest, TssAndDescriptorFaultsKeepCallerState) {
  for (bool Tss32 : {false, true})
    for (unsigned Failure = 0; Failure != 12; ++Failure) {
      SCOPED_TRACE(testing::Message()
                   << "tss32=" << Tss32 << " failure=" << Failure);
      const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
        put(0x7038, gate(true, 48, 0x4000, 1));
        put(0x7030, 0x00cfba000000ffffULL);
        put(0x7040, 0x00cfb2000000ffffULL);
        Special.tr.type = Tss32 ? 11 : 3;
        const unsigned Offset = Tss32 ? 12 : 6, Width = Tss32 ? 4 : 2;
        if (Failure == 0 || Failure == 1)
          Special.tr.limit = Offset + Width - (Failure == 0 ? 0 : 1);
        if (Failure == 2 || Failure == 3)
          Special.tr.base = 0x9000 - Offset - (Failure == 2 ? 0 : Width);
        put(Special.tr.base + Offset, 0xa800, Width);
        put(Special.tr.base + Offset + Width, 65, 2);
        const unsigned PW = GetParam() == 2 ? 4 : 8;
        const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
        if (Failure == 2 || Failure == 3)
          put(PT + 9 * PW, 0x9006, PW);
        if (Failure == 4)
          put(Special.tr.base + Offset + Width, 64, 2); // Wrong RPL.
        if (Failure == 5)
          put(Special.tr.base + Offset + Width, 0x101, 2);
        if (Failure == 6)
          put(Special.tr.base + Offset + Width, 5, 2); // Unusable LDT.
        if (Failure == 7)
          put(0x7040, 0x00cfb0000000ffffULL); // Read-only data.
        if (Failure == 8)
          put(0x7040, 0x00cf92000000ffffULL); // Wrong DPL.
        if (Failure == 9)
          put(0x7040, 0x0040b20000a7feULL); // Final pushed byte out of limit.
        if (Failure == 10)
          put(PT + 7 * PW, 0x7005, PW); // Code accessed write faults.
        if (Failure == 11) {
          put(0x7030, 0x00cfbb000000ffffULL);
          put(PT + 7 * PW, 0x7005, PW); // Stack accessed write faults.
        }
        // A same-ring fault bank observes invalid inner stacks without using
        // the deliberately broken TSS again during exception delivery.
        for (unsigned V = 0; V != 256; ++V)
          put(0x6000 + V * 8,
              (0x2000 + V * 16) | uint64_t{27} << 16 | uint64_t{0xee} << 40);
      };
      ASSERT_NO_FATAL_FAILURE(compare(immediate(true, true, true, 59, 0), true,
                                      true, 3, 0, 0, 0, 0xed7, 0x4000, 0, Code,
                                      {}, Configure));
    }
}

TEST_P(X86LegacyFarNativeTest, LateLimitAndHighReturnPcRespectTransferWidth) {
  for (bool Code32 : {false, true})
    for (bool Wide : {false, true})
      for (bool Gate : {false, true})
        for (bool BadTarget : {false, true}) {
          const auto Configure = [&](kvm_sregs &, kvm_regs &) {
            put(0x7030,
                BadTarget ? 0x0040fa0000003fffULL : 0x00cffa000000ffffULL);
            put(0x7038, gate(Wide, 51, 0x4000));
          };
          ASSERT_NO_FATAL_FAILURE(compare(
              immediate(true, Code32, Wide, Gate ? 59 : 51, 0x4000), Code32,
              true, 3, 0, 0, 0, 0xed7, 0x4000, 0, 0x12345, {}, Configure));
        }
}

TEST_P(X86LegacyFarNativeTest, WordStackWrapAndNonflatTargetCaches) {
  for (bool Wide : {false, true})
    for (bool Gate : {false, true})
      for (bool Target32 : {false, true})
        for (unsigned SP : {0u, 2u, 4u, 6u, 8u}) {
          SCOPED_TRACE(testing::Message()
                       << "wide=" << Wide << " gate=" << Gate
                       << " target32=" << Target32 << " sp=" << SP);
          const auto Configure = [&](kvm_sregs &Special, kvm_regs &Registers) {
            Special.ss.base = 0x20000;
            Registers.rsp = 0xabcd0000 | SP;
            put(0x7030,
                (Target32 ? 0x00cffa000000ffffULL : 0x008ffa000000ffffULL) |
                    uint64_t{6} << 32);
            put(0x7038, gate(Wide, 51, 0x4000));
          };
          ASSERT_NO_FATAL_FAILURE(compare(
              immediate(true, true, Wide, Gate ? 59 : 51, 0x4000), true, false,
              3, 0, 0, 0, 0xed7, 0x64000, 0, Code, {}, Configure));
        }
}

TEST_P(X86LegacyFarNativeTest, ParameterOffsetsWrapAtCallerStackWidth) {
  ExtraMemoryChecks.emplace_back(0x2fff0, 160);
  for (bool Wide : {false, true})
    for (unsigned Offset : {0xfffcu, 0xfffeu})
      for (unsigned Parameters : {0u, 3u, 31u}) {
        SCOPED_TRACE(testing::Message() << "wide=" << Wide << " offset="
                                        << Offset << " params=" << Parameters);
        const auto Configure = [&](kvm_sregs &Special, kvm_regs &Registers) {
          Special.ss.base = 0x20000;
          Registers.rsp = 0xabcd0000 | Offset;
          put(0x7038, gate(Wide, 8, 0x4000, Parameters));
          const unsigned Width = Wide ? 4 : 2;
          for (unsigned I = 0; I != Parameters; ++I)
            put(0x20000 + uint16_t(Offset + I * Width), 0x98760000 | I * 0x123,
                Width);
        };
        ASSERT_NO_FATAL_FAILURE(compare(immediate(true, true, !Wide, 59, 0),
                                        true, false, 3, 0, 0, 0, 0xed7, 0x4000,
                                        0, Code, {}, Configure));
      }
}

INSTANTIATE_TEST_SUITE_P(LegacyFar, X86LegacyFarNativeTest,
                         testing::Values(2u, 3u));
