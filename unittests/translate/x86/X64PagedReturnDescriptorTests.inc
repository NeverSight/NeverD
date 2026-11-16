//===- X64PagedReturnDescriptorTests.inc - Return descriptor ranges -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent code64 IRET and RETF descriptor boundary observations.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X64PagedReturnDescriptorTest : public X64PagedDescriptorBoundaryFixture {
protected:
  static unsigned width(unsigned Form) { return 2u << (Form % 3); }

  static unsigned discard(unsigned Form) {
    return Form < 6 ? 0 : Form < 9 ? 16 : 65535;
  }

  static X64PagedExecutionOptions options(bool TailGPZero) {
    X64PagedExecutionOptions Options;
    Options.IretAccessOrder =
        TailGPZero ? X64PagedIretAccessOrder::FlagsFirstTailGPZero
                   : X64PagedIretAccessOrder::FlagsFirst;
    Options.FarReturnProfile =
        TailGPZero ? X64PagedFarReturnProfile::SelectorFirstTailGPZero
                   : X64PagedFarReturnProfile::SelectorFirstTruncatedIP;
    return Options;
  }

  static std::vector<uint8_t> instruction(unsigned Form) {
    std::vector<uint8_t> Code;
    if (width(Form) != 4)
      Code.push_back(width(Form) == 2 ? 0x66 : 0x48);
    Code.push_back(Form < 3 ? 0xcf : discard(Form) ? 0xca : 0xcb);
    if (discard(Form)) {
      Code.push_back(discard(Form));
      Code.push_back(discard(Form) >> 8);
    }
    return Code;
  }

  void prepare(unsigned Form, bool Stack, unsigned Case, bool Split = false) {
    descriptorBoundary(instruction(Form), Case,
                       Stack ? 0x00cff3000000ffffULL : 0x00af9b000000ffffULL);
    auto &Core = State.Core.Core.Core.Core;
    Core.GPR[0] = 0x1122334455667788ULL;
    Core.GPR[4] = Split ? 0x1e000 - (Form < 3 ? 4 : 3) * width(Form) : 0x1d000;
    State.NMIBlocked = 1;
    // Keep data caches usable after the outer return. KVM clears the present
    // bit in invalidated cache metadata, which is outside this boundary probe.
    State.DS = State.ES = {0, UINT32_MAX, 0x23, 0xc0f3};
    State.FS = State.GS = {UINT32_MAX, 0x23, 0xc0f3};
    RAM.Memory.front().Bytes[0x1800] = 0x90;
    RAM.Memory.front().Bytes[0x1801] = 0x90;
    const uint64_t CS = Stack ? 0x1b : 0x24, SS = Stack ? 0x27 : 0x10;
    std::vector<uint64_t> Values{0x1800, CS};
    if (Form < 3)
      Values.push_back(0x246);
    Values.push_back(0xe800);
    Values.push_back(SS);
    for (unsigned I = 0; I != Values.size(); ++I)
      for (unsigned B = 0; B != width(Form); ++B)
        RAM.Memory.front().Bytes[Core.GPR[4] + I * width(Form) + B +
                                 (Form >= 3 && I >= 2 ? discard(Form) : 0)] =
            Values[I] >> (8 * B);
  }
};

