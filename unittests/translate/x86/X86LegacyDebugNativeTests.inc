//===- X86LegacyDebugNativeTests.inc - Original protected debug traps -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run original TF/ICEBP sequences without KVM guest debugging. Real IDT
/// handlers stop through HLT after the CPU has published DR6 and its frame.
//===----------------------------------------------------------------------===//

class X86LegacyDebugNativeTest : public X86PagingNativeTest {
protected:
  void SetUp() override {
    if (!neverd::test::hostCpuIsIntel())
      GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
    X86PagingNativeTest::SetUp();
  }

  unsigned Comparisons = 0;
  unsigned RoundTrips = 0;

  struct ExecutionBreakpoint {
    uint32_t DR7 = 0x401;
    uint32_t Flags = 0x202;
    unsigned Offset = 0;
    bool FetchFault = false;
    uint32_t CSBase = 0;
    bool Resume = false;
    bool VirtualInterrupt = false;
  };

  struct RegisterAccess {
    std::vector<uint8_t> Bytes;
    unsigned General = 0;
    uint32_t Value = 0x2305, DR7 = 0x400, Flags = 0x2d7;
    bool DE = false, SS = false, FetchFault = false, Resume = false;
  };

  struct Watchpoint {
    std::vector<std::vector<uint8_t>> Instructions;
    std::array<uint32_t, 4> DR{};
    uint32_t DR7 = 0x400, Flags = 0x202;
  };

  enum Case {
    Nop,
    Increment,
    MovSS,
    ConsecutiveSS,
    PopSS,
    LoadSS,
    PopFlagsSet,
    PopFlagsClear,
    IretClear,
    IretSet,
    IretResume,
    FarReturn,
    NearCall,
    NearReturn,
    FarCall,
    FarJump,
    CallGate,
    JumpGate,
    TakenBranch,
    UntakenBranch,
    SetInterrupts,
    ClearInterrupts,
    Icebp,
    StackIcebp,
    SoftwareInterrupt,
    Breakpoint,
    SoftwareDebug,
    Overflow,
    NoOverflow,
    Repeat,
    ZeroRepeat,
    InvalidOpcode,
    OperandFault,
    InvalidSS,
    Sysenter,
    Sysexit,
    MissingDebugGate,
    DebugStackFault,
    DebugTargetLimit,
    ReturnAndStepAgain,
    RepeatAndReturn,
    RepairAndRetry,
    CaseCount
  };

