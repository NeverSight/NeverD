//===- X64PagedDebugFunctionTests.inc - Debug activation receipts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute public v2 function/call exports through the actual paged runtime.
//===----------------------------------------------------------------------===//

class X64PagedDebugFunctionTest : public X64PagedWatchpointTest {
protected:
  struct Function {
    uint64_t ReturnSlot = 0, Phase = 0, Status = 0, Value = 0;
  };
  struct Call {
    Function Activation;
    uint64_t Entry = 0, Value = 0;
  };
  struct Site {
    Call Callee;
    uint64_t Address = 0, Prepared = 0;
  };
  struct Result {
    uint64_t Status, Completed, Value;
  };
  struct HandlerContext {
    uint64_t Site = 0, ReturnSlot = 0, Ready = 0;
  };
  struct API {
    int (*Begin)(const X64ContinuationStateV6 *, Function *) = nullptr;
    uint64_t (*RunFunction)(X64ContinuationStateV6 *, uint8_t *, uint64_t *,
                            Shadows *, uint64_t *, Function *) = nullptr;
    uint64_t (*RunRegion)(X64ContinuationStateV6 *, uint8_t *, uint64_t *,
                          Shadows *, uint64_t *) = nullptr;
    using SiteRun = Result (*)(X64ContinuationStateV6 *, uint8_t *, uint64_t *,
                               Shadows *, uint64_t *, Site *);
    using CallRun = Result (*)(X64ContinuationStateV6 *, uint8_t *, uint64_t *,
                               Shadows *, uint64_t *, Call *);
    SiteRun StartSite = nullptr, ResumeSite = nullptr;
    CallRun StartCall = nullptr, ResumeCall = nullptr;
    int (*BeginHandler)(const Site *, HandlerContext *) = nullptr;
    Result (*ResumeHandler)(X64ContinuationStateV6 *, uint8_t *, uint64_t *,
                            Shadows *, uint64_t *, Site *,
                            HandlerContext *) = nullptr;
  };
  static_assert(sizeof(Function) == 32 && sizeof(Call) == 48 &&
                sizeof(Site) == 64);