TEST_F(X64PagedReturnDescriptorTest, ObserveOriginalRanges) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Observations = 0;
  for (unsigned Form = 0; Form != 12; ++Form)
    for (bool Stack : {false, true})
      for (unsigned Case = 0; Case != 13; ++Case) {
        SCOPED_TRACE(
            llvm::formatv("form={0} stack={1} case={2}", Form, Stack, Case)
                .str());
        X64PagedNativeCPU CPU;
        ASSERT_NO_FATAL_FAILURE(CPU.initialize());
        if (IsSkipped())
          return;
        if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
          GTEST_SKIP()
              << "Return descriptor oracle requires KVM pre-fault memory";
        ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
        if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
          GTEST_SKIP() << "No return profile bound for this Intel core type";
        CPU.PrefaultMemory = true;
        Features.PhysicalAddressBits = CPU.PhysicalBits;
        prepare(Form, Stack, Case);
        X64ContinuationStateV6 Native;
        bool Fault = false;
        ASSERT_NO_FATAL_FAILURE(
            CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                        instruction(Form).size(), Native, Fault, false, nullptr,
                        Controls.CR4, true, false));
        const auto &R = Native.Core.Core.Core.Core;
        uint64_t Error = 0;
        if (Fault) {
          ASSERT_LT(R.GPR[4] + 8, X64PagedNativeCPU::Size);
          for (unsigned I = 0; I != 8; ++I)
            Error |= uint64_t(CPU.RAM[R.GPR[4] + I]) << (I * 8);
        }
        RecordProperty(
            "return_descriptor_" + std::to_string(Form) + "_" +
                std::to_string(Stack) + "_" + std::to_string(Case),
            llvm::formatv(
                "core={0:x};fault={1};pc={2:x};error={3:x};cr2={4:x};sp={5:x};"
                "cs={6:x};ss={7:x};nmi={8};pml4={9:x};low_pte={10:x}",
                CPU.ReturnCoreType, Fault, R.PC, Error, Native.Core.Core.CR2,
                R.GPR[4], Native.Core.CS.Selector, Native.Core.SS.Selector,
                unsigned(Native.NMIBlocked),
                unsigned(
                    CPU.RAM[0x10000 +
                            ((descriptorAddress(Case) >> 39) & 0x1ff) * 8]),
                unsigned(CPU.RAM[0x13000]))
                .str());
        EXPECT_EQ(Fault, faults(Case));
        EXPECT_EQ(R.PC,
                  Fault ? 0x4000 + (pageFault(Case) ? 14 : 13) * 16 : 0x1800);
        EXPECT_EQ(Error, error(Case, CPU.ReturnCoreType == 0x40));
        EXPECT_EQ(Native.Core.Core.CR2, cr2(Case));
        EXPECT_EQ(Native.NMIBlocked, Form < 3 ? 0u : 1u);
        ++Observations;
      }
  RecordProperty("original_code64_return_descriptor_observations",
                 Observations);
}
TEST_F(X64PagedReturnDescriptorTest, GeneratedRangesMatchOriginalInstructions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (unsigned Form = 0; Form != 12; ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          const auto Code = instruction(Form);
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(
              compile(Code, LLVM, O, Driver, {0x1000}, Compiler));
          for (bool Stack : {false, true})
            for (unsigned Case = 0; Case != 13; ++Case) {
              SCOPED_TRACE(
                  llvm::formatv("form={0} stack={1} case={2} llvm={3} {4} {5}",
                                Form, Stack, Case, LLVM, Compiler, O)
                      .str());
              X64PagedNativeCPU CPU;
              ASSERT_NO_FATAL_FAILURE(CPU.initialize());
              if (IsSkipped())
                return;
              if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
                GTEST_SKIP()
                    << "Return descriptor oracle requires KVM pre-fault memory";
              ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
              if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
                GTEST_SKIP()
                    << "No return profile bound for this Intel core type";
              CPU.PrefaultMemory = true;
              Features.PhysicalAddressBits = CPU.PhysicalBits;
              prepare(Form, Stack, Case);
              X64ContinuationStateV6 Native;
              bool Fault = false;
              ASSERT_NO_FATAL_FAILURE(CPU.execute(
                  RAM.Memory.front().Bytes, State, Controls.CR0, Code.size(),
                  Native, Fault, false, nullptr, Controls.CR4, true, false));
              ASSERT_EQ(Fault, faults(Case));
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecution Context(*Memory,
                                        options(CPU.ReturnCoreType == 0x40));
              uint64_t Budget = 1;
              EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context),
                               &Budget),
                        Fault ? 7u : 6u)
                  << Context.lastError();
              EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
              EXPECT_EQ(Budget, 0u);
              EXPECT_TRUE(Context.lastResult().InstructionAttempted);
              EXPECT_EQ(Context.lastResult().InstructionCompleted, !Fault);
              if (Fault) {
                ASSERT_TRUE(Context.lastResult().Exception);
                EXPECT_EQ(Context.lastResult().Exception->Vector,
                          pageFault(Case) ? 14 : 13);
                EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                          error(Case, CPU.ReturnCoreType == 0x40));
                EXPECT_EQ(Context.lastResult().Exception->CR2,
                          pageFault(Case) ? std::optional<uint64_t>(cr2(Case))
                                          : std::nullopt);
              }
              EXPECT_EQ(State, Native);
              compareRAM(*Memory, {CPU.RAM, X64PagedNativeCPU::Size});
              ++Comparisons;
            }
        }
  RecordProperty("original_code64_return_descriptor_comparisons", Comparisons);
}

