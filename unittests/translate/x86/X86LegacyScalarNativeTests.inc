//===- X86LegacyScalarNativeTests.inc - Original protected scalars --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent KVM scalar and original page-fault-handler execution.
//===----------------------------------------------------------------------===//

class X86LegacyScalarNativeTest : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;
  std::optional<X86LegacyCallProfile> Calls;

  void SetUp() override {
    X86PagingNativeTest::SetUp();
    const char *Bound = std::getenv("NEVERD_TEST_LEGACY_CALL");
    if (!Bound)
      return;
    if (!std::strcmp(Bound, "store"))
      Calls = X86LegacyCallProfile::StoreBeforeTargetLimit;
    else if (!std::strcmp(Bound, "check"))
      Calls = X86LegacyCallProfile::CheckBeforeTargetLimit;
    else
      FAIL() << "NEVERD_TEST_LEGACY_CALL must be store or check";
  }

  void compare(const std::vector<uint8_t> &Bytes, bool Code32 = true,
               bool Stack32 = true, unsigned CPL = 3, unsigned Case = 0,
               uint32_t Left = 0x12345678, uint32_t Right = 0x80,
               uint32_t Flags = 0x202, uint32_t Target = 0x4000) {
    using namespace neverd;
    SCOPED_TRACE(testing::Message()
                 << "code32=" << Code32 << " stack32=" << Stack32
                 << " cpl=" << CPL << " case=" << Case << " left=" << std::hex
                 << Left << " right=" << Right << " flags=" << Flags);
    SCOPED_TRACE(testing::PrintToString(Bytes));
    const bool Legacy = GetParam() == 2;
    const unsigned PW = Legacy ? 4 : 8;
    const uint32_t PT = Legacy ? 0x11000 : 0x12000;
    constexpr uint32_t GDT = 0x7000, IDT = 0x6000, Stop = 0x5000;
    constexpr uint32_t Record = 0x4800;
    std::memset(RAM, 0, MemorySize);
    put(0x10000, 0x11000 | (Legacy ? 7 : 1), PW);
    if (!Legacy)
      put(0x11000, 0x12007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * PW, I * 4096 | 7, PW);
    put(GDT + 8, 0x00cf9b000000ffffULL);
    put(GDT + 16, 0x00cf93000000ffffULL);
    put(GDT + 24, (Code32 ? 0x00cf9b000000ffffULL : 0x008f9b000000ffffULL) |
                      (uint64_t(CPL) << 45));
    put(GDT + 32, 0x00cf93000000ffffULL | (uint64_t(CPL) << 45));
    put(0x5804, 0xb000, 4);
    put(0x5808, 16, 2);
    for (unsigned V = 0; V != 256; ++V) {
      const uint32_t Entry = 0x2000 + V * 16;
      put(IDT + V * 8, Entry | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
      put(Entry, 0x05c736, 3);
      put(Entry + 3, Record, 4);
      put(Entry + 7, V, 4);
      RAM[Entry + 11] = 0xe9;
      put(Entry + 12, Stop - Entry - 16, 4);
    }
    std::copy(Bytes.begin(), Bytes.end(), RAM + Code);
    RAM[Code + Bytes.size()] = RAM[Target] = RAM[Stop] = 0xf4;
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    const uint32_t Data = Offset + (Case == 3 ? 0x10000 : 0);
    put(Data, Left, 4);
    put(0xd100, (Case == 11 || Case == 14 || Case == 15) ? Target : Left, 4);
    if (Case == 1 || Case == 8 || Case == 12)
      put(PT + (Data >> 12) * PW, Data & ~4095u | 6, PW);
    if (Case == 2)
      put(PT + (Data >> 12) * PW, Data & ~4095u | 5, PW);
    if (Case == 9 || Case == 15 || Case == 16)
      put(PT + 13 * PW, 0xd006, PW);
    std::vector<std::vector<uint8_t>> Repair;
    if (Case == 12) {
      const uint32_t PTE = PT + (Data >> 12) * PW;
      Repair = {{0xc7, 0x05, uint8_t(PTE), uint8_t(PTE >> 8),
                 uint8_t(PTE >> 16), 0, 7, 0xc0, 0, 0},
                {0x83, 0xc4, 4},
                {0xcf}};
      uint32_t PC = 0x2000 + 14 * 16;
      for (const auto &I : Repair) {
        std::copy(I.begin(), I.end(), RAM + PC);
        PC += I.size();
      }
    }
    kvm_sregs Special = Initial;
    Special.cr0 = 0x80050011;
    Special.cr3 = 0x10000;
    Special.cr4 = Legacy ? 0 : 0x20;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x76543210;
    Special.cs = segment(24 | CPL, true, false, CPL);
    Special.cs.db = Code32;
    if (Case == 14 || Case == 15)
      Special.cs.limit = 0x3fff;
    Special.ds = Special.es = Special.ss = Special.fs = Special.gs =
        segment(32 | CPL, false, false, CPL);
    Special.ss.db = Stack32;
    if (Case == 3)
      Special.ds.base = Special.es.base = Special.fs.base = Special.gs.base =
          0x10000;
    if (Case == 4) {
      Special.ds.g = 0;
      Special.ds.limit = 0xc0ff;
    }
    if (Case == 5)
      Special.ds.unusable = 1;
    if (Case == 6) {
      Special.ss.g = 0;
      Special.ss.limit = 0xc0ff;
    }
    Special.gdt = {GDT, 0xff, {}};
    Special.idt = {IDT, 0x7ff, {}};
    Special.ldt = {};
    Special.ldt.unusable = 1;
    Special.tr = {};
    Special.tr.base = 0x5800;
    Special.tr.limit = 0x67;
    Special.tr.selector = 40;
    Special.tr.type = 11;
    Special.tr.present = 1;
    kvm_regs Registers{};
    Registers.rip = Code;
    Registers.rsp = Case == 13 ? 0xabcd0000 : 0xd100;
    if (Case == 17) {
      const bool Operand32 = Code32 != (std::find(Bytes.begin(), Bytes.end(),
                                                  0x66) != Bytes.end());
      const unsigned Width = Operand32 ? 4 : 2;
      Registers.rsp = 0xabce0000u - Width;
      put(uint16_t(Registers.rsp), Left, Width);
    }
    Registers.rax = Left;
    Registers.rcx = Right;
    Registers.rdx = 0x22334455;
    Registers.rbx = Registers.rdi = Registers.rbp = Offset;
    Registers.rsi = 0;
    Registers.rflags = Flags;
    const auto Cache = [](const kvm_segment &C) {
      return X86LegacySegmentCache{uint32_t(C.base), C.limit, C.selector,
                                   uint16_t(C.type | C.s << 4 | C.dpl << 5 |
                                            C.present << 7 | C.avl << 12 |
                                            C.l << 13 | C.db << 14 | C.g << 15),
                                   bool(C.unusable)};
    };
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    X86LegacyExecutionState State;
    State.Core.GPR = {Left,
                      Right,
                      uint32_t(Registers.rdx),
                      Offset,
                      uint32_t(Registers.rsp),
                      Offset,
                      0,
                      Offset};
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.CPL = CPL;
    State.Core.GDTRBase = GDT;
    State.Core.GDTRLimit = 0xff;
    State.Core.LDTR = Cache(Special.ldt);
    State.TR = Cache(Special.tr);
    State.IDTRBase = IDT;
    State.IDTRLimit = 0x7ff;
    State.EIP = Code;
    State.EFlags = Flags;
    State.CR2 = Special.cr2;
    X86PagingState Controls{.CR0 = Special.cr0,
                            .CR3 = Special.cr3,
                            .CR4 = Special.cr4,
                            .EFER = Special.efer,
                            .RFlags = Flags,
                            .PDPTE = {0x11001, 0, 0, 0}};
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features, Controls));
    X86LegacyExecutionOptions Options{
        {X86LegacySegmentProfile::WordStackOffsetFirst,
         X86AlignmentPriority::BeforePaging, X86FullSegmentLimitChoice::Fault},
        X86LegacyEventProfile::OrderedFrame,
        false,
        X86LegacyReturnProfile::OrderedFrame,
        false,
        X86LegacyScalarProfile::OrderedRAM,
        Calls};
    auto Expected = executeX86LegacyInstruction(State, *Memory, Bytes, Options);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    if (Case == 12) {
      ASSERT_EQ(Expected->DeliveredVector, 14);
      for (const auto &I : Repair) {
        Expected = executeX86LegacyInstruction(State, *Memory, I, Options);
        ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
        ASSERT_TRUE(Expected->InstructionCompleted);
      }
      ASSERT_EQ(State.EIP, Code);
      Expected = executeX86LegacyInstruction(State, *Memory, Bytes, Options);
      ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
      ASSERT_TRUE(Expected->InstructionCompleted);
    }
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = Code + Bytes.size();
    Debug.arch.debugreg[1] = Target;
    Debug.arch.debugreg[2] = Stop;
    Debug.arch.debugreg[7] = 0x415;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    if (Expected->Status == X86LegacyExecutionStatus::Shutdown) {
      ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_SHUTDOWN));
      ++Comparisons;
      return;
    }
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    if (Expected->InstructionCompleted) {
      EXPECT_EQ(Registers.rip, State.EIP);
    } else {
      ASSERT_EQ(Expected->Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Registers.rip, Stop);
      EXPECT_EQ(get(Record, 4), Expected->DeliveredVector);
    }
    const std::array<uint32_t, 8> GPR{
        uint32_t(Registers.rax), uint32_t(Registers.rcx),
        uint32_t(Registers.rdx), uint32_t(Registers.rbx),
        uint32_t(Registers.rsp), uint32_t(Registers.rbp),
        uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
    EXPECT_EQ(GPR, State.Core.GPR);
    EXPECT_EQ(Registers.rflags & ~uint64_t{0x10000},
              State.EFlags & ~uint32_t{0x10000});
    EXPECT_EQ(Special.cr2, State.CR2);
    if (Case == 14 || Case == 15) {
      const auto PTE = Memory->physicalMemory().loadScalar(
          PT + 13 * PW, Legacy ? GuestScalarWidth::I32 : GuestScalarWidth::I64);
      ASSERT_EQ(PTE.Status, GuestMemoryAccessStatus::Completed);
      EXPECT_EQ(get(PT + 13 * PW, PW) & 0x60, PTE.Value & 0x60);
    }
    for (unsigned Begin :
         {0xaff0u - 16, 0xc0f0u, 0xcfe0u, 0xd0e0u, 0xffe0u, 0x1c0f0u}) {
      for (unsigned I = 0; I != 64; ++I) {
        // The 16-bit wrapping stack can approach the page-directory page;
        // hardware translation caches make its A/D bits a separate contract.
        if (Begin + I >= 0x10000 && Begin + I < 0x10020)
          continue;
        auto Got = Memory->physicalMemory().loadScalar(Begin + I,
                                                       GuestScalarWidth::I8);
        ASSERT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
        EXPECT_EQ(get(Begin + I, 1), Got.Value) << std::hex << Begin + I;
      }
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacyScalarNativeTest, ArithmeticRegistersCarryBorrowAndFlags) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {1u, 2u, 4u})
      for (unsigned Op = 0; Op != 8; ++Op)
        for (uint32_t A : {0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u,
                           0xffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu})
          for (uint32_t B : {0u, 1u, 0xffffffffu, 0x80000000u})
            for (unsigned CF : {0u, 1u}) {
              std::vector<uint8_t> Bytes;
              if (Width != 1 && (Width == 4) != Code32)
                Bytes.push_back(0x66);
              Bytes.push_back(Op * 8 + (Width == 1 ? 0 : 1));
              Bytes.push_back(0xc8); // r/m = A, source = C.
              ASSERT_NO_FATAL_FAILURE(
                  compare(Bytes, Code32, true, 3, 0, A, B, 0x202 | CF));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, LoadsStoresExtensionsAndAddressing) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {1u, 2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned Form = 0; Form != 9; ++Form) {
            std::vector<uint8_t> B;
            if (Width != 1 && (Width == 4) != Code32)
              B.push_back(0x66);
            if (Address32 != Code32)
              B.push_back(0x67);
            if (Form == 0 || Form == 1) {
              B.push_back((Form ? 0x88 : 0x8a) + (Width != 1));
              B.push_back(7); // [EDI] or [BX].
            } else if (Form == 2) {
              B.push_back(Width == 1 ? 0xc6 : 0xc7);
              B.push_back(7);
              for (unsigned I = 0; I != Width; ++I)
                B.push_back(0xa5 + I);
            } else if (Form == 3 || Form == 4) {
              B.push_back((Form == 3 ? 0xa0 : 0xa2) + (Width != 1));
              B.push_back(0);
              B.push_back(0xc1);
              if (Address32) {
                B.push_back(0);
                B.push_back(0);
              }
            } else if (Form == 5 || Form == 6) {
              if (Width == 1)
                continue;
              B.push_back(0x0f);
              B.push_back(Form == 5 ? 0xbe : 0xb6);
              B.push_back(7);
            } else {
              if (Width == 1)
                continue;
              B.push_back(Form == 7 ? 0x8d : 0x8b);
              B.push_back(Address32 ? 0x44 : 0x40);
              if (Address32)
                B.push_back(0x73); // [EBX+ESI*2+0].
              B.push_back(0);
            }
            ASSERT_NO_FATAL_FAILURE(compare(B, Code32, true, CPL));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, UnaryStackAndStackPointerAliases) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (bool Stack32 : {false, true})
        for (unsigned CPL : {0u, 3u})
          for (const std::vector<uint8_t> Body : {std::vector<uint8_t>{0x40},
                                                  {0x48},
                                                  {0xf7, 0xd0},
                                                  {0xf7, 0xd8},
                                                  {0x54},
                                                  {0x50},
                                                  {0x58},
                                                  {0x5c},
                                                  {0x6a, 0xff},
                                                  {0xff, 0x37},
                                                  {0x8f, 0x04, 0x24}}) {
            auto B = Body;
            if ((Body == std::vector<uint8_t>{0xff, 0x37} || Body[0] == 0x8f) &&
                !Code32)
              B.insert(B.begin(), 0x67);
            if (Operand32 != Code32)
              B.insert(B.begin(), 0x66);
            ASSERT_NO_FATAL_FAILURE(
                compare(B, Code32, Stack32, CPL, 0, 0x7fffffffu, 0, 0xa97));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, OperandFaultsAndUndefinedFlagsPublication) {
  for (unsigned CPL : {0u, 3u})
    for (unsigned Case : {1u, 2u, 3u, 4u, 5u, 9u, 16u})
      for (const std::vector<uint8_t> B : {std::vector<uint8_t>{0x8b, 7},
                                           {0x89, 7},
                                           {0x83, 7, 1},
                                           {0x83, 0x3f, 1},
                                           {0x83, 0x27, 0},
                                           {0xff, 0x37},
                                           {0x8f, 7},
                                           {0xf7, 0x17},
                                           {0xf7, 0x1f},
                                           {0x50}})
        ASSERT_NO_FATAL_FAILURE(
            compare(B, true, true, CPL, Case, 0x12345678, 0x80, 0xa97));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, AlignmentAndPagingPriority) {
  const char *Bound = std::getenv("NEVERD_TEST_LEGACY_ALIGNMENT");
  if (!Bound || std::strcmp(Bound, "before"))
    GTEST_SKIP() << "Bind the qualified scalar #AC-before-paging processor";
  for (unsigned Case : {7u, 8u})
    for (const std::vector<uint8_t> B : {std::vector<uint8_t>{0x8b, 7},
                                         {0x89, 7},
                                         {0x83, 7, 1},
                                         {0x83, 0x3f, 1},
                                         {0xff, 0x37},
                                         {0x8f, 7}})
      ASSERT_NO_FATAL_FAILURE(
          compare(B, true, true, 3, Case, 0x12345678, 0x80, 0x40202));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, NearBranchesCallsReturnsAndEveryCondition) {
  if (!Calls)
    GTEST_SKIP() << "Bind the processor's CALL stack publication profile";
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (bool Stack32 : {false, true}) {
        const unsigned N = Operand32 ? 4 : 2;
        for (unsigned Kind = 0; Kind != 7; ++Kind) {
          std::vector<uint8_t> B;
          if (Code32 != Operand32)
            B.push_back(0x66);
          if (Kind <= 1) {
            const uint32_t Displacement = 0x4000 - (Code + B.size() + 1 + N);
            B.push_back(Kind ? 0xe8 : 0xe9);
            for (unsigned I = 0; I != N; ++I)
              B.push_back(Displacement >> (I * 8));
          } else if (Kind <= 4) {
            B.push_back(0xff);
            B.push_back(Kind == 2 ? 0xd0 : Kind == 3 ? 0xe0 : 0x17);
          } else {
            B.push_back(Kind == 5 ? 0xc3 : 0xc2);
            if (Kind == 6) {
              B.push_back(7);
              B.push_back(0);
            }
          }
          ASSERT_NO_FATAL_FAILURE(compare(B, Code32, Stack32, 3, 11, 0x4000));
        }
        for (unsigned CC = 0; CC != 16; ++CC)
          for (unsigned Mask = 0; Mask != 32; ++Mask) {
            std::vector<uint8_t> B;
            if (Code32 != Operand32)
              B.push_back(0x66);
            const uint32_t D = 0x4000 - (Code + B.size() + 2 + N);
            B.push_back(0x0f);
            B.push_back(0x80 | CC);
            for (unsigned I = 0; I != N; ++I)
              B.push_back(D >> (I * 8));
            const unsigned Flags = 0x202 | (Mask & 1) | ((Mask & 2) << 1) |
                                   ((Mask & 4) << 4) | ((Mask & 8) << 4) |
                                   ((Mask & 16) << 7);
            ASSERT_NO_FATAL_FAILURE(
                compare(B, Code32, Stack32, 3, 0, 0x12345678, 0x80, Flags));
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, OriginalPageFaultHandlerRepairsAndReturns) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (bool Code32 : {false, true})
      for (bool Stack32 : {false, true}) {
        std::vector<uint8_t> B{0x8b, 7};
        ASSERT_NO_FATAL_FAILURE(compare(B, Code32, Stack32, CPL, 12));
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, BranchLimitsPrecedeOrFollowStackFaults) {
  if (!Calls)
    GTEST_SKIP() << "Bind the processor's CALL stack publication profile";
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true})
      for (unsigned Case : {14u, 15u})
        for (const std::vector<uint8_t> Body :
             {std::vector<uint8_t>{0xff, 0xd0}, {0xff, 0xe0}, {0xc3}, {0xe8}}) {
          auto B = Body;
          if (Operand32 != Code32)
            B.insert(B.begin(), 0x66);
          if (Body[0] == 0xe8) {
            const unsigned Width = Operand32 ? 4 : 2;
            const uint32_t Relative = 0x4000 - (Code + B.size() + Width);
            for (unsigned I = 0; I != Width; ++I)
              B.push_back(Relative >> (I * 8));
          }
          ASSERT_NO_FATAL_FAILURE(compare(B, Code32, true, 3, Case, 0x4000));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, HighByteRegistersAndExtensions) {
  for (bool Code32 : {false, true})
    for (unsigned Register = 0; Register != 8; ++Register)
      for (unsigned Target = 0; Target != 8; ++Target)
        for (uint8_t Opcode : {0x00, 0x10, 0x18, 0x28, 0x38, 0x88, 0x8a})
          ASSERT_NO_FATAL_FAILURE(compare(
              {Opcode, uint8_t(0xc0 | Register << 3 | Target)}, Code32));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, WordStackWrapAndPopMemoryPostIncrement) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      std::vector<uint8_t> Push{0x54};
      if (Operand32 != Code32)
        Push.insert(Push.begin(), 0x66);
      ASSERT_NO_FATAL_FAILURE(compare(Push, Code32, false, 3, 13));
      std::vector<uint8_t> Pop{0x8f, 0x04, 0x24};
      if (!Code32)
        Pop.insert(Pop.begin(), 0x67);
      if (Operand32 != Code32)
        Pop.insert(Pop.begin(), 0x66);
      ASSERT_NO_FATAL_FAILURE(compare(Pop, Code32, false, 3, 17));
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyScalarNativeTest, ShortBranchesAndImmediateEncodings) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      const unsigned N = Operand32 ? 4 : 2;
      for (unsigned CC = 0; CC != 17; ++CC)
        for (unsigned Flags : {0x202u, 0xad7u}) {
          std::vector<uint8_t> B;
          if (Operand32 != Code32)
            B.push_back(0x66);
          const unsigned Relative = 0x40 - B.size() - 2;
          B.push_back(CC == 16 ? 0xeb : 0x70 | CC);
          B.push_back(Relative);
          ASSERT_NO_FATAL_FAILURE(compare(B, Code32, true, 3, 0, 0x12345678,
                                          0x80, Flags, Code + 0x40));
        }
      for (unsigned Op = 0; Op != 8; ++Op)
        for (uint32_t Value : {0u, 1u, 0x7fu, 0x80u, 0xffu})
          for (unsigned Form = 0; Form != 3; ++Form) {
            std::vector<uint8_t> B;
            if (Operand32 != Code32)
              B.push_back(0x66);
            B.push_back(Form == 0 ? Op * 8 + 5 : Form == 1 ? 0x81 : 0x83);
            if (Form)
              B.push_back(0xc0 | Op << 3);
            for (unsigned I = 0; I != (Form == 2 ? 1 : N); ++I)
              B.push_back(Value >> (I * 8));
            ASSERT_NO_FATAL_FAILURE(
                compare(B, Code32, true, 3, 0, 0x80000000, 0x80, 0x203));
          }
    }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyScalars, X86LegacyScalarNativeTest,
                         testing::Values(2u, 3u));