  static std::vector<std::string> compilers() {
    std::vector<std::string> Result{NEVERD_TEST_CLANG};
    if (const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
      Result.emplace_back(NeverC);
    return Result;
  }

  static std::vector<uint8_t> program(bool Handler) {
    std::vector<uint8_t> Code(Handler ? 0x3030 : 0x30, 0xf4);
    const auto Put = [&](unsigned Offset,
                         std::initializer_list<uint8_t> Bytes) {
      std::copy(Bytes.begin(), Bytes.end(), Code.begin() + Offset);
    };
    Put(0, {0xe8, 0x0b, 0, 0, 0, 0xc3}); // CALL 1010; caller RET.
    Put(0x10, {0xc3});
    Put(0x20, {0xc2, 0x10, 0});
    if (Handler)
      Put(0x3010,
          {0x48, 0xff, 0xc0,                                  // INC RAX.
           0x48, 0x81, 0x4c, 0x24, 0x10, 0,    0,    1,    0, // Set saved RF.
           0x48, 0x81, 0x64, 0x24, 0x10, 0xff, 0xfe, 0xff, 0xff,
           0x48, 0xcf}); // Clear saved TF; IRETQ.
    return Code;
  }

  void resetFunction(bool Handler, unsigned CPL = 0) {
    resetDebug(program(Handler), CPL);
    // Keep post-transfer debug frames away from the watched return cell.
    put(0x6010, 0x4010 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                    (uint64_t(0x8e) << 40));
  }

  void compileFunctions(bool LLVM, bool Scheduled, bool Pauses, bool Handler,
                        API &Functions,
                        const std::string &Compiler = NEVERD_TEST_CLANG,
                        const std::string &Optimization = "-O2",
                        unsigned ResultBytes = 8) {
    llvm::json::Array Entries{0x1000, 0x1005, 0x1010, 0x1020};
    if (Handler)
      Entries.push_back(0x4010);
    llvm::json::Object Settings{
        {"profile", "kernel-events-v6"},
        {"mxcsr_mask", "0xffff"},
        {"paging_address_bits", 48},
        {"paged_memory", true},
        {"paged_debug", true},
        {"function_context", true},
        {"follow_internal_calls", true},
        {"scheduled_events", Scheduled},
        {"pause_call_sites", Pauses},
        {"entries", std::move(Entries)},
        {"function_calls", llvm::json::Array{llvm::json::Object{
                               {"entry", 0x1010},
                               {"abi", "win64"},
                               {"argument_bytes", llvm::json::Array{}},
                               {"result_bytes", ResultBytes},
                               {"call_sites", llvm::json::Array{0x1000}}}}}};
    const auto Request =
        llvm::formatv("{0}", llvm::json::Value(std::move(Settings))).str();
    auto Export = exportX64Continuation(image(program(Handler)), Request, LLVM);
    ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
    const auto Manifest = llvm::cantFail(llvm::json::parse(Export->Manifest));
    ASSERT_EQ(Manifest.getAsObject()->getInteger("native_requests"), 0);
    ASSERT_EQ(Manifest.getAsObject()
                  ->getObject("function_context")
                  ->getString("context_type"),
              "neverd_x64_function_context_v2");
    ASSERT_EQ(Manifest.getAsObject()
                  ->getObject("function_calls")
                  ->getString("context_type"),
              "neverd_x64_call_context_v2");
    std::string Extra;
    llvm::raw_string_ostream OS(Extra);
    OS << R"C(
_Static_assert(sizeof(neverd_x64_function_context_v2) == 32, "activation ABI");
_Static_assert(sizeof(neverd_x64_call_context_v2) == 48, "call ABI");
_Static_assert(sizeof(neverd_x64_call_site_context_v2) == 64, "site ABI");
static uint64_t (*transfer_bridge)(uint8_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *);
void bind_transfer(uint64_t (*p)(uint8_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *)) {
    transfer_bridge = p;
}
uint64_t neverd_x64_paged_transfer_v1(uint8_t *c, uint64_t *i, uint64_t *p,
                                     uint64_t *s, uint64_t *a) {
    return transfer_bridge(c, i, p, s, a);
}
)C";
    if (Scheduled)
      OS << R"C(
static int no_event(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *b, neverd_x64_pending_interrupt_v1 *p, void *c) {
    (void)s; (void)b; (void)p; (void)c; return 0;
}
static void no_ack(const neverd_x64_pending_interrupt_v1 *p, void *c) { (void)p; (void)c; }
)C";
    else
      OS << "typedef struct { uint8_t Bytes[8]; } "
            "neverd_x64_interruptibility_v1;\n";
    const std::string Prefix =
        Scheduled ? "paged_debug_events_" : "paged_debug_";
    const auto Arguments =
        Scheduled
            ? "state, context, budget, shadows, polls, no_event, no_ack, 0"
            : "state, context, budget";
    const auto Parameters =
        "neverd_x64_state_v6 *state, uint8_t *context, uint64_t *budget, "
        "neverd_x64_interruptibility_v1 *shadows, uint64_t *polls";
    OS << "uint64_t test_function(" << Parameters
       << ", neverd_x64_function_context_v2 *function) {\n    return "
       << (Scheduled ? "neverd_x64_continue_paged_debug_events_function_v6"
                     : "neverd_x64_run_paged_debug_function_v6")
       << "(" << Arguments << ", function);\n}\n";
    OS << "uint64_t test_region(" << Parameters << ") {\n    return "
       << (Scheduled ? "neverd_x64_continue_paged_events_v6"
                     : "neverd_x64_run_paged_v6")
       << "(" << Arguments << ");\n}\n";
    for (bool Resume : {false, true})
      for (bool AtSite : {false, true}) {
        OS << "neverd_x64_call_result_v1 test_"
           << (Resume ? "resume_" : "start_") << (AtSite ? "site(" : "call(")
           << Parameters << ", "
           << (AtSite ? "neverd_x64_call_site_context_v2 *call"
                      : "neverd_x64_call_context_v2 *call")
           << ") {\n    return neverd_x64_" << (Resume ? "resume_" : "call_")
           << Prefix << (AtSite ? "site_1000" : "1010")
           << "_v6(state, context, budget, call";
        if (Scheduled)
          OS << ", shadows, polls, no_event, no_ack, 0";
        OS << ");\n}\n";
      }
    if (Pauses) {
      OS << "_Static_assert(sizeof(neverd_x64_call_site_handler_context_v1) == "
            "24, \"handler ABI\");\n";
      OS << "neverd_x64_call_result_v1 test_resume_handler(" << Parameters
         << ", neverd_x64_call_site_context_v2 *site, "
         << "neverd_x64_call_site_handler_context_v1 *handler) {\n"
         << "    return neverd_x64_resume_handler_" << Prefix
         << "site_1000_v6(state, context, budget, site";
      if (Scheduled)
        OS << ", shadows, polls, no_event, no_ack, 0";
      OS << ", handler);\n}\n";
    }
    llvm::sys::DynamicLibrary DL;
    const auto Name =
        std::string(
            testing::UnitTest::GetInstance()->current_test_info()->name()) +
        "-debug-function-" + std::string(LLVM ? "llvm-" : "high-") +
        std::to_string(Compilation++);
    ASSERT_NO_FATAL_FAILURE(
        compileProgram(*Export, Name, Compiler, Optimization, DL, Extra));
    using Transfer = decltype(&neverd_x64_paged_transfer_v1);
    const auto Binder = reinterpret_cast<void (*)(Transfer)>(
        DL.getAddressOfSymbol("bind_transfer"));
    ASSERT_NE(Binder, nullptr);
    Binder(&neverd_x64_paged_transfer_v1);
#define GET_FUNCTION(Field, Name)                                              \
  Functions.Field = reinterpret_cast<decltype(Functions.Field)>(               \
      DL.getAddressOfSymbol(Name));                                            \
  ASSERT_NE(Functions.Field, nullptr)
    GET_FUNCTION(Begin, "neverd_x64_begin_debug_function_v6");
    GET_FUNCTION(RunFunction, "test_function");
    GET_FUNCTION(RunRegion, "test_region");
    GET_FUNCTION(StartSite, "test_start_site");
    GET_FUNCTION(ResumeSite, "test_resume_site");
    GET_FUNCTION(StartCall, "test_start_call");
    GET_FUNCTION(ResumeCall, "test_resume_call");
    if (Pauses) {
      GET_FUNCTION(BeginHandler, "neverd_x64_begin_debug_call_site_handler_v1");
      GET_FUNCTION(ResumeHandler, "test_resume_handler");
    }
#undef GET_FUNCTION
  }
};

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedDebugFunctionTest, OriginalTransfersRetainPreDebugState) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (CPU.KVM < 0)
    GTEST_SKIP() << "KVM unavailable";
  if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
    GTEST_SKIP() << "Original debug oracle requires KVM pre-fault memory";
  CPU.PrefaultMemory = true;
  unsigned Comparisons = 0;
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true})
        for (const std::string Optimization : {"-O0", "-O2"}) {
          API A;
          ASSERT_NO_FATAL_FAILURE(compileFunctions(
              LLVM, Scheduled, false, false, A, Compiler, Optimization));
          for (uint64_t PC : {0x1000u, 0x1005u, 0x1020u})
            for (unsigned CPL : {0u, 3u})
              for (bool Data : {false, true}) {
                SCOPED_TRACE(testing::Message()
                             << Compiler << LLVM << Scheduled << Optimization
                             << " pc=" << PC << " cpl=" << CPL
                             << " data=" << Data);
                resetFunction(false, CPL);
                auto &Core = State.Core.Core.Core.Core;
                Core.PC = PC;
                Core.RFlags = Data ? 0x202 : 0x302;
                put(Core.GPR[4], 0x1100);
                X64PagedDebugState Debug;
                if (Data) {
                  Debug.DR7 = 0xb0401;
                  Debug.DR[0] = Core.GPR[4] - (PC == 0x1000 ? 8 : 0);
                }
                CPU.DebugRegisters.emplace();
                CPU.DebugRegisters->dr6 = Debug.DR6;
                CPU.DebugRegisters->dr7 = Debug.DR7;
                std::copy(Debug.DR.begin(), Debug.DR.end(),
                          CPU.DebugRegisters->db);
                X64ContinuationStateV6 Native;
                bool Fault = false;
                ASSERT_NO_FATAL_FAILURE(CPU.execute(
                    RAM.Memory.front().Bytes, State, Controls.CR0, 0, Native,
                    Fault, true, nullptr, Controls.CR4, true, false));
                ASSERT_TRUE(Fault);
                Memory = llvm::cantFail(
                    X86PagedMemory::create(RAM, Features, Controls));
                X64PagedExecution Context(*Memory, watchOptions(Debug));
                auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
                uint64_t Budget = 1, Polls = 1;
                Shadows Shadow;
                Function F;
                Site S;
                if (PC == 0x1000) {
                  const auto R =
                      A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
                  ASSERT_EQ(R.Status, 7u) << Context.lastError();
                  EXPECT_EQ(R.Completed, 0u);
                  EXPECT_EQ(S.Callee.Activation.Phase, 1u);
                } else {
                  ASSERT_EQ(A.Begin(&State, &F), 1);
                  ASSERT_EQ(A.RunFunction(&State, Opaque, &Budget, &Shadow,
                                          &Polls, &F),
                            7u)
                      << Context.lastError();
                  EXPECT_EQ(F.Phase, 2u);
                  EXPECT_EQ(F.Status, 7u);
                  EXPECT_EQ(F.Value, Native.Core.Core.Core.Core.GPR[0]);
                }
                EXPECT_EQ(Budget, 0u);
                expectPagedSegmentCPU(State, Native, *Memory, CPU);
                EXPECT_EQ(Memory->physicalMemory()
                              .snapshotMemoryRegions()
                              .front()
                              .Bytes,
                          std::vector<uint8_t>(CPU.RAM, CPU.RAM + 0x40000));
                EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
                EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
                uint64_t Instruction = 0, Target = 0, SP = 0, Value = 0;
                EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, &Instruction,
                                                       &Target, &SP, &Value),
                          PC == 0x1000 ? 1u : 3u);
                uint64_t SavedPC = 0, SavedSP = 0;
                const auto Frame = Native.Core.Core.Core.Core.GPR[4];
                std::memcpy(&SavedPC, CPU.RAM + Frame, sizeof(SavedPC));
                std::memcpy(&SavedSP, CPU.RAM + Frame + 24, sizeof(SavedSP));
                EXPECT_EQ(Instruction, PC);
                EXPECT_EQ(Target, SavedPC);
                EXPECT_EQ(SP, SavedSP);
                EXPECT_EQ(Value, Native.Core.Core.Core.Core.GPR[0]);
                if (PC == 0x1000)
                  EXPECT_EQ(S.Callee.Activation.ReturnSlot, SavedSP);
                ++Comparisons;
              }
        }
  RecordProperty("original_comparisons", Comparisons);
}
#endif