TEST_F(X64PagedReturnDescriptorTest, ResumeFreezesDescriptorProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Form = 0; Form != 12; ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const auto Code = instruction(Form);
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            compile(Code, LLVM, "-O2", Driver, {0x1000}, Compiler));
        for (bool Stack : {false, true})
          for (unsigned Pause = 0; Pause != 3; ++Pause) {
            const bool Split = Pause != 0;
            if (Split && (!Stack || discard(Form)))
              continue;
            for (bool TailGPZero : {false, true}) {
              SCOPED_TRACE(
                  llvm::formatv(
                      "form={0} stack={1} pause={2} zero={3} llvm={4} {5}",
                      Form, Stack, Pause, TailGPZero, LLVM, Compiler)
                      .str());
              prepare(Form, Stack, 2, Split);
              if (Pause == 2)
                put(0x5018, 0x00affa000000ffffULL);
              const auto Before = State;
              const auto Options = options(TailGPZero);
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecution Direct(*Memory, Options);
              uint64_t Budget = 1;
              ASSERT_EQ(
                  Driver(&State, reinterpret_cast<uint8_t *>(&Direct), &Budget),
                  7u)
                  << Direct.lastError();
              ASSERT_TRUE(Direct.lastResult().Exception);
              EXPECT_EQ(Direct.lastResult().Exception->ErrorCode,
                        TailGPZero ? 0u : 0x24u);
              const auto Expected = State;
              const auto ExpectedRAM = bytes(*Memory);
              State = Before;
              const unsigned Page = Split ? 0x1e000 : 0x1d000;
              Memory = without(Page);
              X64PagedExecution First(*Memory, Options);
              Budget = 1;
              ASSERT_EQ(
                  Driver(&State, reinterpret_cast<uint8_t *>(&First), &Budget),
                  18u)
                  << First.lastError();
              const bool Consumed = Form < 3 || Pause == 2;
              EXPECT_EQ(Budget, Consumed ? 0u : 1u);
              EXPECT_EQ(First.lastResult().InstructionAttempted, Consumed);
              auto StoppedState = Before;
              StoppedState.NMIBlocked = Form < 3 ? 0 : 1;
              EXPECT_EQ(State, StoppedState);
              const auto StoppedRAM = bytes(*Memory);
              const auto Pending = First.continuation();
              ASSERT_TRUE(Pending);
              // The accepted IRET has zero budget after unblocking NMI. A
              // repeated missing-backing stop must still resume without charge.
              ASSERT_EQ(
                  Driver(&State, reinterpret_cast<uint8_t *>(&First), &Budget),
                  18u)
                  << First.lastError();
              EXPECT_FALSE(First.lastResult().InstructionAttempted);
              EXPECT_EQ(Budget, Consumed ? 0u : 1u);
              EXPECT_EQ(State, StoppedState);
              compareRAM(*Memory, StoppedRAM);
              auto Repaired = repair(*Memory, Page);
              const auto BeforeRAM = bytes(*Repaired);
              for (bool Iret : {false, true}) {
                auto Changed = Options;
                if (Iret)
                  Changed.IretAccessOrder =
                      options(!TailGPZero).IretAccessOrder;
                else
                  Changed.FarReturnProfile =
                      options(!TailGPZero).FarReturnProfile;
                X64PagedExecution Refused(*Repaired, Changed, Pending);
                EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Refused),
                                 &Budget),
                          5u);
                EXPECT_FALSE(Refused.lastError().empty());
                EXPECT_EQ(Refused.continuation(), Pending);
                EXPECT_FALSE(Refused.lastResult().InstructionAttempted);
                EXPECT_FALSE(Refused.lastResult().InstructionResumed);
                EXPECT_EQ(Budget, Consumed ? 0u : 1u);
                EXPECT_EQ(State, StoppedState);
                compareRAM(*Repaired, BeforeRAM);
              }
              X64PagedExecution Resumed(*Repaired, Options, Pending);
              ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Resumed),
                               &Budget),
                        7u)
                  << Resumed.lastError();
              EXPECT_EQ(Budget, 0u);
              EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
              EXPECT_EQ(Resumed.lastResult().InstructionAttempted, !Consumed);
              EXPECT_FALSE(Resumed.lastResult().InstructionCompleted);
              ASSERT_TRUE(Resumed.lastResult().Exception);
              EXPECT_EQ(Resumed.lastResult().Exception->Vector, 13);
              EXPECT_EQ(Resumed.lastResult().Exception->ErrorCode,
                        TailGPZero ? 0u : 0x24u);
              EXPECT_FALSE(Resumed.continuation());
              EXPECT_EQ(State, Expected);
              compareRAM(*Repaired, ExpectedRAM);
              ++Comparisons;
            }
          }
      }
  RecordProperty("return_descriptor_resume_comparisons", Comparisons);
}

