//===- X86LegacyVM86NativeTests.inc - Original VM86 mode sequences -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original IRETD, VM86 instructions and protected event frames in KVM.
//===----------------------------------------------------------------------===//

class X86LegacyVM86NativeTest : public X86PagingNativeTest,
                                public X86LegacyVM86TestState {
protected:
  using X86LegacyTestState::put;
  unsigned Comparisons = 0;

  void compare(unsigned Case) {
    SCOPED_TRACE(testing::Message() << "vm86 case=" << Case);
    const bool Low = Case == 1 || Case == 2 || Case == 3 || Case == 10 ||
                     Case == 11 || Case == 15 || Case == 21 || Case == 52;
    vm86ReturnFrame(Low ? 0x20202 : 0x23202);
    O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
    auto Controls = M->controls();
    Controls.CR0 |= 0x10;
    Controls.CR4 =
        Case == 6 ? 2
        : (Case == 1 || Case == 3 || Case == 5 || (Case >= 8 && Case <= 11) ||
           (Case >= 19 && Case <= 28) || (Case >= 33 && Case <= 39))
            ? 1
            : 0;
    llvm::cantFail(M->bindControls(Controls));
    S.TR.Limit = 0x200;
    put(0x5866, 0x80, GuestScalarWidth::I16);
    // All reference streams stop in a protected handler at CPL0. The OUT
    // marker does not change registers/flags; it is not part of the model run.
    for (unsigned Vector = 0; Vector != 256; ++Vector) {
      const uint32_t Handler = 0x2000 + Vector * 16;
      put(0x6000 + Vector * 8,
          Handler | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
      put(Handler, 0x80e6, GuestScalarWidth::I16);
    }
    using Unit = std::pair<uint32_t, std::vector<uint8_t>>;
    std::vector<Unit> Units{{0x1000, {0xcf}}};
    if (Case <= 3 && Case != 0) {
      const unsigned Width = Case == 3 ? 4 : 2;
      const auto Size =
          Width == 4 ? GuestScalarWidth::I32 : GuestScalarWidth::I16;
      put(0xc000, 0x180, Size);
      put(0xc000 + Width, 0x100, Size);
      put(0xc000 + 2 * Width, 0x202, Size);
      Units.push_back({0x1100, Case == 3 ? std::vector<uint8_t>{0x66, 0xcf}
                                         : std::vector<uint8_t>{0xcf}});
      Units.push_back({0x1180, {0xcc}});
    } else if (Case == 4 || (Case >= 5 && Case <= 7)) {
      if (Case == 4)
        put(0xd000, 0x100100, GuestScalarWidth::I32);
      else
        put(0xd008, 0x1a3202, GuestScalarWidth::I32);
      Units.push_back({0x1100, {0x90}});
      Units.push_back({0x1101, {0xcc}});
    } else if ((Case >= 8 && Case <= 10) || (Case >= 19 && Case <= 28)) {
      const unsigned Vector = Case == 25 || Case == 26 ? 255
                              : Case == 28             ? 48
                                                       : 64;
      put(Vector * 4, 0x01000180, GuestScalarWidth::I32);
      Units.push_back(
          {0x1100, Case == 24 ? std::vector<uint8_t>{0x66, 0xcd, 0x40}
                              : std::vector<uint8_t>{0xcd, uint8_t(Vector)}});
      Units.push_back({0x1180, {0xcc}});
      if (Case == 8)
        put(0x11000, 3, GuestScalarWidth::I32); // Supervisor IVT page only.
      if (Case == 9) {
        put(0xd00c, 0x106, GuestScalarWidth::I32);
        put(0xd010, 0, GuestScalarWidth::I32); // Stack overwrites its own IVT.
      }
      if (Case == 10)
        put(0xd008, 0xa0202, GuestScalarWidth::I32);
      if (Case == 19)
        put(0x5866, 16, GuestScalarWidth::I16);
      if (Case == 20 || Case == 21)
        put(0x5868, 1, GuestScalarWidth::I8); // Redirect vector 64 to IDT.
      if (Case == 22)
        S.TR.Limit = 0x66;
      if (Case == 25 || Case == 26)
        put(0x5866, Case == 25 ? 31 : 16, GuestScalarWidth::I16);
      if (Case == 27) {
        S.TR.Attributes = 0x83;
        put(0x5802, 0xe000, GuestScalarWidth::I16);
        put(0x5804, 16, GuestScalarWidth::I16);
      }
      if (Case == 28)
        S.TR.Limit = 0x67;
      if (Case == 23)
        S.TR.Limit = 0x67; // Bitmap byte at 0x68 is beyond TSS.
    } else if (Case == 11) {
      Units.insert(Units.end(), {{0x1100, {0xfb}},
                                 {0x1101, {0x9c}},
                                 {0x1102, {0xfa}},
                                 {0x1103, {0x9d}},
                                 {0x1104, {0xcc}}});
    } else if (Case == 12) {
      Units.insert(
          Units.end(),
          {{0x1100, {0x66, 0x9c}}, {0x1102, {0x66, 0x9d}}, {0x1104, {0xcc}}});
    } else if (Case == 13) {
      Units.insert(Units.end(), {{0x1100, {0x9a, 0, 2, 0x23, 1}},
                                 {0x1430, {0xcb}},
                                 {0x1105, {0xcc}}});
    } else if (Case == 14) {
      S.Core.GPR[0] = 0x12345678;
      Units.insert(Units.end(),
                   {{0x1100, {0x2e, 0xa3, 0, 4}}, {0x1104, {0xcc}}});
    } else if (Case == 15 || Case == 16) {
      Units.push_back({0x1100, {0xcd, 0x40}});
      if (Case == 16)
        put(0x6200, 0x2000 | (uint64_t{27} << 16) | (uint64_t{0xee} << 40));
    } else if (Case == 17) {
      put(0x11010, 0x4003, GuestScalarWidth::I32);
      Units.push_back({0x1100, {0xa1, 0, 0}}); // VM86 DS user data #PF.
    } else if (Case == 18) {
      S.Core.GPR[0] = 0;
      Units.insert(
          Units.end(),
          {{0x1100, {0x8e, 0xd0}}, {0x1102, {0x90}}, {0x1103, {0xcc}}});
    } else if (Case >= 51) {
      put(0xd008, (Low ? 0x20202 : 0x23202) | 0x10000, GuestScalarWidth::I32);
      put(0xc000, 0x180, GuestScalarWidth::I16);
      put(0xc002, 0x100, GuestScalarWidth::I16);
      put(0xc004, 0x202, GuestScalarWidth::I16);
      S.Debug.emplace();
      O.Debug = X86LegacyDebugProfile::BranchWatchpoints;
      S.Debug->DR[0] = 0x1180;
      S.Debug->DR7 = 0x401;
      Units.push_back({0x1100, {0xcf}});
      Units.push_back({0x1180, {0xcc}});
    } else if (Case >= 33) {
      unsigned SP = 0x4000;
      if (Case <= 39) {
        const unsigned Values[]{0, 2, 4, 6, 1, 3, 5};
        SP = Values[Case - 33];
        put(0x100, 0x01000180, GuestScalarWidth::I32);
        Units.push_back({0x1100, {0xcd, 0x40}});
      } else if (Case <= 45) {
        const unsigned Values[]{0, 2, 4, 1, 3, 5};
        SP = Values[Case - 40];
        Units.push_back({0x1100, {0x9a, 0x80, 1, 0, 1}});
      } else if (Case <= 49) {
        const unsigned Values[]{1, 3, 5, 8};
        SP = Values[Case - 46];
        Units.push_back({0x1100, {0x66, 0x9a, 0, 0, 1, 0, 0, 1}});
      } else {
        Units.push_back({0x1100, {0x66, 0x9a, 0x80, 1, 0, 0, 0, 1}});
        Units.push_back({0x1108, {0xcc}});
        put(0xbffc, 0xaaaa5555, GuestScalarWidth::I32);
      }
      put(0xd00c, 0xabcd0000 | SP, GuestScalarWidth::I32);
      Units.push_back({0x1180, Case == 50 ? std::vector<uint8_t>{0x66, 0xcb}
                                          : std::vector<uint8_t>{0xcc}});
    } else if (Case >= 29) {
      Units.push_back({0x1100, {0xcd, 0x40}});
      if (Case == 29)
        put(0x6200, 0x2400 | (uint64_t{8} << 16) | (uint64_t{0xe6} << 40));
      if (Case == 30) {
        put(0x7038, 0x00cf9f000000ffffULL);
        put(0x6200, 0x2000 | (uint64_t{56} << 16) | (uint64_t{0xee} << 40));
      }
      if (Case == 31) {
        put(0x7038, 0x00409b0000001fffULL);
        put(0x6200, 0x2000 | (uint64_t{56} << 16) | (uint64_t{0xee} << 40));
      }
      if (Case == 32) {
        put(0x7010, 0x000093000000ffffULL);
        put(0x5804, 0x1234e000, GuestScalarWidth::I32);
      }
    } else {
      Units.insert(Units.end(),
                   {{0x1100, {0xb8, 0x34, 0x12}}, {0x1103, {0xcc}}});
    }
    for (const auto &[Address, Bytes] : Units)
      for (unsigned I = 0; I != Bytes.size(); ++I)
        put(Address + I, Bytes[I], GuestScalarWidth::I8);
    const auto Image = M->physicalMemory().snapshotMemoryRegions();
    std::memset(RAM, 0, MemorySize);
    std::copy(Image.front().Bytes.begin(), Image.front().Bytes.end(), RAM);
    const auto Segment = [](const neverd::X86LegacySegmentCache &C) {
      kvm_segment K{};
      K.base = C.Base;
      K.limit = C.Limit;
      K.selector = C.Selector;
      K.type = C.Attributes & 15;
      K.s = (C.Attributes >> 4) & 1;
      K.dpl = (C.Attributes >> 5) & 3;
      K.present = (C.Attributes >> 7) & 1;
      K.avl = (C.Attributes >> 12) & 1;
      K.l = (C.Attributes >> 13) & 1;
      K.db = (C.Attributes >> 14) & 1;
      K.g = (C.Attributes >> 15) & 1;
      K.unusable = C.Unusable;
      return K;
    };
    kvm_sregs Special = Initial;
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    for (unsigned I = 0; I != 6; ++I)
      *Segments[I] = Segment(S.Core.Segments[I]);
    Special.tr = Segment(S.TR);
    Special.ldt = Segment(S.Core.LDTR);
    Special.gdt = {S.Core.GDTRBase, S.Core.GDTRLimit, {}};
    Special.idt = {S.IDTRBase, S.IDTRLimit, {}};
    Special.cr0 = Controls.CR0;
    Special.cr2 = S.CR2;
    Special.cr3 = Controls.CR3;
    Special.cr4 = Controls.CR4;
    Special.efer = 0;
    auto Flush = Special;
    Flush.cr3 = 0x19000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    kvm_debugregs HardwareDebug{};
    HardwareDebug.dr6 = 0xffff0ff0;
    HardwareDebug.dr7 = 0x400;
    if (S.Debug) {
      for (unsigned I = 0; I != 4; ++I)
        HardwareDebug.db[I] = S.Debug->DR[I];
      HardwareDebug.dr6 = S.Debug->DR6;
      HardwareDebug.dr7 = S.Debug->DR7;
    }
    ASSERT_EQ(ioctl(CPU, KVM_SET_DEBUGREGS, &HardwareDebug), 0);
    kvm_regs Regs{};
    std::array<__u64 *, 8> GPR{&Regs.rax, &Regs.rcx, &Regs.rdx, &Regs.rbx,
                               &Regs.rsp, &Regs.rbp, &Regs.rsi, &Regs.rdi};
    for (unsigned I = 0; I != 8; ++I)
      *GPR[I] = S.Core.GPR[I];
    Regs.rip = S.EIP;
    Regs.rflags = S.EFlags;
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Regs), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    unsigned Steps = 0;
    do {
      const auto Address = S.Core.Segments[1].Base + S.EIP;
      auto It = std::find_if(Units.begin(), Units.end(),
                             [&](const Unit &U) { return U.first == Address; });
      ASSERT_NE(It, Units.end()) << Address;
      auto R = executeX86LegacyInstruction(S, *M, It->second, O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_TRUE(R->InstructionCompleted ||
                  R->Status == X86LegacyExecutionStatus::EventDelivered);
      ASSERT_LT(++Steps, 16u);
    } while (S.Core.CPL != 0 || S.EIP < 0x2000 || S.EIP >= 0x3000 ||
             (S.EIP & 15));
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_IO))
        << Run->fail_entry.hardware_entry_failure_reason;
    ASSERT_EQ(Run->io.port, 0x80);
    Run->immediate_exit = 1;
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), -1);
    ASSERT_EQ(errno, EINTR);
    Run->immediate_exit = 0;
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Regs), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    EXPECT_EQ(Regs.rip - 2, S.EIP);
    EXPECT_EQ(Regs.rflags, S.EFlags);
    for (unsigned I = 0; I != 8; ++I)
      EXPECT_EQ(*GPR[I], S.Core.GPR[I]) << I;
    EXPECT_EQ(Special.cr2, S.CR2);
    for (unsigned I = 0; I != 6; ++I) {
      EXPECT_EQ(Segments[I]->selector, S.Core.Segments[I].Selector) << I;
      EXPECT_EQ(bool(Segments[I]->unusable), S.Core.Segments[I].Unusable) << I;
      if (S.Core.Segments[I].Unusable)
        continue;
      EXPECT_EQ(Segments[I]->base, S.Core.Segments[I].Base) << I;
      EXPECT_EQ(Segments[I]->limit, S.Core.Segments[I].Limit) << I;
      EXPECT_EQ(Segments[I]->type, S.Core.Segments[I].Attributes & 15) << I;
      EXPECT_EQ(Segments[I]->dpl, (S.Core.Segments[I].Attributes >> 5) & 3)
          << I;
      EXPECT_EQ(Segments[I]->db, bool(S.Core.Segments[I].Attributes & 0x4000))
          << I;
    }
    const auto After = M->physicalMemory().snapshotMemoryRegions();
    for (unsigned I = 0; I != Image.front().Bytes.size(); ++I) {
      // Only page-table A/D bits have a different walk-timing contract.
      const uint8_t Mask =
          I >= 0x10000 && I < 0x12000 && I % 4 == 0 ? 0x9f : 0xff;
      ASSERT_EQ(RAM[I] & Mask, After.front().Bytes[I] & Mask) << I;
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacyVM86NativeTest, IretExecutionVMEAndProtectedFrames) {
  for (unsigned Case = 0; Case != 53; ++Case)
    ASSERT_NO_FATAL_FAILURE(compare(Case));
  RecordProperty("original_sequence_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(VM86Native, X86LegacyVM86NativeTest,
                         testing::Values(2u));