TEST_F(X64PagedDebugFunctionTest, ReturnCompletionKeepsActualStopsAndValue) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true})
        for (unsigned ResultBytes : {1u, 2u, 4u, 8u}) {
          API A;
          ASSERT_NO_FATAL_FAILURE(compileFunctions(
              LLVM, Scheduled, true, true, A, Compiler, "-O2", ResultBytes));
          for (unsigned Case = 0; Case != 5; ++Case) {
            SCOPED_TRACE(testing::Message()
                         << LLVM << Scheduled << " case=" << Case);
            resetFunction(true);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = 0x1010;
            const auto OriginalValue = Core.GPR[0];
            const auto ExpectedValue =
                OriginalValue &
                (ResultBytes == 8 ? UINT64_MAX
                                  : (uint64_t(1) << (ResultBytes * 8)) - 1);
            put(Core.GPR[4], 0x1100);
            if (Case == 1)
              put(0x13030, 0x50067); // Missing host backing for the IDT.
            if (Case == 4)
              State.IDTR.Limit = 0; // #DB, #GP and #DF cannot enter.
            if (Case == 3) {
              put(0x13000 + (Core.GPR[4] >> 12) * 8, 0); // RET operand #PF.
              put(0x60e0, 0x40e0 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                              (uint64_t(0x8e) << 40));
            }
            X64PagedDebugState Debug;
            auto Options = watchOptions(Debug);
            if (Case == 2) {
              Debug.DR7 = 0xb0401;
              Debug.DR[0] =
                  0x6010; // Successful #DB entry generates another #DB.
              Options.EventDelivery.MaxAttempts = 1;
            }
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory, Options);
            auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
            uint64_t Budget = 9, Polls = 9;
            Shadows Shadow;
            Call C;
            const auto R =
                A.StartCall(&State, Opaque, &Budget, &Shadow, &Polls, &C);
            const uint64_t Expected = Case == 1   ? 18
                                      : Case == 2 ? 23
                                      : Case == 4 ? 19
                                                  : 7;
            ASSERT_EQ(R.Status, Expected) << Context.lastError();
            EXPECT_EQ(Budget, 8u);
            EXPECT_EQ(R.Completed, unsigned(Case != 3));
            EXPECT_EQ(C.Activation.Phase, Case == 3 ? 1u : 2u);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, Case != 3);
            if (Case == 3) {
              uint64_t Unchanged = 0x55;
              EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, &Unchanged,
                                                     &Unchanged, &Unchanged,
                                                     &Unchanged),
                        0u);
              EXPECT_EQ(Unchanged, 0x55u);
              continue;
            }
            EXPECT_EQ(R.Value, ExpectedValue);
            const auto StoppedState = State;
            const auto StoppedRAM =
                Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
            const auto StoppedPolls = Polls;
            const auto Again =
                A.ResumeCall(&State, Opaque, &Budget, &Shadow, &Polls, &C);
            EXPECT_EQ(Again.Status, Expected);
            EXPECT_EQ(Again.Completed, 1u);
            EXPECT_EQ(Again.Value, ExpectedValue);
            EXPECT_EQ(Budget, 8u);
            EXPECT_EQ(Polls, StoppedPolls);
            EXPECT_EQ(std::memcmp(&State, &StoppedState, sizeof(State)), 0);
            EXPECT_EQ(
                Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
                StoppedRAM);
            if (Case == 4)
              continue; // Shutdown has no resumable handler state.
            if (Case == 1 || Case == 2) {
              ASSERT_TRUE(Context.lastResult().Delivery->Continuation);
              if (Case == 1)
                ASSERT_EQ(
                    Memory->physicalMemory()
                        .storeScalar(0x13030, GuestScalarWidth::I64, 0x6067)
                        .Status,
                    GuestMemoryAccessStatus::Completed);
              else
                Debug.DR7 = 0x400;
              auto Resumed = Context.resume(
                  State, *Context.lastResult().Delivery->Continuation, 0xffff);
              ASSERT_TRUE(bool(Resumed)) << llvm::toString(Resumed.takeError());
              EXPECT_EQ(Resumed->Exit, 7u);
              EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, nullptr, nullptr,
                                                     nullptr, nullptr),
                        0u);
            }
            // Execute the selected handler independently of the consumed
            // activation. Recursive entry may return to another handler;
            // neither outcome can replace the already captured scalar result.
            Budget = Polls = 9;
            ASSERT_EQ(A.RunRegion(&State, Opaque, &Budget, &Shadow, &Polls),
                      uint64_t(X64ContinuationExitV1::PrivilegeReturn))
                << Context.lastError();
            EXPECT_NE(Core.GPR[0], OriginalValue);
            const auto Left = Budget;
            const auto Cached =
                A.ResumeCall(&State, Opaque, &Budget, &Shadow, &Polls, &C);
            EXPECT_EQ(Cached.Status, Expected);
            EXPECT_EQ(Cached.Completed, 1u);
            EXPECT_EQ(Cached.Value, ExpectedValue);
            EXPECT_EQ(Budget, Left);
          }
        }
}