TEST_F(X64PagedReturnDescriptorTest, IretFaultEventResumeRetainsAcceptedError) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true}) {
      const auto Code = instruction(2);
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(
          compile(Code, LLVM, "-O2", Driver, {0x1000}, Compiler));
      for (bool TailGPZero : {false, true}) {
        SCOPED_TRACE(
            llvm::formatv("zero={0} llvm={1} {2}", TailGPZero, LLVM, Compiler)
                .str());
        prepare(2, true, 2);
        const auto Before = State;
        const auto Options = options(TailGPZero);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Direct(*Memory, Options);
        uint64_t Budget = 1;
        ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Direct), &Budget),
                  7u)
            << Direct.lastError();
        const auto Expected = State;
        const auto ExpectedRAM = bytes(*Memory);
        State = Before;
        auto Snapshot = RAM;
        const auto &Bytes = RAM.Memory.front().Bytes;
        Snapshot.Memory = {{0,
                            MemoryPermission::Read | MemoryPermission::Write,
                            0,
                            {Bytes.begin(), Bytes.begin() + 0x1c000}},
                           {0x1e000,
                            MemoryPermission::Read | MemoryPermission::Write,
                            0,
                            {Bytes.begin() + 0x1e000, Bytes.end()}}};
        Memory = llvm::cantFail(
            X86PagedMemory::create(Snapshot, Features, Controls));
        X64PagedExecution First(*Memory, Options);
        Budget = 1;
        ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&First), &Budget),
                  18u)
            << First.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(State.NMIBlocked, 0u);
        const auto StoppedState = State;
        const auto StoppedRAM = bytes(*Memory);
        ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&First), &Budget),
                  18u)
            << First.lastError();
        EXPECT_FALSE(First.lastResult().InstructionAttempted);
        EXPECT_EQ(State, StoppedState);
        compareRAM(*Memory, StoppedRAM);
        auto Frame = repair(*Memory, 0x1d000);
        X64PagedExecution Refused(*Frame, options(!TailGPZero),
                                  First.continuation());
        EXPECT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Refused), &Budget), 5u);
        EXPECT_FALSE(Refused.lastError().empty());
        EXPECT_EQ(State, StoppedState);
        EXPECT_EQ(Budget, 0u);
        X64PagedExecution Fault(*Frame, Options, First.continuation());
        ASSERT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Fault), &Budget),
                  18u)
            << Fault.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_FALSE(Fault.lastResult().InstructionAttempted);
        ASSERT_TRUE(Fault.lastResult().Exception);
        EXPECT_EQ(Fault.lastResult().Exception->Vector, 13);
        EXPECT_EQ(Fault.lastResult().Exception->ErrorCode,
                  TailGPZero ? 0u : 0x24u);
        EXPECT_FALSE(Fault.continuation());
        ASSERT_TRUE(Fault.lastResult().Delivery);
        ASSERT_TRUE(Fault.lastResult().Delivery->Continuation);
        auto Handler = repair(*Frame, 0x1c000);
        X64PagedExecution Resumed(*Handler, Options);
        auto Event = Resumed.resume(
            State, *Fault.lastResult().Delivery->Continuation, 0xffff);
        ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
        EXPECT_EQ(Event->Exit, 7u);
        EXPECT_EQ(Budget, 0u);
        EXPECT_FALSE(Resumed.lastResult().InstructionAttempted);
        EXPECT_FALSE(Resumed.continuation());
        EXPECT_EQ(State, Expected);
        compareRAM(*Handler, ExpectedRAM);
        ++Comparisons;
      }
    }
  RecordProperty("return_descriptor_event_resume_comparisons", Comparisons);
}