  void compare(bool Code32, bool Stack32, bool Gate32, unsigned CPL, Case Which,
               uint32_t DR6, bool RF,
               std::optional<ExecutionBreakpoint> Execute = std::nullopt,
               std::optional<RegisterAccess> RegistersAccess = std::nullopt,
               std::optional<Watchpoint> Watch = std::nullopt) {
    SCOPED_TRACE(testing::Message()
                 << "case=" << Which << " code32=" << Code32 << " stack32="
                 << Stack32 << " gate32=" << Gate32 << " cpl=" << CPL
                 << " dr6=" << std::hex << DR6 << " rf=" << RF);
    const bool Legacy = GetParam() == 2;
    const unsigned PW = Legacy ? 4 : 8;
    const uint32_t PT = Legacy ? 0x11000 : 0x12000;
    const bool RoundTrip = Which >= ReturnAndStepAgain ||
                           (Execute && Execute->Resume) ||
                           (RegistersAccess && RegistersAccess->Resume);
    constexpr uint32_t GDT = 0x7000, IDT = 0x6000, TSS = 0x5800;
    constexpr uint32_t Target = 0x1800;
    std::memset(RAM, 0, MemorySize);
    put(0x10000, 0x11000 | (Legacy ? 7 : 1), PW);
    if (!Legacy)
      put(0x11000, 0x12007);
    for (unsigned I = 0; I != MemorySize / 4096; ++I)
      put(PT + I * PW, I * 4096 | 7, PW);
    const uint64_t Data =
        Stack32 ? 0x00cf93000000ffffULL : 0x008f93000000ffffULL;
    put(GDT + 8, 0x00cf9b000000ffffULL);
    put(GDT + 16, Data);
    put(GDT + 24, (Code32 ? 0x00cf9b000000ffffULL : 0x008f9b000000ffffULL) |
                      (uint64_t(CPL) << 45));
    put(GDT + 32, Data | (uint64_t(CPL) << 45));
    put(GDT + 40, 0x00409b0000000fffULL); // Short #DB target code segment.
    put(GDT + 48, Target | (uint64_t{8} << 16) | (uint64_t{2} << 32) |
                      (uint64_t(Gate32 ? 0xec : 0xe4) << 40));
    put(TSS + 4, 0xe000, 4);
    put(TSS + 8, 16, 2);
    for (unsigned V = 0; V != 256; ++V) {
      const uint32_t Entry = 0x4000 + V * 16;
      put(IDT + V * 8,
          Entry | (uint64_t{8} << 16) | (uint64_t(Gate32 ? 0xee : 0xe6) << 40));
      RAM[Entry] = 0xf4;
    }
    if (Which == MissingDebugGate)
      put(IDT + 8, 0x4010 | (uint64_t{8} << 16) |
                       (uint64_t(Gate32 ? 0x6e : 0x66) << 40));
    if (Which == DebugTargetLimit)
      put(IDT + 8, 0x4010 | (uint64_t{40} << 16) |
                       (uint64_t(Gate32 ? 0xee : 0xe6) << 40));
    if (Which == DebugStackFault) {
      ASSERT_EQ(CPL, 3u);
      put(IDT + 8, 0x4010 | (uint64_t{27} << 16) |
                       (uint64_t(Gate32 ? 0xee : 0xe6) << 40));
      put(PT + 12 * PW, 0xc006, PW);
    }
    if (Which == OperandFault || Which == RepairAndRetry)
      put(PT + 8 * PW, 0x8006, PW);
    if (Execute && Execute->FetchFault)
      put(PT + ((Code + Execute->CSBase) / 4096) * PW, 0, PW);
    if (RegistersAccess && RegistersAccess->FetchFault)
      put(PT + (Code / 4096) * PW, 0, PW);
    put(0x8000, 0x89abcdef, 4);
    const unsigned OperandBytes = Which == IretResume ? 4 : Code32 ? 4 : 2;
    put(0x9000, 0xc000, OperandBytes);
    put(0x9000 + OperandBytes, 32 | CPL, 2);

    kvm_sregs Special = Initial;
    Special.cr0 = 0x80010011;
    Special.cr3 = 0x10000;
    Special.cr4 = Legacy ? 0 : 0x20;
    if (Execute && Execute->VirtualInterrupt)
      Special.cr4 |= 2;
    if (RegistersAccess && RegistersAccess->DE)
      Special.cr4 |= 8;
    Special.efer = Legacy ? 0 : 0x800;
    Special.cr2 = 0x76543210;
    Special.cs = segment(24 | CPL, true, false, CPL);
    Special.cs.db = Code32;
    if (Execute)
      Special.cs.base = Execute->CSBase;
    Special.ss = Special.ds = Special.es = Special.fs = Special.gs =
        segment(32 | CPL, false, false, CPL);
    Special.ss.db = Stack32;
    Special.gdt = {GDT, 0xff, {}};
    Special.idt = {IDT, 0x7ff, {}};
    Special.ldt = {};
    Special.ldt.unusable = 1;
    Special.tr = {};
    Special.tr.base = TSS;
    Special.tr.limit = 0x67;
    Special.tr.selector = 56;
    Special.tr.type = 11;
    Special.tr.present = 1;
    kvm_regs Registers{};
    Registers.rip = Code;
    Registers.rsp =
        (Stack32 ? 0 : 0xabcd0000) |
        (Which == DebugStackFault ? 0xd000 + (Gate32 ? 4 : 2) : 0xd000);
    Registers.rflags = (RF ? 0x10000 : 0) | 0x302;
    if (Execute)
      Registers.rflags = Execute->Flags;
    if (Execute && Execute->VirtualInterrupt)
      Registers.rflags |= 0x180000;
    Registers.rax = 0x12340000 | (32 | CPL);
    Registers.rbx = 0x33445566;
    Registers.rcx = Which == ZeroRepeat ? 0 : 3;
    Registers.rdx = Target;
    Registers.rbp = 0x55667788;
    Registers.rsi = 0x8000;
    Registers.rdi = Which == LoadSS ? 0x9000
                    : (Which == OperandFault || Which == RepairAndRetry)
                        ? 0x8000
                        : 0x8100;
    if (Which == PopFlagsSet || Which == IretSet)
      Registers.rflags &= ~0x100u;
    if (Which == Overflow)
      Registers.rflags |= 0x800;
    if (Which == SetInterrupts)
      Registers.rflags &= ~0x200u;
    if (Which == InvalidSS)
      Registers.rax = 0;
    if (Which == Sysexit)
      Registers.rcx = 0xc000;

    if (RegistersAccess) {
      std::array<decltype(Registers.rax) *, 8> General{
          &Registers.rax, &Registers.rcx, &Registers.rdx, &Registers.rbx,
          &Registers.rsp, &Registers.rbp, &Registers.rsi, &Registers.rdi};
      *General[RegistersAccess->General] = RegistersAccess->Value;
      Registers.rflags = RegistersAccess->Flags;
      if (RegistersAccess->SS) {
        Registers.rbx = 32 | CPL;
        if (!CPL)
          Registers.rflags &= ~uint64_t{0x100};
      }
    }
    if (Watch)
      Registers.rflags = Watch->Flags;
    std::vector<std::vector<uint8_t>> Instructions{{0x90}};
    const auto Immediate = [](uint8_t Opcode, uint32_t Value, unsigned Bytes) {
      std::vector<uint8_t> Result{Opcode};
      for (unsigned I = 0; I != Bytes; ++I)
        Result.push_back(Value >> (I * 8));
      return Result;
    };
    switch (Which) {
    case Increment:
      Instructions = {{0x40}};
      break;
    case ReturnAndStepAgain:
      Instructions = {{0x40}, {0x40}, {0xcd, 64}};
      break;
    case RepeatAndReturn:
      Instructions = {{0xf3, 0xa4}, {0xcd, 64}};
      break;
    case MovSS:
      Instructions = {{0x8e, 0xd0}, {0x90}};
      break;
    case ConsecutiveSS:
      Instructions = {{0x8e, 0xd0}, {0x8e, 0xd0}};
      break;
    case StackIcebp:
      Instructions = {{0x8e, 0xd0}, {0xf1}};
      break;
    case PopSS:
      Instructions = {{0x17}, {0x90}};
      put(0xd000, 32 | CPL, OperandBytes);
      break;
    case LoadSS:
      Instructions = {{0x0f, 0xb2, 0x07}};
      if (!Code32)
        Instructions[0].insert(Instructions[0].begin(), 0x67);
      break;
    case PopFlagsSet:
    case PopFlagsClear:
      Instructions = {{0x9d}};
      put(0xd000, Which == PopFlagsSet ? 0x302 : 0x202, OperandBytes);
      if (Which == PopFlagsSet)
        Instructions.push_back({0x90});
      break;
    case IretClear:
    case IretSet:
    case IretResume:
    case FarReturn:
      Instructions = {{uint8_t(Which == FarReturn ? 0xcb : 0xcf)}};
      if (Which == IretResume && !Code32)
        Instructions[0].insert(Instructions[0].begin(), 0x66);
      put(0xd000, Target, OperandBytes);
      put(0xd000 + OperandBytes, 24 | CPL, OperandBytes);
      put(0xd000 + 2 * OperandBytes,
          Which == IretSet      ? 0x302
          : Which == IretResume ? 0x10202
                                : 0x202,
          OperandBytes);
      if (Which == IretSet)
        Instructions.push_back({0x90});
      break;
    case NearCall:
      Instructions = {
          Immediate(0xe8, Target - Code - 1 - OperandBytes, OperandBytes)};
      break;
    case NearReturn:
      Instructions = {{0xc3}};
      put(0xd000, Target, OperandBytes);
      break;
    case FarCall:
    case FarJump:
      Instructions = {
          Immediate(Which == FarCall ? 0x9a : 0xea, Target, OperandBytes)};
      Instructions[0].insert(Instructions[0].end(), {uint8_t(24 | CPL), 0});
      break;
    case CallGate:
    case JumpGate:
      Instructions = {
          Immediate(Which == CallGate ? 0x9a : 0xea, 0, OperandBytes)};
      Instructions[0].insert(Instructions[0].end(), {uint8_t(48 | CPL), 0});
      put(0xd000, 0x123456789abcdef0ULL);
      break;
    case TakenBranch:
      Instructions = {{0x75, 0x20}};
      break;
    case UntakenBranch:
      Instructions = {{0x74, 0x20}};
      break;
    case SetInterrupts:
      Instructions = {{0xfb}};
      break;
    case ClearInterrupts:
      Instructions = {{0xfa}};
      break;
    case Icebp:
      Instructions = {{0xf1}};
      break;
    case SoftwareInterrupt:
      Instructions = {{0xcd, 64}};
      break;
    case Breakpoint:
      Instructions = {{0xcc}};
      break;
    case SoftwareDebug:
      Instructions = {{0xcd, 1}};
      break;
    case Overflow:
    case NoOverflow:
      Instructions = {{0xce}};
      break;
    case Repeat:
    case ZeroRepeat:
      Instructions = {{0xf3, 0xa4}};
      break;
    case InvalidOpcode:
      Instructions = {{0x0f, 0x0b}};
      break;
    case OperandFault:
    case RepairAndRetry:
      Instructions = {{0x8b, 0x07}};
      if (!Code32)
        Instructions[0].insert(Instructions[0].begin(), 0x67);
      if (Which == RepairAndRetry)
        Instructions.push_back({0xcd, 64});
      break;
    case InvalidSS:
      Instructions = {{0x8e, 0xd0}};
      break;
    case Sysenter:
      Instructions = {{0x0f, 0x34}};
      break;
    case Sysexit:
      Instructions = {{0x0f, 0x35}};
      break;
    default:
      break;
    }
    if (RegistersAccess) {
      Instructions = {RegistersAccess->Bytes};
      if (RegistersAccess->SS) {
        // Execute MOV SS itself: an injected shadow plus TF lacks VMX's
        // pending-debug state. At CPL0, first release KVM's lazy MOV-DR
        // interception with GD clear, then set GD in the guest. POPFD sets
        // TF immediately before MOV SS, whose own TF trap is suppressed.
        if (!CPL) {
          auto SetCX = Immediate(0xb9, RegistersAccess->DR7, 4);
          auto PushFlags = Immediate(0x68, RegistersAccess->Flags, 4);
          std::vector<uint8_t> PopFlags{0x9d};
          if (!Code32) {
            SetCX.insert(SetCX.begin(), 0x66);
            PushFlags.insert(PushFlags.begin(), 0x66);
            PopFlags.insert(PopFlags.begin(), 0x66);
          }
          Instructions = {{0x0f, 0x21, 0xf9},
                          SetCX,
                          {0x0f, 0x23, 0xf9},
                          PushFlags,
                          PopFlags,
                          {0x8e, 0xd3},
                          RegistersAccess->Bytes};
        } else {
          Instructions = {{0x8e, 0xd3}, RegistersAccess->Bytes};
        }
      }
    }
    if (Watch) {
      if (!Watch->Instructions.empty()) {
        Instructions = Watch->Instructions;
        if (!Code32)
          for (auto &Bytes : Instructions)
            if (Bytes.size() >= 2 && Bytes[1] == 0x07)
              Bytes.insert(Bytes.begin(), 0x67);
      }
      put(0x8100, 32 | CPL, 4);
    }
    if (Execute || RegistersAccess || Watch)
      Instructions.push_back({0xcd, 64});
    uint32_t End = Code + Special.cs.base;
    for (const auto &Bytes : Instructions) {
      std::copy(Bytes.begin(), Bytes.end(), RAM + End);
      End += Bytes.size();
    }
    RAM[End] = 0xf4;
    RAM[Target] = 0x90;
    RAM[Target + 1] = 0xf4;

    auto Steps = Instructions;
    if (RoundTrip) {
      const std::vector<uint8_t> Return =
          Gate32 ? std::vector<uint8_t>{0xcf}
                 : std::vector<uint8_t>{0x66, 0xcf};
      const std::vector<uint8_t> Count{0xfe, 0x05, 0x00, 0x95, 0x00, 0x00};
      std::copy(Count.begin(), Count.end(), RAM + 0x4010);
      std::copy(Return.begin(), Return.end(), RAM + 0x4010 + Count.size());
      Steps.clear();
      if (Execute && Execute->Resume) {
        // The 32-bit #DB frame keeps RF clear. The handler sets it explicitly
        // before IRETD so the original instruction executes once on retry.
        ASSERT_TRUE(Gate32);
        std::vector<uint8_t> SetRF{0x81, 0x0d};
        for (uint32_t Value : {CPL ? 0xdff4u : 0xcffcu, 0x10000u})
          for (unsigned I = 0; I != 4; ++I)
            SetRF.push_back(Value >> (I * 8));
        std::copy(SetRF.begin(), SetRF.end(), RAM + 0x4010);
        std::copy(Count.begin(), Count.end(), RAM + 0x4010 + SetRF.size());
        std::copy(Return.begin(), Return.end(),
                  RAM + 0x4010 + SetRF.size() + Count.size());
        Steps = {Instructions[0], SetRF, Count, Return, Instructions[0]};
      } else if (RegistersAccess && RegistersAccess->Resume) {
        Steps = {Instructions[0], Count, Return, Instructions[0]};
      } else if (Which == RepairAndRetry) {
        std::vector<uint8_t> Repair{0xc7, 0x05};
        for (uint32_t Value : {PT + 8 * PW, 0x8007u})
          for (unsigned I = 0; I != 4; ++I)
            Repair.push_back(Value >> (I * 8));
        const std::vector<uint8_t> DropError{0x83, 0xc4,
                                             uint8_t(Gate32 ? 4 : 2)};
        uint32_t PC = 0x40e0;
        for (const auto &Bytes : {Repair, DropError, Return}) {
          std::copy(Bytes.begin(), Bytes.end(), RAM + PC);
          PC += Bytes.size();
        }
        Steps = {Instructions[0], Repair, DropError, Return};
      }
      const unsigned Traps = Which == ReturnAndStepAgain ? 2
                             : Which == RepeatAndReturn  ? 3
                                                         : 1;
      if ((!Execute || !Execute->Resume) &&
          (!RegistersAccess || !RegistersAccess->Resume))
        for (unsigned I = 0; I != Traps; ++I)
          Steps.insert(Steps.end(), {Instructions[0], Count, Return});
      Steps.push_back({0xcd, 64});
    }

    const auto Cache = [](const kvm_segment &C) {
      return neverd::X86LegacySegmentCache{
          uint32_t(C.base), C.limit, C.selector,
          uint16_t(C.type | C.s << 4 | C.dpl << 5 | C.present << 7 |
                   C.avl << 12 | C.l << 13 | C.db << 14 | C.g << 15),
          bool(C.unusable)};
    };
    std::array<kvm_segment *, 6> Segments{&Special.es, &Special.cs,
                                          &Special.ss, &Special.ds,
                                          &Special.fs, &Special.gs};
    X86LegacyExecutionState State;
    for (unsigned I = 0; I != Segments.size(); ++I)
      State.Core.Segments[I] = Cache(*Segments[I]);
    State.Core.GPR = {uint32_t(Registers.rax), uint32_t(Registers.rcx),
                      uint32_t(Registers.rdx), uint32_t(Registers.rbx),
                      uint32_t(Registers.rsp), uint32_t(Registers.rbp),
                      uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
    State.Core.CPL = CPL;
    State.Core.GDTRBase = GDT;
    State.Core.GDTRLimit = 0xff;
    State.Core.LDTR.Unusable = true;
    State.EIP = Code;
    State.EFlags = Registers.rflags;
    State.CR2 = Special.cr2;
    State.IDTRBase = IDT;
    State.IDTRLimit = 0x7ff;
    State.TR = Cache(Special.tr);
    State.Sysenter = {8, 0xb000, Target};
    State.Debug = X86LegacyDebugState{DR6, 0x400, 0};
    if (Execute) {
      State.Debug->DR7 = Execute->DR7;
      State.Debug->DR.fill(Code + Execute->CSBase + Execute->Offset);
    }
    if (RegistersAccess) {
      State.Debug->DR7 =
          RegistersAccess->SS && !CPL ? 0x400 : RegistersAccess->DR7;
      State.Debug->DR = {0x9000, 0x9100, 0x9200, 0x9300};
    }
    if (Watch) {
      State.Debug->DR7 = Watch->DR7;
      State.Debug->DR = Watch->DR;
    }
    const auto InitialDebug = *State.Debug;
    State.NMIBlocked =
        Which == IretClear || Which == IretSet || Which == IretResume;
    const bool InitialNMI = State.NMIBlocked;
    auto Logical =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    Logical.Memory.push_back({0,
                              MemoryPermission::Read | MemoryPermission::Write,
                              0, std::vector<uint8_t>(RAM, RAM + MemorySize)});
    auto Memory =
        llvm::cantFail(X86PagedMemory::create(Logical, Features,
                                              {.CR0 = Special.cr0,
                                               .CR3 = Special.cr3,
                                               .CR4 = Special.cr4,
                                               .EFER = Special.efer,
                                               .RFlags = Registers.rflags,
                                               .PDPTE = {0x11001, 0, 0, 0}}));
    X86LegacyExecutionOptions Options{
        {X86LegacySegmentProfile::WordStackOffsetFirst,
         X86AlignmentPriority::BeforePaging,
         neverd::X86FullSegmentLimitChoice::Fault},
        X86LegacyEventProfile::OrderedFrame,
        false,
        X86LegacyReturnProfile::OrderedFrame};
    Options.Scalars = X86LegacyScalarProfile::OrderedRAM;
    Options.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
    Options.Calls = X86LegacyCallProfile::StoreBeforeTargetLimit;
    Options.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
    Options.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
    Options.Debug = Execute ? X86LegacyDebugProfile::ExecuteBreakpoints
                            : X86LegacyDebugProfile::InstructionStep;
    if (RegistersAccess) {
      Options.Debug = X86LegacyDebugProfile::DebugRegisters;
      Options.GeneralDetectFrame = neverd::X86GeneralDetectFrame::ClearResume;
    }
    if (Watch)
      Options.Debug = X86LegacyDebugProfile::Watchpoints;
    if (Execute && Execute->Resume)
      Options.LogicalAuxiliaryCarry = false;
    X86LegacyExecutionResult Result;
    for (const auto &Bytes : Steps) {
      auto Got = executeX86LegacyInstruction(State, *Memory, Bytes, Options);
      ASSERT_TRUE(bool(Got)) << llvm::toString(Got.takeError());
      Result = std::move(*Got);
      if (!RoundTrip &&
          Result.Status == X86LegacyExecutionStatus::EventDelivered)
        break;
      ASSERT_TRUE(Result.Status == X86LegacyExecutionStatus::Completed ||
                  (RoundTrip &&
                   Result.Status == X86LegacyExecutionStatus::EventDelivered));
    }
    ASSERT_EQ(Result.Status, X86LegacyExecutionStatus::EventDelivered);
    ASSERT_EQ(State.Core.CPL, 0u);
    // Native HLT fetch occurs after the observed event boundary. Account for
    // exactly that fetch in RAM/page effects; it changes no debug cause/frame.
    uint8_t Halt = 0;
    auto Fetch = llvm::cantFail(Memory->loadSegment(
        {neverd::X86SegmentRegister::CS, State.Core.Segments[1], State.EIP,
         MemoryAccessKind::Execute, State.Core.CPL,
         neverd::X86FullSegmentLimitChoice::Fault},
        llvm::MutableArrayRef(&Halt, 1)));
    ASSERT_EQ(Fetch.Status, X86PageWalkStatus::Completed);
    ASSERT_EQ(Halt, 0xf4);

    auto Flush = Special;
    Flush.cr3 = 0x14000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0) << errno;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Special), 0) << errno;
    if (Watch)
      ASSERT_NO_FATAL_FAILURE(
          neverd::test::prefaultX86KVMBacking(CPU, MemorySize));
    struct {
      uint32_t Count = 4;
      uint32_t Reserved = 0;
      kvm_msr_entry Entries[4];
    } MSRs{};
    MSRs.Entries[0] = {.index = 0x1d9, .data = 0};
    MSRs.Entries[1] = {.index = 0x174, .data = 8};
    MSRs.Entries[2] = {.index = 0x175, .data = 0xb000};
    MSRs.Entries[3] = {.index = 0x176, .data = Target};
    ASSERT_EQ(ioctl(CPU, KVM_SET_MSRS, &MSRs), 4);
    kvm_guest_debug Debugger{};
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debugger), 0);
    kvm_debugregs Debug{};
    // Disabled comparators deliberately match the original instruction and
    // following bytes. They must not invent active hardware-breakpoint causes.
    for (unsigned I = 0; I != 4; ++I)
      Debug.db[I] = Code + I;
    if (Execute || RegistersAccess || Watch)
      std::copy(InitialDebug.DR.begin(), InitialDebug.DR.end(), Debug.db);
    Debug.dr6 = InitialDebug.DR6;
    Debug.dr7 = InitialDebug.DR7;
    ASSERT_EQ(ioctl(CPU, KVM_SET_DEBUGREGS, &Debug), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    Events.nmi.masked = InitialNMI;
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Run->exit_reason, unsigned(KVM_EXIT_HLT));
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &Registers), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Special), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_DEBUGREGS, &Debug), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &Events), 0);
    EXPECT_EQ(Registers.rip - 1, State.EIP);
    EXPECT_EQ(Registers.rflags, State.EFlags);
    EXPECT_EQ(Special.cr2, State.CR2);
    EXPECT_EQ(Debug.dr6, State.Debug->DR6);
    EXPECT_EQ(Debug.dr7, State.Debug->DR7);
    if (RegistersAccess || Watch)
      for (unsigned Slot = 0; Slot != 4; ++Slot)
        EXPECT_EQ(Debug.db[Slot], State.Debug->DR[Slot]);
    EXPECT_EQ(bool(Events.nmi.masked), State.NMIBlocked);
    EXPECT_EQ(bool(Events.interrupt.shadow & KVM_X86_SHADOW_INT_MOV_SS),
              State.SSBlocked);
    EXPECT_EQ(bool(Events.interrupt.shadow & KVM_X86_SHADOW_INT_STI),
              State.STIBlocked);
    const std::array<uint32_t, 8> GPR{
        uint32_t(Registers.rax), uint32_t(Registers.rcx),
        uint32_t(Registers.rdx), uint32_t(Registers.rbx),
        uint32_t(Registers.rsp), uint32_t(Registers.rbp),
        uint32_t(Registers.rsi), uint32_t(Registers.rdi)};
    EXPECT_EQ(GPR, State.Core.GPR);
    for (unsigned I = 0; I != Segments.size(); ++I) {
      const auto Native = Cache(*Segments[I]);
      const auto &Model = State.Core.Segments[I];
      EXPECT_EQ(Native.Selector, Model.Selector);
      EXPECT_EQ(Native.Base, Model.Base);
      EXPECT_EQ(Native.Limit, Model.Limit);
      EXPECT_EQ(Native.Attributes, Model.Attributes);
      EXPECT_EQ(Native.Unusable, Model.Unusable);
    }
    const auto Snapshot = Memory->physicalMemory().snapshotMemoryRegions();
    ASSERT_EQ(Snapshot.size(), 1u);
    const auto &ModelRAM = Snapshot[0].Bytes;
    ASSERT_EQ(ModelRAM.size(), MemorySize);
    const auto Difference =
        std::mismatch(ModelRAM.begin(), ModelRAM.end(), RAM);
    EXPECT_EQ(Difference.first, ModelRAM.end())
        << "first RAM difference at 0x" << std::hex
        << std::distance(ModelRAM.begin(), Difference.first);
    if (RoundTrip) {
      EXPECT_EQ(RAM[0x9500], Which == ReturnAndStepAgain ? 2
                             : Which == RepeatAndReturn  ? 3
                                                         : 1);
      ++RoundTrips;
    }
    ++Comparisons;
  }
};

TEST_P(X86LegacyDebugNativeTest, OriginalInstructionStepsAndDebugCauses) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (unsigned CPL : {0u, 3u})
          for (unsigned I = 0; I != CaseCount; ++I) {
            if (I == DebugStackFault && CPL != 3)
              continue;
            // Use canonical native DR6: raw KVM injection accepts BLD/RTM
            // bits that architectural register reads force to 1 on this CPU.
            for (uint32_t Status :
                 {0xffff0ff0u, 0xffffafffu, 0xffffefffu, 0xffffeff0u})
              for (bool RF : {false, true}) {
                ASSERT_NO_FATAL_FAILURE(compare(Code32, Stack32, Gate32, CPL,
                                                static_cast<Case>(I), Status,
                                                RF));
                if (HasFailure())
                  return;
              }
          }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("round_trip_comparisons", RoundTrips);
}

INSTANTIATE_TEST_SUITE_P(LegacyDebug, X86LegacyDebugNativeTest,
                         testing::Values(2u, 3u));
