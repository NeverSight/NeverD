//===- X86ModeSegmentNativeTests.inc - Original segment loads ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original non64 MOV/LxS and code64 observations after null FS/GS loads.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X86ModeSegmentNativeTest : public X86ModeSegmentTest {
protected:
  void initializeOriginal(X64PagedNativeCPU &Native) {
    std::copy(RAM.Memory.front().Bytes.begin(), RAM.Memory.front().Bytes.end(),
              Native.RAM);
    auto &V = Machine.Core.Core;
    auto &R = registers();
    auto S = Native.Initial;
    std::array<kvm_segment *, 6> Segments{&S.es, &S.cs, &S.ss,
                                          &S.ds, &S.fs, &S.gs};
    for (unsigned I = 0; I != Segments.size(); ++I) {
      *Segments[I] = X64PagedNativeCPU::segment(cache(I));
      Segments[I]->unusable = bool(V.Unusable & (1u << UnusableBits[I]));
    }
    S.gdt.base = V.GDTR.Base;
    S.gdt.limit = V.GDTR.Limit;
    S.idt.base = V.IDTR.Base;
    S.idt.limit = V.IDTR.Limit;
    S.tr = X64PagedNativeCPU::segment(V.TR);
    S.ldt = X64PagedNativeCPU::segment(V.LDTR);
    S.ldt.unusable = bool(V.Unusable & 64);
    S.cr0 = Controls.CR0;
    S.cr3 = Controls.CR3;
    S.cr4 = Controls.CR4;
    S.efer = Controls.EFER;
    S.cr2 = V.Core.Core.CR2;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_SREGS, &S), 0) << errno;
    kvm_regs G{};
    G.rip = R.PC;
    G.rflags = R.RFlags;
    G.rax = R.GPR[0];
    G.rcx = R.GPR[1];
    G.rdx = R.GPR[2];
    G.rbx = R.GPR[3];
    G.rsp = R.GPR[4];
    G.rbp = R.GPR[5];
    G.rsi = R.GPR[6];
    G.rdi = R.GPR[7];
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &G), 0) << errno;
    kvm_vcpu_events Events{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_VCPU_EVENTS, &Events), 0) << errno;
  }

  void compareOriginal(X64PagedNativeCPU &Native, bool TerminalHalt = false) {
    kvm_sregs S{};
    kvm_regs G{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &S), 0) << errno;
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &G), 0) << errno;
    auto &R = registers();
    EXPECT_EQ(R.PC + TerminalHalt, G.rip);
    EXPECT_EQ(R.RFlags, G.rflags);
    const std::array<uint64_t, 8> GPR{G.rax, G.rcx, G.rdx, G.rbx,
                                      G.rsp, G.rbp, G.rsi, G.rdi};
    for (unsigned I = 0; I != GPR.size(); ++I)
      EXPECT_EQ(uint32_t(R.GPR[I]), uint32_t(GPR[I]));
    const std::array<const kvm_segment *, 6> Segments{&S.es, &S.cs, &S.ss,
                                                      &S.ds, &S.fs, &S.gs};
    for (unsigned I = 0; I != Segments.size(); ++I) {
      SCOPED_TRACE(I);
      const auto C = cache(I);
      const auto &H = *Segments[I];
      EXPECT_EQ(C.Selector, H.selector);
      EXPECT_EQ(bool(Machine.Core.Core.Unusable & (1u << UnusableBits[I])),
                bool(H.unusable));
      if (I >= 4 || !H.unusable)
        EXPECT_EQ(C.Base, H.base);
      if (H.unusable)
        continue; // A null load leaves the other cached bytes opaque.
      EXPECT_EQ(C.Limit, H.limit);
      const uint16_t Attributes = H.type | H.s << 4 | H.dpl << 5 |
                                  H.present << 7 | H.avl << 12 | H.l << 13 |
                                  H.db << 14 | H.g << 15;
      EXPECT_EQ(C.Attributes, Attributes);
    }
    EXPECT_EQ(Memory->controls().CR0, S.cr0);
    EXPECT_EQ(Memory->controls().CR3, S.cr3);
    EXPECT_EQ(Memory->controls().CR4, S.cr4);
    EXPECT_EQ(Memory->controls().EFER, S.efer);
    EXPECT_EQ(Machine.Core.Core.Core.Core.CR2, S.cr2);
    const auto Image = Memory->physicalMemory().snapshotMemoryRegions();
    ASSERT_EQ(Image.size(), 1u);
    const auto Difference = std::mismatch(
        Image.front().Bytes.begin(), Image.front().Bytes.end(), Native.RAM);
    ASSERT_EQ(Difference.first, Image.front().Bytes.end())
        << llvm::formatv("RAM at {0:x}: model={1:x}, CPU={2:x}",
                         Difference.first - Image.front().Bytes.begin(),
                         unsigned(*Difference.first),
                         unsigned(*Difference.second))
               .str();
  }
};