TEST_F(X64PagedReturnDescriptorTest, UnknownProfilesRefuseBeforeEffects) {
  for (bool Interrupt : {false, true}) {
    prepare(Interrupt ? 2 : 5, false, 0);
    const auto Before = State;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    const auto BeforeRAM = bytes(*Memory);
    const auto BeforeControls = Memory->controls();
    auto Result =
        Interrupt
            ? returnX64PagedInterrupt(
                  State, *Memory, 8, X64PagedAlignmentPriority::AfterPaging,
                  static_cast<X64PagedIretAccessOrder>(0xff))
            : returnX64PagedFar(State, *Memory, 8, 0,
                                X64PagedAlignmentPriority::AfterPaging,
                                static_cast<X64PagedFarReturnProfile>(0xff));
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(State, Before);
    EXPECT_EQ(Memory->controls(), BeforeControls);
    compareRAM(*Memory, BeforeRAM);
  }
}

TEST_F(X64PagedReturnDescriptorTest, FullModeRetainsAcceptedProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Form : {2u, 5u, 8u, 11u})
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const auto Code = instruction(Form);
        llvm::json::Object Settings{{"profile", "kernel-events-v6"},
                                    {"mxcsr_mask", "0xffff"},
                                    {"paging_address_bits", 48},
                                    {"paged_memory", true},
                                    {"scheduled_events", true},
                                    {"instructions", llvm::json::Array{0x1000}},
                                    {"null_segment_base_policy", "clear"}};
        const auto Request =
            llvm::formatv("{0}", llvm::json::Value(std::move(Settings))).str();
        auto Export = exportX64Continuation(image(Code), Request, LLVM);
        ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
        const auto Manifest =
            llvm::cantFail(llvm::json::parse(Export->Manifest));
        const auto *Units = Manifest.getAsObject()->getArray("units");
        ASSERT_NE(Units, nullptr);
        ASSERT_EQ(Units->size(), 1u);
        const auto Symbol = Units->front().getAsObject()->getString("name");
        ASSERT_TRUE(Symbol);
        const auto Adapters =
            "\nuint64_t mode_body(void *s, void *e) { return " + Symbol->str() +
            "_paged_adapter(s,e); }\n" +
            "uint64_t mode_validate(void *s, void *e) { return " +
            Symbol->str() + "_validate_paged_adapter(s,e); }\n";
        llvm::sys::DynamicLibrary DL;
        const auto Name = std::string("return-descriptor-mode-") +
                          (LLVM ? "llvm" : "high") + "-" +
                          std::to_string(Compilation++);
        ASSERT_NO_FATAL_FAILURE(
            compileProgram(*Export, Name, Compiler, "-O2", DL, Adapters));
        X86ModeInstruction Instruction{0x1000, X86ExecutionMode::Long64, Code};
        Instruction.Body =
            reinterpret_cast<X64PagedBody>(DL.getAddressOfSymbol("mode_body"));
        Instruction.Validate = reinterpret_cast<X64PagedBody>(
            DL.getAddressOfSymbol("mode_validate"));
        const auto Driver = reinterpret_cast<Run>(
            DL.getAddressOfSymbol("neverd_x64_run_paged_v6"));
        ASSERT_NE(Instruction.Body, nullptr);
        ASSERT_NE(Instruction.Validate, nullptr);
        ASSERT_NE(Driver, nullptr);
        for (bool Stack : {false, true})
          for (bool TailGPZero : {false, true}) {
            SCOPED_TRACE(llvm::formatv("form={0} {1} llvm={2} zero={3}", Form,
                                       Compiler, LLVM, TailGPZero)
                             .str());
            prepare(Form, Stack, 2);
            const auto Before = State;
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Direct(*Memory, options(TailGPZero));
            uint64_t Budget = 1;
            ASSERT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Direct), &Budget),
                7u)
                << Direct.lastError();
            const auto Expected = State;
            const auto ExpectedRAM = bytes(*Memory);
            X86ModeState Machine;
            Machine.Core.Core = Before;
            X86ModeExecutionOptions Options;
            Options.Long = options(TailGPZero);
            Options.MXCSRMask = 0xffff;
            Options.PagingAddressBits = 48;
            Options.Undefined.emplace();
            Options.Legacy.Segments.FullLimitChoice =
                X86FullSegmentLimitChoice::Wrap;
            Memory = without(0x1d000);
            X86ModeExecution First(*Memory, Options);
            Budget = 1;
            auto Stopped = First.run(Machine, {Instruction}, Budget);
            ASSERT_TRUE(bool(Stopped)) << llvm::toString(Stopped.takeError());
            EXPECT_TRUE(Stopped->OwnerError.empty()) << Stopped->OwnerError;
            EXPECT_EQ(Stopped->Status, X86LegacyExecutionStatus::BackingFault);
            EXPECT_EQ(Budget, Form < 3 ? 0u : 1u);
            EXPECT_EQ(Stopped->InstructionAttempted, Form < 3);
            ASSERT_TRUE(Machine.Pending);
            auto Repaired = repair(*Memory, 0x1d000);
            // Full-mode pending work owns its original options. A new context's
            // opposite profile applies only after the accepted instruction.
            Options.Long = options(!TailGPZero);
            X86ModeExecution Resumed(*Repaired, Options);
            auto Done = Resumed.run(Machine, {}, Budget);
            ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
            EXPECT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
            EXPECT_EQ(Done->Attempts, Form < 3 ? 0u : 1u);
            EXPECT_EQ(Done->Completions, 0u);
            ASSERT_TRUE(Done->Exception);
            EXPECT_EQ(Done->Exception->Vector, 13);
            EXPECT_EQ(Done->Exception->ErrorCode, TailGPZero ? 0u : 0x24u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_FALSE(Machine.Pending);
            EXPECT_EQ(Machine.Core.Core, Expected);
            compareRAM(*Repaired, ExpectedRAM);
            ++Comparisons;
          }
      }
  RecordProperty("return_descriptor_full_mode_comparisons", Comparisons);
}

#endif