TEST_F(X64PagedDebugFunctionTest, CallStartsOnceBeforeItsDebugHandler) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true})
        for (bool Pauses : {false, true}) {
          API A;
          ASSERT_NO_FATAL_FAILURE(
              compileFunctions(LLVM, Scheduled, Pauses, true, A, Compiler));
          resetFunction(true);
          auto &Core = State.Core.Core.Core.Core;
          const auto EntrySP = Core.GPR[4], OriginalValue = Core.GPR[0];
          X64PagedDebugState Debug;
          Debug.DR7 = 0xb0401;
          Debug.DR[0] = EntrySP - 8;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, watchOptions(Debug));
          auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
          uint64_t Budget = 9, Polls = 9;
          Shadows Shadow;
          Site S;
          auto R = A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
          ASSERT_EQ(R.Status, 7u) << Context.lastError();
          EXPECT_EQ(R.Completed, 0u);
          EXPECT_EQ(S.Callee.Activation.Phase, 1u);
          EXPECT_EQ(S.Callee.Activation.ReturnSlot, EntrySP - 8);
          EXPECT_EQ(Budget, 8u);
          EXPECT_NE(Core.GPR[4], S.Callee.Activation.ReturnSlot);
          // The active callee can continue its handler; the original CALL is
          // never executed again, even though the live PC/SP name that handler.
          R = A.ResumeSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
          ASSERT_EQ(R.Status, uint64_t(X64ContinuationExitV1::PrivilegeReturn))
              << Context.lastError();
          EXPECT_EQ(R.Completed, 0u);
          EXPECT_EQ(Core.PC, 0x1010u);
          EXPECT_EQ(Core.GPR[4], EntrySP - 8);
          EXPECT_EQ(Core.GPR[0], OriginalValue + 1);
          R = A.ResumeSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
          ASSERT_EQ(R.Status, 7u) << Context.lastError();
          EXPECT_EQ(R.Completed, 1u);
          EXPECT_EQ(R.Value, OriginalValue + 1);
          EXPECT_EQ(S.Callee.Activation.Phase, 2u);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(EntrySP - 8, GuestScalarWidth::I64)
                        .Value,
                    0x1005u);
          Budget = Polls = 9;
          ASSERT_EQ(A.RunRegion(&State, Opaque, &Budget, &Shadow, &Polls),
                    uint64_t(X64ContinuationExitV1::PrivilegeReturn))
              << Context.lastError();
          EXPECT_EQ(Core.PC, 0x1005u);
          EXPECT_EQ(Core.GPR[0], OriginalValue + 2);
          const auto Left = Budget;
          R = A.ResumeSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
          EXPECT_EQ(R.Status, 7u);
          EXPECT_EQ(R.Completed, 1u);
          EXPECT_EQ(R.Value, OriginalValue + 1);
          EXPECT_EQ(Budget, Left);
        }
}

