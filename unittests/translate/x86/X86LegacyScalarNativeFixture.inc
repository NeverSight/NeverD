//===- X86LegacyScalarNativeFixture.inc - Protected scalar CPU fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared original-byte scalar, divide and multiply execution and handler
/// retry. Included inside the native paging test translation unit after its
/// fixture.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSCALARNATIVEFIXTURE_INC
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSCALARNATIVEFIXTURE_INC

class X86LegacyScalarNativeFixture : public X86PagingNativeTest {
protected:
  unsigned Comparisons = 0;
  std::optional<X86LegacyCallProfile> Calls;
  std::optional<X86LegacyFarTransferProfile> FarTransfers;
  std::optional<X86LegacyMultiplyProfile> Multiply;
  std::optional<uint32_t> ShiftUndefinedFlags;
  std::optional<X86LegacyDoubleShiftUndefinedState> DoubleShiftUndefined;
  std::optional<uint32_t> BitTestUndefinedFlags;
  std::optional<X86LegacyBitScanChoices> BitScanChoices;
  std::optional<X86LegacyAtomicProfile> Atomics;
  std::optional<uint32_t> MemoryHigh;
  /// Bind only architectural undefined bits from the independent original CPU.
  /// All defined values, flags, faults and memory still come from the model.
  std::optional<uint32_t> DecimalUndefinedMask;
  bool CheckOperandPageEffects = false;
  std::optional<X86LegacyStringProfile> Strings;
  std::vector<std::pair<uint32_t, unsigned>> ExtraMemoryChecks;
  std::optional<X86LegacyEventKind> PendingEvent;
  bool STIShadow = false;
  std::optional<X86PortIOProfile> PortIO;
  bool KnownPortBatchRestart = false;
  unsigned PortBatchDiscrepancies = 0;

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
               uint32_t Flags = 0x202, uint32_t Target = 0x4000,
               uint32_t HighDividend = 0x22334455,
               uint32_t InstructionPC = Code,
               std::optional<std::array<uint32_t, 8>> InputRegisters = {},
               std::function<void(kvm_sregs &, kvm_regs &)> Configure = {}) {
    using namespace neverd;
    SCOPED_TRACE(testing::Message()
                 << "code32=" << Code32 << " stack32=" << Stack32
                 << " cpl=" << CPL << " case=" << Case << " pc=" << std::hex
                 << InstructionPC << " left=" << Left << " right=" << Right
                 << " flags=" << Flags);
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
    std::copy(Bytes.begin(), Bytes.end(), RAM + InstructionPC);
    RAM[InstructionPC + Bytes.size()] = RAM[Target] = RAM[Stop] = 0xf4;
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    const uint32_t Data = Offset + (Case == 3 ? 0x10000 : 0);
    put(Data, Left, 4);
    if (MemoryHigh)
      put(Data + 4, *MemoryHigh, 4);
    put(0xd100, (Case == 11 || Case == 14 || Case == 15) ? Target : Left, 4);
    if (Case == 1 || Case == 8 || Case == 12)
      put(PT + (Data >> 12) * PW, Data & ~4095u | 6, PW);
    if (Case == 2)
      put(PT + (Data >> 12) * PW, Data & ~4095u | 5, PW);
    if (Case == 9 || Case == 15 || Case == 16 || Case == 21 || Case == 23)
      put(PT + 13 * PW, 0xd006, PW);
    std::vector<std::vector<uint8_t>> Repair;
    auto RetryBytes = Bytes;
    if (Case == 12 || Case == 21 || Case == 23) {
      const uint32_t Page = (Case == 21 || Case == 23) ? 0xd000 : Data & ~4095u;
      const uint32_t PTE = PT + (Page >> 12) * PW;
      Repair = {{0xc7, 0x05, uint8_t(PTE), uint8_t(PTE >> 8),
                 uint8_t(PTE >> 16), 0, uint8_t(Page | 7), uint8_t(Page >> 8),
                 uint8_t(Page >> 16), 0},
                {0x83, 0xc4, 4},
                {0xcf}};
      uint32_t PC = 0x2000 + 14 * 16;
      for (const auto &I : Repair) {
        std::copy(I.begin(), I.end(), RAM + PC);
        PC += I.size();
      }
    }
    if (Case == 22) {
      Repair = {{0xb9, 0, 0, 0, 0}, {0xcf}};
      uint32_t PC =
          0x2000 + 5 * 16; // #BR preserves flags and has no error code.
      for (const auto &I : Repair) {
        std::copy(I.begin(), I.end(), RAM + PC);
        PC += I.size();
      }
    }
    if (Case == 18) {
      Repair = {{0xb9, 3, 0, 0, 0}, {0xcf}};
      uint32_t PC = 0x2000; // #DE has no error code to discard.
      for (const auto &I : Repair) {
        std::copy(I.begin(), I.end(), RAM + PC);
        PC += I.size();
      }
    }
    if (Case == 20) {
      const uint32_t Immediate = InstructionPC + Bytes.size() - 1;
      Repair = {{0xc6, 0x05, uint8_t(Immediate), uint8_t(Immediate >> 8),
                 uint8_t(Immediate >> 16), uint8_t(Immediate >> 24), 10},
                {0xcf}};
      RetryBytes.back() = 10;
      uint32_t PC = 0x2000;
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
    if (Case == 19) {
      Special.cs.g = 0;
      Special.cs.limit = InstructionPC + 0x1f;
    }
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
    Registers.rip = InstructionPC;
    Registers.rsp = Case == 13 ? 0xabcd0000 : 0xd100;
    if (Case == 17) {
      const bool Operand32 = Code32 != (std::find(Bytes.begin(), Bytes.end(),
                                                  0x66) != Bytes.end());
      const unsigned Width = Operand32 ? 4 : 2;
      Registers.rsp = 0xabce0000u - Width;
      put(uint16_t(Registers.rsp), Left, Width);
    }
    Registers.rflags = Flags;
    if (Configure)
      Configure(Special, Registers);
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
    State.Core.GPR = InputRegisters.value_or(
        std::array<uint32_t, 8>{Left, Right, HighDividend, Offset,
                                uint32_t(Registers.rsp), Offset, 0, Offset});
    Registers.rax = State.Core.GPR[0];
    Registers.rcx = State.Core.GPR[1];
    Registers.rdx = State.Core.GPR[2];
    Registers.rbx = State.Core.GPR[3];
    Registers.rsp = State.Core.GPR[4];
    Registers.rbp = State.Core.GPR[5];
    Registers.rsi = State.Core.GPR[6];
    Registers.rdi = State.Core.GPR[7];
    const uint32_t StackLinear =
        Special.ss.base +
        (Special.ss.db ? uint32_t(Registers.rsp) : uint16_t(Registers.rsp));
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.CPL = CPL;
    State.Core.GDTRBase = Special.gdt.base;
    State.Core.GDTRLimit = Special.gdt.limit;
    State.Core.LDTR = Cache(Special.ldt);
    State.TR = Cache(Special.tr);
    State.IDTRBase = Special.idt.base;
    State.IDTRLimit = Special.idt.limit;
    State.EIP = InstructionPC;
    State.EFlags = Flags;
    State.CR2 = Special.cr2;
    State.SSBlocked = PendingEvent && !STIShadow;
    State.STIBlocked = PendingEvent && STIShadow;
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
    Options.DivisionArithmeticFlags = Flags & kX86LegacyArithmeticFlagsMask;
    Options.Multiply = Multiply;
    Options.ShiftUndefinedFlags = ShiftUndefinedFlags;
    Options.DoubleShiftUndefined = DoubleShiftUndefined;
    Options.BitTestUndefinedFlags = BitTestUndefinedFlags;
    Options.BitScanChoices = BitScanChoices;
    Options.Atomics = Atomics;
    Options.Strings = Strings;
    Options.FarTransfers = FarTransfers;
    PagedPortTape Tape, NativeTape;
    if (PortIO)
      Options.PortIO =
          X86PortIOBinding{*PortIO, PagedPortTape::transfer, &Tape};
    if (PendingEvent && STIShadow)
      Options.NMIBlockedBySTI = true;
    const uint8_t PendingVector =
        PendingEvent == X86LegacyEventKind::NMI ? 2 : 64;
    if (PendingEvent) {
      const auto Deferred = llvm::cantFail(deliverX86LegacyEvent(
          State, *Memory, {State.EIP, 0, 0, PendingVector, *PendingEvent},
          Options));
      ASSERT_EQ(Deferred.Status, X86LegacyExecutionStatus::EventDeferred);
    }
    auto Flush = Special;
    Flush.cr3 = 0x90000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    Events.interrupt.shadow = State.SSBlocked    ? KVM_X86_SHADOW_INT_MOV_SS
                              : State.STIBlocked ? KVM_X86_SHADOW_INT_STI
                                                 : 0;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug Debug{};
    Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = InstructionPC + Bytes.size();
    Debug.arch.debugreg[1] = Target;
    Debug.arch.debugreg[2] = Stop;
    Debug.arch.debugreg[7] = PendingEvent ? 0x410 : 0x415;
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    const auto RunToBoundary = [&] {
      do {
        ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
        if (Run->exit_reason != KVM_EXIT_IO)
          return;
        ASSERT_TRUE(PortIO.has_value());
        PagedPortTape::kvm(&NativeTape, Run->io.direction == KVM_EXIT_IO_IN,
                           Run->io.port, Run->io.size, Run->io.count,
                           reinterpret_cast<uint8_t *>(Run) +
                               Run->io.data_offset);
      } while (true);
    };
    if (PendingEvent == X86LegacyEventKind::NMI)
      ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
    Run->request_interrupt_window =
        PendingEvent == X86LegacyEventKind::Interrupt;
    if (Run->request_interrupt_window) {
      ASSERT_NO_FATAL_FAILURE(RunToBoundary());
      ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_IRQ_WINDOW_OPEN));
      Run->request_interrupt_window = 0;
      kvm_interrupt IRQ{PendingVector};
      ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &IRQ), 0);
    }
    ASSERT_NO_FATAL_FAILURE(RunToBoundary());
    if (DecimalUndefinedMask) {
      ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
      Options.DecimalUndefinedFlags = Registers.rflags & *DecimalUndefinedMask;
    }
    // REP is one committed element per model call. Native code runs freely;
    // for asynchronous cases replay its observed count boundary, without
    // deriving operands, results, flags or memory from the native execution.
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    const bool Address32 =
        Code32 != (std::find(Bytes.begin(), Bytes.end(), 0x67) != Bytes.end());
    const uint32_t CountMask = Address32 ? UINT32_MAX : UINT16_MAX;
    const auto Execute = [&](const std::vector<uint8_t> &Instruction) {
      uint64_t Remaining = uint64_t(State.Core.GPR[1] & CountMask) + 1;
      auto R =
          executeX86LegacyInstruction(State, *Memory, Instruction, Options);
      while (R && R->Status == X86LegacyExecutionStatus::RepeatContinue &&
             --Remaining &&
             (!PendingEvent ||
              (State.Core.GPR[1] & CountMask) != (Registers.rcx & CountMask)))
        R = executeX86LegacyInstruction(State, *Memory, Instruction, Options);
      return R;
    };
    auto Expected = Execute(Bytes);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    if (PendingEvent) {
      ASSERT_TRUE(Expected->InstructionCompleted ||
                  Expected->Status == X86LegacyExecutionStatus::RepeatContinue);
      Expected = deliverX86LegacyEvent(
          State, *Memory, {State.EIP, 0, 0, PendingVector, *PendingEvent},
          Options);
      ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
      ASSERT_EQ(Expected->Status, X86LegacyExecutionStatus::EventDelivered);
    }
    // At CPL0 an unmapped original stack also prevents #PF/#DF entry.
    // Only an inner-ring stack switch can reach the Case 21 repair handler.
    if (Case == 12 || Case == 18 || Case == 20 || Case == 22 || Case == 23 ||
        (Case == 21 && CPL != 0)) {
      ASSERT_EQ(Expected->DeliveredVector,
                Case == 12 || Case == 21 || Case == 23 ? 14
                : Case == 22                           ? 5
                                                       : 0);
      for (const auto &I : Repair) {
        Expected = executeX86LegacyInstruction(State, *Memory, I, Options);
        ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
        ASSERT_TRUE(Expected->InstructionCompleted);
      }
      ASSERT_EQ(State.EIP, InstructionPC);
      Expected = Execute(RetryBytes);
      ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
      ASSERT_TRUE(Expected->InstructionCompleted);
    }
    unsigned BatchBytes = 0;
    uint32_t BatchAddress = 0;
    if (KnownPortBatchRestart) {
      // KVM batches a cross-page REP INS write, then retries the whole batch
      // after the handler. This is deliberately not a model/CPU equivalence.
      ASSERT_EQ(Tape.Transfers.size(), 3u);
      ASSERT_EQ(NativeTape.Transfers.size(), 4u);
      BatchBytes = Tape.Transfers.front().Bytes;
      BatchAddress = InputRegisters->at(7) & CountMask;
      PagedPortTape ExpectedTape;
      for (unsigned I = 0; I != 4; ++I)
        ExpectedTape.exchange(true, uint16_t(InputRegisters->at(2)), BatchBytes,
                              0);
      EXPECT_EQ(NativeTape.Transfers, ExpectedTape.Transfers);
      ExpectedTape.Transfers.pop_back();
      EXPECT_EQ(Tape.Transfers, ExpectedTape.Transfers);
      const uint32_t Mask = BatchBytes == 4 ? UINT32_MAX : UINT16_MAX;
      for (unsigned I = 0; I != 2; ++I) {
        const uint32_t Address = BatchAddress + I * BatchBytes;
        EXPECT_EQ(get(Address, BatchBytes),
                  (0x87654321u + (I + 2) * 0x123u) & Mask);
        auto Got = Memory->physicalMemory().loadScalar(
            Address,
            BatchBytes == 4 ? GuestScalarWidth::I32 : GuestScalarWidth::I16);
        ASSERT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
        EXPECT_EQ(Got.Value, (0x87654321u + I * 2 * 0x123u) & Mask);
      }
      ++PortBatchDiscrepancies;
    } else if (PortIO) {
      EXPECT_EQ(Tape.Transfers, NativeTape.Transfers);
    }
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
    for (unsigned I = 0; I != Segments.size(); ++I) {
      const auto Native = Cache(*Segments[I]);
      const auto &Model = State.Core.Segments[I];
      EXPECT_EQ(Native.Selector, Model.Selector);
      EXPECT_EQ(Native.Unusable, Model.Unusable);
      // VMX can normalize an unusable cache's hidden bits. They are opaque
      // in the owned contract; only the selector and unusable bit are defined.
      if (Model.Unusable)
        continue;
      EXPECT_EQ(Native.Base, Model.Base);
      EXPECT_EQ(Native.Limit, Model.Limit);
      EXPECT_EQ(Native.Attributes, Model.Attributes);
    }
    if (PendingEvent) {
      kvm_vcpu_events Final{};
      ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &Final), 0);
      EXPECT_EQ(bool(Final.nmi.masked), State.NMIBlocked);
    }
    // The shared page walker deliberately sets A on each valid entry before
    // a later permission fault. Hardware need not choose that faulting-walk
    // image; qualify exact A/D only for successful operand translations.
    if (CheckOperandPageEffects && Expected->InstructionCompleted) {
      for (uint32_t Address :
           {Offset, Offset + (MemoryHigh ? 7 : 3), Offset + 0x10000,
            Offset + (MemoryHigh ? 0x10007 : 0x10003)}) {
        const uint32_t PTE = PT + (Address >> 12) * PW;
        auto Got = Memory->physicalMemory().loadScalar(
            PTE, Legacy ? GuestScalarWidth::I32 : GuestScalarWidth::I64);
        ASSERT_EQ(Got.Status, GuestMemoryAccessStatus::Completed);
        EXPECT_EQ(get(PTE, PW) & 0x60, Got.Value & 0x60) << std::hex << Address;
      }
    }
    if (Case == 14 || Case == 15) {
      const auto PTE = Memory->physicalMemory().loadScalar(
          PT + 13 * PW, Legacy ? GuestScalarWidth::I32 : GuestScalarWidth::I64);
      ASSERT_EQ(PTE.Status, GuestMemoryAccessStatus::Completed);
      EXPECT_EQ(get(PT + 13 * PW, PW) & 0x60, PTE.Value & 0x60);
    }
    const auto &FinalStack = State.Core.Segments[2];
    const uint32_t FinalStackLinear =
        FinalStack.Base + ((FinalStack.Attributes & 0x4000)
                               ? State.Core.GPR[4]
                               : uint16_t(State.Core.GPR[4]));
    auto MemoryChecks = ExtraMemoryChecks;
    for (unsigned Begin : {0xaff0u - 16, 0xc0f0u, 0xcfe0u, 0xd0e0u, 0xffe0u,
                           0x1c0f0u, StackLinear >= 16 ? StackLinear - 16 : 0,
                           FinalStackLinear >= 16 ? FinalStackLinear - 16 : 0})
      MemoryChecks.emplace_back(Begin, 64);
    for (const auto &[Begin, Size] : MemoryChecks) {
      if (Begin > MemorySize - Size)
        continue;
      for (unsigned I = 0; I != Size; ++I) {
        // Both different batch images were independently checked above.
        if (KnownPortBatchRestart && Begin + I >= BatchAddress &&
            Begin + I < BatchAddress + 2 * BatchBytes)
          continue;
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
    if (!KnownPortBatchRestart)
      ++Comparisons;
  }
};

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSCALARNATIVEFIXTURE_INC
