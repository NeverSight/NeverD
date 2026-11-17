//===- X64PagedDescriptorCanonicalityTests.inc - Descriptor ranges -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original and generated code64 selector loads at descriptor boundaries.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X64PagedDescriptorCanonicalityTest
    : public X64PagedDescriptorBoundaryFixture {
protected:
  static std::vector<uint8_t> code(unsigned Form) {
    switch (Form) {
    case 0:
      return {0x8e, 0xe0};
    case 1:
      return {0x8e, 0x27};
    case 2:
      return {0x0f, 0xa1};
    case 3:
      return {0x66, 0x0f, 0xa1};
    case 4:
      return {0x66, 0x0f, 0xb4, 0x07};
    case 5:
      return {0x0f, 0xb4, 0x07};
    case 6:
      return {0x48, 0x0f, 0xb4, 0x07};
    case 7:
      return {0x0f, 0xb5, 0x07};
    default:
      return {0x0f, 0xb2, 0x07};
    }
  }

  void boundary(unsigned Form, unsigned Case) {
    descriptorBoundary(code(Form), Case, 0x00cf93022000ffffULL);
    auto &Core = State.Core.Core.Core.Core;
    Core.GPR[0] = Form == 0 ? 0x24 : 0x1122334455667788ULL;
    if (Form < 4) {
      put(0x30000, 0x24);
      if (Form >= 2)
        put(Core.GPR[4], 0x24);
    } else if (Form == 4) {
      put(0x30000, 0x00245678);
    } else if (Form == 6) {
      put(0x30000, 0x1122334455667788ULL);
      put(0x30008, 0x24);
    } else {
      put(0x30000, 0x002412345678ULL);
    }
  }

  static X64PagedExecutionOptions options(bool TailGPZero) {
    X64PagedExecutionOptions Options;
    Options.AlignmentPriority = X64PagedAlignmentPriority::AfterPaging;
    Options.FarPointerProfile =
        TailGPZero ? X64PagedFarPointerProfile::OffsetFirstTailGPZero
                   : X64PagedFarPointerProfile::OffsetFirst;
    Options.SegmentMoveProfile =
        TailGPZero ? X64PagedSegmentMoveProfile::SelectorFirstTailGPZero
                   : X64PagedSegmentMoveProfile::SelectorFirst;
    return Options;
  }
};

TEST_F(X64PagedDescriptorCanonicalityTest, ObserveOriginalDescriptorRanges) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Observations = 0;
  for (unsigned Form = 0; Form != 9; ++Form)
    for (unsigned Case = 0; Case != 13; ++Case) {
      SCOPED_TRACE(llvm::formatv("form={0} case={1}", Form, Case).str());
      X64PagedNativeCPU CPU;
      ASSERT_NO_FATAL_FAILURE(CPU.initialize());
      if (IsSkipped())
        return;
      if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
        GTEST_SKIP() << "Descriptor oracle requires KVM pre-fault memory";
      ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
      if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
        GTEST_SKIP() << "No descriptor profile bound for this Intel core type";
      CPU.PrefaultMemory = true;
      Features.PhysicalAddressBits = CPU.PhysicalBits;
      boundary(Form, Case);
      X64ContinuationStateV6 Native;
      bool Fault = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(
          RAM.Memory.front().Bytes, State, Controls.CR0, code(Form).size(),
          Native, Fault, false, nullptr, Controls.CR4, true, false));
      const auto &R = Native.Core.Core.Core.Core;
      ASSERT_EQ(Fault, faults(Case));
      uint64_t Error = 0;
      if (Fault) {
        ASSERT_LT(R.GPR[4] + 8, X64PagedNativeCPU::Size);
        for (unsigned I = 0; I != 8; ++I)
          Error |= uint64_t(CPU.RAM[R.GPR[4] + I]) << (I * 8);
      }
      const uint64_t PML4 =
          0x10000 + ((descriptorAddress(Case) >> 39) & 0x1ff) * 8;
      const std::optional<uint64_t> PTE =
          Case == 1 || Case == 4 ? std::nullopt
          : Case == 8            ? std::optional<uint64_t>(0x16000)
          : Case == 9            ? std::optional<uint64_t>(0x13000)
                                 : std::optional<uint64_t>(0x16ff8);
      const auto Leaf =
          PTE ? llvm::formatv("{0:x}={1:x}", *PTE, unsigned(CPU.RAM[*PTE]))
                    .str()
              : "unmapped";
      RecordProperty(
          "descriptor_range_" + std::to_string(Form) + "_" +
              std::to_string(Case),
          llvm::formatv("core={0:x};pc={1:x};error={2:x};cr2={3:x};"
                        "pml4={4:x}={5:x};pte={6};rax={7:x};fs={8:x};"
                        "base={9:x};low_pte={10:x}",
                        CPU.ReturnCoreType, R.PC, Error, Native.Core.Core.CR2,
                        PML4, unsigned(CPU.RAM[PML4]), Leaf, R.GPR[0],
                        Native.FS.Selector, R.FSBase,
                        unsigned(CPU.RAM[0x13000]))
              .str());
      EXPECT_EQ(R.PC, !Fault ? 0x1000 + code(Form).size()
                             : 0x4000 + (pageFault(Case) ? 14 : 13) * 16);
      EXPECT_EQ(Error, error(Case, CPU.ReturnCoreType == 0x40));
      EXPECT_EQ(Native.Core.Core.CR2, cr2(Case));
      ++Observations;
    }
  RecordProperty("original_code64_descriptor_range_observations", Observations);
}