TEST_F(X86ModeSegmentNativeTest, OriginalMOVAndLxSRetainNonCodeLDMetadata) {
  unsigned Comparisons = 0;
  for (Mode M : Modes)
    for (unsigned S : {0u, 2u, 3u, 4u, 5u})
      for (uint16_t Attributes : {0xa093u, 0xe093u, 0xa09bu, 0xe09bu})
        for (bool Far : {false, true}) {
          if (S == 2 && (Attributes & 8))
            continue;
          SCOPED_TRACE((unsigned(M) * 6 + S) * 0x20000 + Attributes * 2 + Far);
          // Each independent stream has a fresh TLB and one initial SREGS
          // write.
          X64PagedNativeCPU Native;
          ASSERT_NO_FATAL_FAILURE(Native.initialize());
          if (IsSkipped())
            return;
          ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
          start(M);
          put(0x8020, descriptor(0x22000, UINT32_MAX, Attributes));
          registers().GPR[0] = 32;
          put(0x2000, uint64_t(32) << 32 | 0x12345678);
          const std::vector<uint8_t> Opcode =
              S == 0   ? std::vector<uint8_t>{0xc4}
              : S == 3 ? std::vector<uint8_t>{0xc5}
                       : std::vector<uint8_t>{0x0f, uint8_t(S == 2   ? 0xb2
                                                            : S == 4 ? 0xb4
                                                                     : 0xb5)};
          const auto Code =
              Far ? absolute(M, Opcode, 0, 0x2000, 4)
                  : std::vector<uint8_t>{0x8e, uint8_t(0xc0 | S << 3)};
          const auto PC = add(0x1000, M, Code);
          add(PC, M, {0xf4});
          ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
          ASSERT_NO_FATAL_FAILURE(Native.run());
          ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          uint64_t Budget = Program.size();
          auto Got =
              X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
          ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
          ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
          ASSERT_EQ(Got->Status, X86LegacyExecutionStatus::Halted);
          ASSERT_EQ(Got->Completions, Program.size());
          ASSERT_NO_FATAL_FAILURE(compareOriginal(Native));
          ++Comparisons;
        }
  RecordProperty("original_non64_segment_comparisons", Comparisons);
  RecordProperty("original_initial_sregs_writes", Comparisons);
  RecordProperty("original_mode_rewrites", 0);
}

TEST_F(X86ModeSegmentNativeTest,
       NullCompatibilityBaseIsObservedAfterOriginalFarJump) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (unsigned S : {4u, 5u})
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true})
          for (const std::string Optimization : {"-O0", "-O2"}) {
            SCOPED_TRACE(unsigned(M) * 6 + S);
            // Each independent stream has a fresh TLB and one initial SREGS
            // write.
            X64PagedNativeCPU Native;
            ASSERT_NO_FATAL_FAILURE(Native.initialize());
            if (IsSkipped())
              return;
            ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
            start(M);
            // A complete base survives entry to compatibility. Upper bits are
            // ignored there; the original far JMP later makes them observable.
            setCache(S, {0x100022000ULL, UINT32_MAX, 16, 0xc093});
            put(0x11020, 0x12003);
            put(0, 0xaabbccdd11223344ULL);
            put(0x22000, 0x9988776655443322ULL);
            registers().GPR[0] = 0;
            uint64_t PC = add(0x1000, M, {0x8e, uint8_t(0xc0 | S << 3)});
            if (code32(M))
              add(PC, M, {0xea, 0, 0x11, 0, 0, 0x18, 0});
            else
              add(PC, M, {0xea, 0, 0x11, 0x18, 0});
            PC = add(0x1100, Mode::Long64,
                     {uint8_t(S == 4 ? 0x64 : 0x65), 0x48, 0x8b, 0x04, 0x25, 0,
                      0, 0, 0});
            PC = add(PC, Mode::Long64, {0x48, 0x89, 0x04, 0x25, 0, 0x30, 2, 0});
            RAM.Memory.front().Bytes[PC] = 0xf4;
            ASSERT_NO_FATAL_FAILURE(compileLong(LLVM, Optimization, Compiler));
            ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
            ASSERT_NO_FATAL_FAILURE(Native.run());
            ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            uint64_t Budget = Program.size();
            auto Got = X86ModeExecution(*Memory, Options)
                           .run(Machine, Program, Budget);
            ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
            ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
            ASSERT_TRUE(Got->BudgetExhausted);
            ASSERT_EQ(Got->Completions, Program.size());
            ASSERT_NO_FATAL_FAILURE(compareOriginal(Native, true));
            EXPECT_EQ(physical(0x23000, GuestScalarWidth::I64),
                      0xaabbccdd11223344ULL);
            ++Comparisons;
          }
  RecordProperty("original_non64_null_base_source_comparisons", Comparisons);
  RecordProperty("original_initial_sregs_writes", Comparisons);
  RecordProperty("original_mode_rewrites", 0);
}