TEST_F(X64PagedDebugFunctionTest,
       SchedulerKeepsSSShadowAndEntryErrorsClearReceipts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true}) {
      API A;
      ASSERT_NO_FATAL_FAILURE(
          compileFunctions(LLVM, true, false, false, A, Compiler));
      for (bool Blocked : {false, true}) {
        resetFunction(false);
        auto &Core = State.Core.Core.Core.Core;
        Core.PC = 0x1010;
        Core.RFlags = 0x202;
        put(Core.GPR[4], 0x1100);
        X64PagedDebugState Debug;
        Debug.DR7 = 0x401;
        Debug.DR[0] = 0x1010;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(*Memory, watchOptions(Debug));
        auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
        uint64_t Budget = 1, Polls = 1;
        Shadows Shadow;
        Shadow.SSBlocked = Blocked;
        Function F;
        ASSERT_EQ(A.Begin(&State, &F), 1);
        ASSERT_EQ(A.RunFunction(&State, Opaque, &Budget, &Shadow, &Polls, &F),
                  Blocked ? 3u : 7u)
            << Context.lastError();
        EXPECT_EQ(F.Phase, Blocked ? 2u : 1u);
        EXPECT_EQ(Shadow.SSBlocked, 0u);
        EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, nullptr, nullptr,
                                               nullptr, nullptr),
                  Blocked ? 3u : 0u);
        EXPECT_EQ(neverd_x64_paged_debug_step_v1(
                      Opaque, reinterpret_cast<uint8_t *>(&State), 0, nullptr,
                      0, nullptr, nullptr, 0xffff, 48, 2),
                  5u);
        EXPECT_EQ(neverd_x64_paged_progress_v1(Opaque), 0u);
        uint64_t Unchanged = 0x55;
        EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, &Unchanged, nullptr,
                                               nullptr, nullptr),
                  0u);
        EXPECT_EQ(Unchanged, 0x55u);
      }
    }
}