TEST_F(X64PagedDescriptorCanonicalityTest,
       GeneratedRangesMatchOriginalInstructions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (unsigned Form = 0; Form != 9; ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          const auto Code = code(Form);
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, {0x1000},
                                          Compiler, 0x1000, nullptr,
                                          X64NullSegmentBasePolicy::Clear));
          for (unsigned Case = 0; Case != 13; ++Case) {
            SCOPED_TRACE(llvm::formatv("form={0} case={1} {2} llvm={3} {4}",
                                       Form, Case, Compiler, LLVM, O)
                             .str());
            X64PagedNativeCPU CPU;
            ASSERT_NO_FATAL_FAILURE(CPU.initialize());
            if (IsSkipped())
              return;
            if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
              GTEST_SKIP() << "Descriptor oracle requires KVM pre-fault memory";
            ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
            if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
              GTEST_SKIP()
                  << "No descriptor profile bound for this Intel core type";
            CPU.PrefaultMemory = true;
            Features.PhysicalAddressBits = CPU.PhysicalBits;
            boundary(Form, Case);
            X64ContinuationStateV6 Native;
            bool Fault = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, Code.size(),
                Native, Fault, false, nullptr, Controls.CR4, true, false));
            ASSERT_EQ(Fault, faults(Case));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            const bool TailGPZero = CPU.ReturnCoreType == 0x40;
            X64PagedExecution Context(*Memory, options(TailGPZero));
            uint64_t Budget = 1;
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                Fault ? 7u : 9u)
                << Context.lastError();
            EXPECT_EQ(Budget, 0u);
            EXPECT_TRUE(Context.lastResult().InstructionAttempted);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Fault);
            if (Fault) {
              ASSERT_TRUE(Context.lastResult().Exception);
              EXPECT_EQ(Context.lastResult().Exception->Vector,
                        pageFault(Case) ? 14 : 13);
              EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                        error(Case, TailGPZero));
              EXPECT_EQ(Context.lastResult().Exception->CR2,
                        pageFault(Case) ? std::optional<uint64_t>(cr2(Case))
                                        : std::nullopt);
            }
            EXPECT_EQ(State, Native);
            compareRAM(*Memory, {CPU.RAM, X64PagedNativeCPU::Size});
            ++Comparisons;
          }
        }
  RecordProperty("original_code64_descriptor_range_comparisons", Comparisons);
}