TEST_F(X86ModeSegmentNativeTest, OriginalDescriptorFaultAddressAndPriority) {
  unsigned Comparisons = 0;
  for (Mode M : {Mode::Compatibility16, Mode::Compatibility32})
    for (unsigned Case = 0; Case != 4; ++Case)
      for (bool Far : {false, true}) {
        SCOPED_TRACE((unsigned(M) * 4 + Case) * 2 + Far);
        X64PagedNativeCPU Native;
        ASSERT_NO_FATAL_FAILURE(Native.initialize());
        if (IsSkipped())
          return;
        ASSERT_NO_FATAL_FAILURE(Native.pinForReturnComparison());
        start(M);
        if (Native.ReturnCoreType != 0x20 && Native.ReturnCoreType != 0x40)
          GTEST_SKIP() << "Descriptor-tail qualification needs the declared "
                          "hybrid core profile";
        Options.Legacy.Segments.DescriptorCanonicality =
            Native.ReturnCoreType == 0x20
                ? X86DescriptorCanonicalityProfile::SelectorForRange
                : X86DescriptorCanonicalityProfile::SelectorForStart;
        const uint64_t Address = Case == 0  ? 0x0000800000000000ULL
                                 : Case < 3 ? 0x00007ffffffffffcULL
                                            : 0xffff800000008020ULL;
        // Keep the GDT, IDT and handler stack valid. Only the loaded LDT's
        // target descriptor is bad, so the original fault frame is observable.
        Machine.Core.Core.LDTR = {Address - 0x20, 0x3f, 0x30, 0x82};
        Machine.Core.Core.Unusable &= ~64;
        constexpr uint64_t OldCR2 = 0x1122334455667788ULL;
        Machine.Core.Core.Core.Core.CR2 = OldCR2;
        if (Case == 1 || Case == 2) {
          put(0x107f8, 0x14003);
          put(0x14ff8, 0x15003);
          put(0x15ff8, 0x16003);
          put(0x16ff8, Case == 1 ? 0x17003 : 0x17002);
        }
        registers().GPR[0] = registers().Undefined[0] | 0x24;
        put(0x2000, uint64_t(0x24) << 32 | 0x12345678);
        const auto Before = cache(4);
        const auto OldGPR = registers().GPR[0];
        add(0x1000, M,
            Far ? absolute(M, {0x0f, 0xb4}, 0, 0x2000, 4)
                : std::vector<uint8_t>{0x8e, 0xe0});
        ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
        ASSERT_NO_FATAL_FAILURE(Native.run());
        ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_HLT));
        kvm_regs G{};
        kvm_sregs S{};
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &G), 0) << errno;
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &S), 0) << errno;
        ASSERT_TRUE(G.rip == 0x40d1 || G.rip == 0x40e1);
        ASSERT_LT(G.rsp + 8, X64PagedNativeCPU::Size);
        uint64_t Error = 0;
        for (unsigned I = 0; I != 8; ++I)
          Error |= uint64_t(Native.RAM[G.rsp + I]) << (I * 8);
        RecordProperty(
            "descriptor_fault_" + std::to_string(unsigned(M)) + "_" +
                std::to_string(Case) + "_" + std::to_string(Far),
            llvm::formatv("pc={0:x};error={1:x};cr2={2:x};pml4={3:x};pte={4:x}",
                          G.rip - 1, Error, S.cr2,
                          unsigned(Native.RAM[0x107f8]),
                          unsigned(Native.RAM[0x16ff8]))
                .str());
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        uint64_t Budget = 1;
        auto Got =
            X86ModeExecution(*Memory, Options).run(Machine, Program, Budget);
        ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
        ASSERT_TRUE(Got->OwnerError.empty()) << Got->OwnerError;
        ASSERT_TRUE(Got->Exception);
        EXPECT_EQ(Got->Exception->Vector, (G.rip - 0x4001) / 16);
        EXPECT_EQ(Got->Exception->ErrorCode, Error);
        EXPECT_EQ(Got->Completions, 0u);
        EXPECT_FALSE(Got->SegmentWritten);
        EXPECT_EQ(cache(4), Before);
        EXPECT_EQ(registers().GPR[0], OldGPR);
        EXPECT_FALSE(Machine.SSBlocked);
        EXPECT_FALSE(Machine.Pending);
        compareOriginal(Native, true);
        ++Comparisons;
      }
  RecordProperty("original_non64_descriptor_fault_comparisons", Comparisons);
  RecordProperty("original_initial_sregs_writes", Comparisons);
  RecordProperty("original_mode_rewrites", 0);
}
#endif