TEST_F(X64PagedDebugFunctionTest, NestedReturnDoesNotConsumeTheRootCell) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true}) {
        API A;
        ASSERT_NO_FATAL_FAILURE(
            compileFunctions(LLVM, Scheduled, false, false, A, Compiler));
        resetFunction(false);
        auto &Core = State.Core.Core.Core.Core;
        Core.RFlags = 0x202;
        const auto EntrySP = Core.GPR[4];
        put(EntrySP, 0x1100);
        X64PagedDebugState Debug;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(*Memory, watchOptions(Debug));
        auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
        uint64_t Budget = 2, Polls = 3;
        Shadows Shadow;
        Function F;
        ASSERT_EQ(A.Begin(&State, &F), 1);
        ASSERT_EQ(A.RunFunction(&State, Opaque, &Budget, &Shadow, &Polls, &F),
                  9u)
            << Context.lastError();
        EXPECT_EQ(F.Phase, 1u);
        EXPECT_EQ(Core.PC, 0x1005u);
        EXPECT_EQ(Core.GPR[4], EntrySP);
        Budget = 1;
        ASSERT_EQ(A.RunFunction(&State, Opaque, &Budget, &Shadow, &Polls, &F),
                  3u)
            << Context.lastError();
        EXPECT_EQ(F.Phase, 2u);
        EXPECT_EQ(F.Status, 3u);
        EXPECT_EQ(Core.PC, 0x1100u);
        EXPECT_EQ(Core.GPR[4], EntrySP + 8);
        EXPECT_EQ(Budget, 0u);
      }
}

TEST_F(X64PagedDebugFunctionTest,
       FaultingCallRemainsPendingUntilActualCompletion) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true}) {
        API A;
        ASSERT_NO_FATAL_FAILURE(
            compileFunctions(LLVM, Scheduled, true, true, A, Compiler));
        resetFunction(true);
        auto &Core = State.Core.Core.Core.Core;
        const auto EntrySP = Core.GPR[4];
        const auto PTE = 0x13000 + ((EntrySP - 8) >> 12) * 8;
        put(PTE, 0);
        put(0x60e0, 0x40e0 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                        (uint64_t(0x8e) << 40));
        X64PagedDebugState Debug;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(*Memory, watchOptions(Debug));
        auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
        uint64_t Budget = 3, Polls = 3;
        Shadows Shadow;
        Site S;
        const auto Failed =
            A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
        ASSERT_EQ(Failed.Status, 7u) << Context.lastError();
        EXPECT_EQ(Failed.Completed, 0u);
        EXPECT_EQ(S.Callee.Activation.Phase, 0u);
        EXPECT_EQ(S.Callee.Activation.ReturnSlot, EntrySP);
        EXPECT_EQ(Budget, 2u);
        EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, nullptr, nullptr,
                                               nullptr, nullptr),
                  0u);
        ASSERT_EQ(Memory->physicalMemory()
                      .storeScalar(PTE, GuestScalarWidth::I64,
                                   ((EntrySP - 8) & ~uint64_t(0xfff)) | 0x67)
                      .Status,
                  GuestMemoryAccessStatus::Completed);
        // Exercise the existing producer-owned restore interface; this case
        // does not claim that an original page-fault handler performed the
        // repair.
        Core.PC = 0x1000;
        Core.GPR[4] = EntrySP;
        Core.RFlags = 0x302;
        const auto Completed =
            A.ResumeSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
        ASSERT_EQ(Completed.Status, 7u) << Context.lastError();
        EXPECT_EQ(Completed.Completed, 0u);
        EXPECT_EQ(S.Callee.Activation.Phase, 1u);
        EXPECT_EQ(S.Callee.Activation.ReturnSlot, EntrySP - 8);
        EXPECT_EQ(Budget, 1u);
        EXPECT_EQ(Memory->physicalMemory()
                      .loadScalar(EntrySP - 8, GuestScalarWidth::I64)
                      .Value,
                  0x1005u);
      }
}