TEST_F(X64PagedDescriptorCanonicalityTest, ResumeFreezesDescriptorProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Form = 1; Form != 9; ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const auto Code = code(Form);
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {0x1000},
                                        Compiler, 0x1000, nullptr,
                                        X64NullSegmentBasePolicy::Clear));
        for (bool TailGPZero : {false, true}) {
          SCOPED_TRACE(llvm::formatv("form={0} {1} llvm={2} zero={3}", Form,
                                     Compiler, LLVM, TailGPZero)
                           .str());
          boundary(Form, 2);
          const auto Before = State;
          const auto Options = options(TailGPZero);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Direct(*Memory, Options);
          uint64_t Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Direct), &Budget), 7u)
              << Direct.lastError();
          ASSERT_TRUE(Direct.lastResult().Exception);
          EXPECT_EQ(Direct.lastResult().Exception->Vector, 13);
          EXPECT_EQ(Direct.lastResult().Exception->ErrorCode,
                    TailGPZero ? 0u : 0x24u);
          const auto Expected = State;
          const auto ExpectedRAM = bytes(*Memory);
          State = Before;
          const unsigned Page = Form == 2 || Form == 3 ? 0x1e000 : 0x30000;
          Memory = without(Page);
          X64PagedExecution First(*Memory, Options);
          Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&First), &Budget), 18u)
              << First.lastError();
          EXPECT_EQ(Budget, 1u);
          EXPECT_FALSE(First.lastResult().InstructionAttempted);
          EXPECT_EQ(State, Before);
          const auto Pending = First.continuation();
          ASSERT_TRUE(Pending);
          auto Repaired = repair(*Memory, Page);
          const auto BeforeRAM = bytes(*Repaired);
          for (bool Far : {false, true}) {
            auto Changed = Options;
            if (Far)
              Changed.FarPointerProfile =
                  options(!TailGPZero).FarPointerProfile;
            else
              Changed.SegmentMoveProfile =
                  options(!TailGPZero).SegmentMoveProfile;
            X64PagedExecution Refused(*Repaired, Changed, Pending);
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Refused), &Budget),
                5u);
            EXPECT_FALSE(Refused.lastError().empty());
            EXPECT_EQ(Refused.continuation(), Pending);
            EXPECT_FALSE(Refused.lastResult().InstructionAttempted);
            EXPECT_FALSE(Refused.lastResult().InstructionResumed);
            EXPECT_EQ(Budget, 1u);
            EXPECT_EQ(State, Before);
            compareRAM(*Repaired, BeforeRAM);
          }
          X64PagedExecution Resumed(*Repaired, Options, Pending);
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              7u)
              << Resumed.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_TRUE(Resumed.lastResult().InstructionResumed);
          EXPECT_TRUE(Resumed.lastResult().InstructionAttempted);
          EXPECT_FALSE(Resumed.lastResult().InstructionCompleted);
          ASSERT_TRUE(Resumed.lastResult().Exception);
          EXPECT_EQ(Resumed.lastResult().Exception->ErrorCode,
                    TailGPZero ? 0u : 0x24u);
          EXPECT_FALSE(Resumed.continuation());
          EXPECT_EQ(State, Expected);
          compareRAM(*Repaired, ExpectedRAM);
          ++Comparisons;
        }
      }
  RecordProperty("descriptor_profile_resume_comparisons", Comparisons);
}

TEST_F(X64PagedDescriptorCanonicalityTest, FullModeRetainsAcceptedProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Form : {1u, 5u})
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const auto Code = code(Form);
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
        const auto Name = std::string("descriptor-mode-") +
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
        for (bool TailGPZero : {false, true}) {
          SCOPED_TRACE(llvm::formatv("form={0} {1} llvm={2} zero={3}", Form,
                                     Compiler, LLVM, TailGPZero)
                           .str());
          boundary(Form, 2);
          const auto Before = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Direct(*Memory, options(TailGPZero));
          uint64_t Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Direct), &Budget), 7u)
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
          Memory = without(0x30000);
          X86ModeExecution First(*Memory, Options);
          Budget = 1;
          auto Stopped = First.run(Machine, {Instruction}, Budget);
          ASSERT_TRUE(bool(Stopped)) << llvm::toString(Stopped.takeError());
          EXPECT_TRUE(Stopped->OwnerError.empty()) << Stopped->OwnerError;
          EXPECT_EQ(Stopped->Status, X86LegacyExecutionStatus::BackingFault);
          EXPECT_EQ(Budget, 1u);
          EXPECT_FALSE(Stopped->InstructionAttempted);
          ASSERT_TRUE(Machine.Pending);
          auto Repaired = repair(*Memory, 0x30000);
          // Full-mode pending work owns its original options. A new context's
          // opposite profile applies only after the accepted instruction.
          Options.Long = options(!TailGPZero);
          X86ModeExecution Resumed(*Repaired, Options);
          auto Done = Resumed.run(Machine, {}, Budget);
          ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
          EXPECT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
          EXPECT_EQ(Done->Attempts, 1u);
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
  RecordProperty("descriptor_profile_full_mode_comparisons", Comparisons);
}

TEST_F(X64PagedDescriptorCanonicalityTest, UnknownProfilesRefuseBeforeEffects) {
  for (unsigned Form : {0u, 5u}) {
    boundary(Form, 0);
    const auto Before = State;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    const auto BeforeRAM = bytes(*Memory);
    auto Result =
        Form == 0
            ? moveX64PagedSegment(State, *Memory, code(Form),
                                  X64PagedAlignmentPriority::AfterPaging,
                                  static_cast<X64PagedSegmentMoveProfile>(0xff),
                                  X64NullSegmentBasePolicy::Clear)
            : loadX64PagedFarPointer(
                  State, *Memory, code(Form),
                  X64PagedAlignmentPriority::AfterPaging,
                  static_cast<X64PagedFarPointerProfile>(0xff),
                  X64NullSegmentBasePolicy::Clear);
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(State, Before);
    compareRAM(*Memory, BeforeRAM);
  }
}

#endif
