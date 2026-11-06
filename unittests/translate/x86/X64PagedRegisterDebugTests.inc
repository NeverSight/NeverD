//===- X64PagedRegisterDebugTests.inc - Faultable MOV-DR ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original register transfers, competing exceptions and persistent GD entry.
//===----------------------------------------------------------------------===//

class X64PagedDebugRegisterTest : public X64PagedDebugTest {
protected:
  X64PagedExecutionOptions registerOptions(X64PagedDebugState &Debug) {
    auto O = options(Debug);
    O.DebugProfile = X86InstructionDebugProfile::DebugRegisters;
    O.GeneralDetectFrame = X86GeneralDetectFrame::ClearResume;
    return O;
  }
};

TEST_F(X64PagedDebugRegisterTest, RefusalAndAcceptedEntryAreAtomic) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    Schedule Scheduled = nullptr;
    const std::vector<uint8_t> Code{0x0f, 0x23, 0xf8}; // MOV DR7, RAX.
    ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled));
    for (unsigned Case = 0; Case != 8; ++Case) {
      resetDebug(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.RFlags = 0x102d7;
      Core.GPR[0] = 0x10001; // Valid CPU state, unbound data watchpoint.
      X64PagedDebugState Debug;
      auto O = registerOptions(Debug);
      if (Case == 7) {
        Core.GPR[0] = 0x800; // RTM is a control, not a clearable reserved bit.
      } else if (Case) {
        Debug.DR7 |= 0x2000;
        if (Case == 1)
          O.GeneralDetectFrame.reset();
        else if (Case == 2)
          O.GeneralDetectFrame = static_cast<X86GeneralDetectFrame>(255);
        else if (Case < 5) {
          O.GeneralDetectFrame = Case & 1 ? X86GeneralDetectFrame::FaultResume
                                          : X86GeneralDetectFrame::ClearResume;
          put(0x13030, 0x50067); // Accepted entry stops before IDT read.
        } else if (Case == 5) {
          RAM.Memory.front().Bytes[0x1000] = 0x90;
        } else {
          put(0x13008, 0);
        }
      }
      const auto Before = State;
      const auto BeforeDebug = Debug;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, O);
      uint64_t Budget = 1;
      const auto Exit =
          Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
      if (Case < 3 || Case == 7) {
        EXPECT_EQ(Exit, 5u);
        EXPECT_FALSE(Context.lastError().empty());
        EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        EXPECT_EQ(std::memcmp(&Debug, &BeforeDebug, sizeof(Debug)), 0);
        continue;
      }
      if (Case >= 5) {
        EXPECT_EQ(Exit, Case == 5 ? 20u : 7u);
        EXPECT_EQ(std::memcmp(&Debug, &BeforeDebug, sizeof(Debug)), 0);
        if (Case == 5)
          EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        else {
          ASSERT_TRUE(Context.lastResult().Exception);
          EXPECT_EQ(Context.lastResult().Exception->Vector, 14);
        }
        continue;
      }
      ASSERT_EQ(Exit, 18u) << Context.lastError();
      EXPECT_EQ(Debug.DR6, 0xffff2ff0u);
      EXPECT_EQ(Debug.DR7, 0x400u);
      EXPECT_FALSE(Context.lastResult().InstructionCompleted);
      ASSERT_TRUE(Context.lastResult().Delivery);
      ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
      ASSERT_EQ(Memory->physicalMemory()
                    .storeScalar(0x13030, GuestScalarWidth::I64, 0x6067)
                    .Status,
                GuestMemoryAccessStatus::Completed);
      auto Resumed = Context.resume(
          State, *Context.lastResult().Delivery->Continuation, 0xffff);
      ASSERT_TRUE(bool(Resumed)) << llvm::toString(Resumed.takeError());
      EXPECT_EQ(Resumed->Exit, 7u);
      EXPECT_EQ(Core.PC, 0x4010u);
      const auto Stack = Core.GPR[4];
      EXPECT_EQ(Memory->physicalMemory()
                    .loadScalar(Stack, GuestScalarWidth::I64)
                    .Value,
                0x1000u);
      EXPECT_EQ(Memory->physicalMemory()
                    .loadScalar(Stack + 16, GuestScalarWidth::I64)
                    .Value,
                (Case & 1) ? 0x102d7u : 0x2d7u);
      EXPECT_EQ(Core.GPR[0], 0x10001u);
      EXPECT_EQ(Debug.DR6, 0xffff2ff0u);
    }
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedDebugRegisterTest, OriginalRegistersPrefixesAndFaultPriority) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  struct Case {
    std::vector<uint8_t> Bytes;
    unsigned General, Debug;
    bool Write;
  };
  std::vector<Case> Cases;
  for (bool Write : {false, true})
    for (unsigned Debug = 0; Debug != 8; ++Debug)
      for (unsigned General = 0; General != 16; ++General) {
        std::vector<uint8_t> Bytes;
        if (General >= 8)
          Bytes.push_back(0x41);
        Bytes.insert(Bytes.end(),
                     {0x0f, uint8_t(Write ? 0x23 : 0x21),
                      uint8_t(0xc0 | (Debug << 3) | (General & 7))});
        Cases.push_back({Bytes, General, Debug, Write});
      }
  for (uint8_t Prefix : {0x66, 0x67, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0xf2,
                         0xf3, 0xf0, 0x40, 0x42, 0x44, 0x48, 0x4c})
    Cases.push_back({{Prefix, 0x0f, 0x21, 0xe0}, 0, 4, false});
  for (uint8_t Mod : {0x00, 0x40, 0x80})
    for (uint8_t General : {0, 4, 5, 7})
      Cases.push_back(
          {{0x0f, 0x21, uint8_t(Mod | General)}, General, 0, false});
  Cases.push_back(
      {{0xf3, 0x64, 0x66, 0x67, 0x49, 0x0f, 0x21, 0xc3}, 11, 0, false});
  std::vector<uint8_t> Code(Cases.size() * 16, 0xf4);
  std::vector<uint64_t> PCs;
  for (unsigned I = 0; I != Cases.size(); ++I) {
    std::copy(Cases[I].Bytes.begin(), Cases[I].Bytes.end(),
              Code.begin() + I * 16);
    PCs.push_back(0x1000 + I * 16);
  }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string Optimization : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled, PCs,
                                             Compiler, Optimization));
        for (unsigned I = 0; I != Cases.size(); ++I)
          for (unsigned Setup = 0; Setup != 15; ++Setup) {
            const auto &C = Cases[I];
            // Keep a successful TF read of DR6/7 from replacing the stack
            // with an unmapped address. No bits of any comparison are masked.
            if (Setup == 3 && C.General == 4 && !C.Write && C.Debug >= 4)
              continue;
            const unsigned CPL = Setup >= 7 && Setup <= 9 ? Setup - 6 : 0;
            resetDebug(Code, CPL);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = PCs[I];
            Core.RFlags = 0x2d7 | ((Setup == 3 || Setup == 4) ? 0x100 : 0) |
                          ((Setup == 2 || Setup == 5) ? 0x10000 : 0);
            const bool GD = Setup == 1 || Setup == 4 || Setup == 5 ||
                            Setup == 6 || (Setup >= 7 && Setup <= 12);
            X64PagedDebugState Debug;
            Debug.DR6 = Setup & 1 ? 0xffffafff : 0xffff0fff;
            Debug.DR7 = GD ? 0x2400 : 0x400;
            Debug.DR = {0x1e000, 0x1d000, 0x1c000, 0x1b000};
            Core.GPR[C.General] = C.General == 4
                                      ? (Setup == 12 ? 0x1e000 : 0x2000)
                                  : Setup == 13 ? 0x8000000012345678ULL
                                  : Setup == 14 ? 0xd000
                                                : 0x2305;
            if (Setup == 10 || Setup == 11)
              Controls.CR4 |= 8;
            if (Setup == 12)
              put(0x13000 + (Core.PC / 4096) * 8, 0);
            X64ContinuationEventV1 E;
            E.SSBlocked = Setup == 6;
            CPU.DebugRegisters.emplace();
            CPU.DebugRegisters->dr6 = Debug.DR6;
            CPU.DebugRegisters->dr7 = Debug.DR7;
            std::copy(Debug.DR.begin(), Debug.DR.end(), CPU.DebugRegisters->db);
            X64ContinuationStateV6 Native;
            bool Fault = false;
            SCOPED_TRACE(testing::Message()
                         << "case=" << I << " setup=" << Setup << " "
                         << Compiler << Optimization << " llvm=" << LLVM);
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, C.Bytes.size(),
                Native, Fault, true, &E, Controls.CR4, true, false, false));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory, registerOptions(Debug));
            Shadows Shadow;
            Shadow.SSBlocked = E.SSBlocked;
            Producer P;
            P.Ready = 0;
            uint64_t Budget = 1, Polls = 1;
            Scheduled(&State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                      &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
            ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
            EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
            for (unsigned Slot = 0; Slot != 4; ++Slot)
              EXPECT_EQ(Debug.DR[Slot], CPU.DebugRegisters->db[Slot]);
            ASSERT_NO_FATAL_FAILURE(
                expectPagedSegmentCPU(State, Native, *Memory, CPU));
            const auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
            const auto Difference =
                std::mismatch(Actual.begin(), Actual.end(), CPU.RAM);
            EXPECT_EQ(Difference.first, Actual.end())
                << "RAM difference at " << std::hex
                << std::distance(Actual.begin(), Difference.first);
            EXPECT_EQ(Shadow.SSBlocked,
                      bool(CPU.InterruptShadow & KVM_X86_SHADOW_INT_MOV_SS));
            if (HasFailure())
              return;
            ++Comparisons;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
}
#endif

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedDebugRegisterTest,
       OriginalHandlerClearsGeneralDetectAndRetries) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  std::vector<uint8_t> Code(0x301c, 0xf4);
  const std::vector<uint8_t> Move{0x0f, 0x21, 0xc0};
  const std::vector<uint8_t> Handler{
      0x0f, 0x21, 0xf9, // Read DR7 in the real #DB handler.
      0xfe, 0x04, 0x25, 0x00, 0x00, 0x03, 0x00, // INC byte [0x30000].
      0x48, 0xcf}; // IRETQ retries the original MOV.
  std::copy(Move.begin(), Move.end(), Code.begin());
  std::copy(Handler.begin(), Handler.end(), Code.begin() + 0x3010);
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(NeverC);
  unsigned Comparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string Optimization : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compileDebug(Code, LLVM, Driver, Scheduled,
                                             {0x1000, 0x4010, 0x4013, 0x401a},
                                             Compiler, Optimization));
        for (bool RF : {false, true}) {
          resetDebug(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.RFlags = RF ? 0x102d7 : 0x2d7;
          put(0x30000, 0);
          X64PagedDebugState Debug;
          Debug.DR7 = 0x2400;
          Debug.DR[0] = 0xffff800012345678;
          CPU.DebugRegisters.emplace();
          CPU.DebugRegisters->dr6 = Debug.DR6;
          CPU.DebugRegisters->dr7 = Debug.DR7;
          std::copy(Debug.DR.begin(), Debug.DR.end(), CPU.DebugRegisters->db);
          X64ContinuationStateV6 Native;
          bool Fault = false;
          ASSERT_NO_FATAL_FAILURE(CPU.execute(
              RAM.Memory.front().Bytes, State, Controls.CR0, Move.size(),
              Native, Fault, true, nullptr, Controls.CR4, true, false));
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, registerOptions(Debug));
          Shadows Shadow;
          Producer P;
          P.Ready = 0;
          uint64_t Budget = 5, Polls = 8;
          for (unsigned Attempts = 0; Attempts != 5 && Core.PC != 0x1003;
               ++Attempts) {
            Scheduled(&State, reinterpret_cast<uint8_t *>(&Context), &Budget,
                      &Shadow, &Polls, Producer::peek, Producer::accepted, &P);
            ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
          }
          EXPECT_EQ(Core.PC, 0x1003u);
          EXPECT_EQ(Core.GPR[1], 0x400u);
          EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
          EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
          ASSERT_NO_FATAL_FAILURE(
              expectPagedSegmentCPU(State, Native, *Memory, CPU));
          const auto Actual =
              Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
          EXPECT_EQ(std::mismatch(Actual.begin(), Actual.end(), CPU.RAM).first,
                    Actual.end());
          EXPECT_EQ(Actual[0x30000], 1);
          if (HasFailure())
            return;
          ++Comparisons;
        }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("round_trip_comparisons", Comparisons);
}
#endif