TEST_F(X64PagedDebugFunctionTest,
       SchedulerRefusalCannotReuseAnEarlierTransfer) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true}) {
      API A;
      ASSERT_NO_FATAL_FAILURE(
          compileFunctions(LLVM, true, false, false, A, Compiler));
      for (unsigned Case = 0; Case != 3; ++Case) {
        SCOPED_TRACE(testing::Message() << LLVM << " case=" << Case);
        resetFunction(false);
        const auto Before = State;
        X64PagedDebugState Debug;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(*Memory, watchOptions(Debug));
        auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
        uint64_t Budget = 1, Polls = 1;
        Shadows Shadow;
        Site Previous;
        ASSERT_EQ(
            A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &Previous)
                .Status,
            7u);
        ASSERT_EQ(neverd_x64_paged_transfer_v1(Opaque, nullptr, nullptr,
                                               nullptr, nullptr),
                  1u);
        // A producer starts a separate activation at the same original CALL.
        // The context still contains its previous operation's transfer receipt.
        State = Before;
        const auto RAMBefore =
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes;
        Budget = Case == 2 ? 0 : 1;
        Polls = Case == 1 ? 0 : 1;
        Shadow.SSBlocked = Case == 0 ? 2 : 0;
        Site Fresh;
        const auto Rejected =
            A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &Fresh);
        EXPECT_EQ(Rejected.Status,
                  Case == 0 ? uint64_t(X64ContinuationExitV1::InvalidState)
                  : Case == 1
                      ? uint64_t(X64ContinuationExitV1::EventBudgetExhausted)
                      : 9u);
        EXPECT_EQ(Rejected.Completed, 0u);
        EXPECT_EQ(Fresh.Callee.Activation.Phase, 0u);
        EXPECT_EQ(Fresh.Callee.Activation.ReturnSlot,
                  Before.Core.Core.Core.Core.GPR[4]);
        EXPECT_EQ(Budget, Case == 2 ? 0u : 1u);
        EXPECT_EQ(Polls, Case == 1 ? 0u : 1u);
        EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAMBefore);
      }
    }
}

TEST_F(X64PagedDebugFunctionTest, CompletedCallSurvivesEventEntryStops) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true}) {
        API A;
        ASSERT_NO_FATAL_FAILURE(
            compileFunctions(LLVM, Scheduled, true, true, A, Compiler));
        for (unsigned Case = 0; Case != 3; ++Case) {
          SCOPED_TRACE(testing::Message()
                       << LLVM << Scheduled << " case=" << Case);
          resetFunction(true);
          auto &Core = State.Core.Core.Core.Core;
          const auto EntrySP = Core.GPR[4];
          if (Case == 0)
            put(0x13030, 0x50067);
          if (Case == 2)
            State.IDTR.Limit = 0;
          X64PagedDebugState Debug;
          auto Options = watchOptions(Debug);
          if (Case == 1) {
            Debug.DR7 = 0xb0401;
            Debug.DR[0] = 0x6010;
            Options.EventDelivery.MaxAttempts = 1;
          }
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, Options);
          auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
          uint64_t Budget = 9, Polls = 9;
          Shadows Shadow;
          Site S;
          const auto R =
              A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
          ASSERT_EQ(R.Status, Case == 0   ? 18u
                              : Case == 1 ? 23u
                                          : 19u)
              << Context.lastError();
          EXPECT_EQ(R.Completed, 0u);
          EXPECT_EQ(S.Callee.Activation.Phase, 1u);
          EXPECT_EQ(S.Callee.Activation.ReturnSlot, EntrySP - 8);
          EXPECT_EQ(Budget, 8u);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(EntrySP - 8, GuestScalarWidth::I64)
                        .Value,
                    0x1005u);
          if (Case == 2)
            continue;
          const auto Continuation = Context.lastResult().Delivery->Continuation;
          ASSERT_TRUE(Continuation);
          if (Case == 0)
            ASSERT_EQ(Memory->physicalMemory()
                          .storeScalar(0x13030, GuestScalarWidth::I64, 0x6067)
                          .Status,
                      GuestMemoryAccessStatus::Completed);
          else
            Debug.DR7 = 0x400;
          auto Delivered = Context.resume(State, *Continuation, 0xffff);
          ASSERT_TRUE(bool(Delivered)) << llvm::toString(Delivered.takeError());
          EXPECT_EQ(Delivered->Exit, 7u);
          EXPECT_EQ(neverd_x64_paged_transfer_v1(Opaque, nullptr, nullptr,
                                                 nullptr, nullptr),
                    0u);
          // Continue the actual handler inside the retained callee activation.
          // A recursive fixed-IST frame may return to a handler again.
          const auto After =
              A.ResumeSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
          ASSERT_EQ(After.Status,
                    uint64_t(X64ContinuationExitV1::PrivilegeReturn))
              << Context.lastError();
          EXPECT_EQ(After.Completed, 0u);
          EXPECT_EQ(S.Callee.Activation.Phase, 1u);
          EXPECT_EQ(S.Callee.Activation.ReturnSlot, EntrySP - 8);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(EntrySP - 8, GuestScalarWidth::I64)
                        .Value,
                    0x1005u);
          if (Case == 0) {
            EXPECT_EQ(Core.PC, 0x1010u);
            EXPECT_EQ(Core.GPR[4], EntrySP - 8);
            const auto Done =
                A.ResumeSite(&State, Opaque, &Budget, &Shadow, &Polls, &S);
            EXPECT_EQ(Done.Status, 3u) << Context.lastError();
            EXPECT_EQ(Done.Completed, 1u);
            EXPECT_EQ(Core.PC, 0x1005u);
            EXPECT_EQ(Core.GPR[4], EntrySP);
          }
        }
      }
}

TEST_F(X64PagedDebugFunctionTest,
       PendingHandlerRequiresAQuiescentPrivilegeReturn) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (const auto &Compiler : compilers())
    for (bool LLVM : {false, true})
      for (bool Scheduled : {false, true}) {
        API A;
        ASSERT_NO_FATAL_FAILURE(
            compileFunctions(LLVM, Scheduled, true, true, A, Compiler));
        for (bool ReturnDebug : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << Compiler << LLVM << Scheduled << ReturnDebug);
          resetFunction(true);
          auto &Core = State.Core.Core.Core.Core;
          Core.RFlags = 0x202;
          const auto EntrySP = Core.GPR[4], OriginalValue = Core.GPR[0];
          X64PagedDebugState Debug;
          Debug.DR7 = 0x401;
          // The selected handler must set RF in the saved flags to permit
          // retry; instruction-breakpoint entry does not set it for us.
          Debug.DR[0] = 0x1000;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory, watchOptions(Debug));
          auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
          uint64_t Budget = 1, Polls = 1;
          Shadows Shadow;
          Site S;
          ASSERT_EQ(
              A.StartSite(&State, Opaque, &Budget, &Shadow, &Polls, &S).Status,
              7u)
              << Context.lastError();
          EXPECT_EQ(S.Callee.Activation.Phase, 0u);
          HandlerContext H;
          ASSERT_EQ(A.BeginHandler(&S, &H), 1);
          EXPECT_EQ(H.ReturnSlot, EntrySP);
          if (ReturnDebug) {
            Debug.DR7 = 0xb0401;
            Debug.DR[0] = Core.GPR[4]; // IRET's frame read causes another #DB.
            // The new #DB must use a different stack: an R/W comparator
            // also observes event-frame writes and would recursively trap
            // if the fixed IST overwrote this same watched frame.
            ASSERT_EQ(Memory->physicalMemory()
                          .storeScalar(0x8024, GuestScalarWidth::I64, 0x1c000)
                          .Status,
                      GuestMemoryAccessStatus::Completed);
          }
          Budget = Polls = 4;
          auto R =
              A.ResumeHandler(&State, Opaque, &Budget, &Shadow, &Polls, &S, &H);
          if (ReturnDebug) {
            ASSERT_EQ(R.Status, 7u) << Context.lastError();
            EXPECT_EQ(H.Ready, 0u);
            EXPECT_EQ(S.Callee.Activation.Phase, 0u);
            EXPECT_EQ(Core.PC, 0x4010u);
            Debug.DR7 = 0x400;
            Budget = Polls = 4;
            R = A.ResumeHandler(&State, Opaque, &Budget, &Shadow, &Polls, &S,
                                &H);
          }
          ASSERT_EQ(R.Status, 9u) << Context.lastError();
          EXPECT_EQ(R.Completed, 0u);
          EXPECT_EQ(H.Ready, 1u);
          EXPECT_EQ(S.Callee.Activation.Phase, 0u);
          EXPECT_EQ(Core.PC, 0x1000u);
          EXPECT_EQ(Core.GPR[4], EntrySP);
          EXPECT_EQ(Budget, 0u);
          Budget = Polls = 1;
          R = A.ResumeHandler(&State, Opaque, &Budget, &Shadow, &Polls, &S, &H);
          ASSERT_EQ(R.Status, 9u) << Context.lastError();
          EXPECT_EQ(S.Callee.Activation.Phase, 1u);
          EXPECT_EQ(S.Callee.Activation.ReturnSlot, EntrySP - 8);
          EXPECT_EQ(Core.PC, 0x1010u);
          EXPECT_EQ(Core.GPR[4], EntrySP - 8);
          Budget = Polls = 1;
          R = A.ResumeHandler(&State, Opaque, &Budget, &Shadow, &Polls, &S, &H);
          ASSERT_EQ(R.Status, 3u) << Context.lastError();
          EXPECT_EQ(R.Completed, 1u);
          EXPECT_EQ(R.Value, OriginalValue + (ReturnDebug ? 2 : 1));
          EXPECT_EQ(Core.PC, 0x1005u);
          EXPECT_EQ(Core.GPR[4], EntrySP);
        }
      }
}
