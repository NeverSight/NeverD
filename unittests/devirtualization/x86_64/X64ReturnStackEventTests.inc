//===- X64ReturnStackEventTests.inc - Offline physical entry C ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile both source routes and prove the paged transfer protocol in IR.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, PagedReturnEntryValidatesTheSelection) {
  load({0xc3, 0xc3}, {{0x1000, 1}, {0x1001, 1}});
  for (bool LLVM : {false, true}) {
    const auto Selection = request(
        R"("entries":[4096,4097],"paged_memory":true,"return_stack_events":true)");
    const char *Report = nullptr;
    const auto Both = take(neverd_x64_continuation_source_v1(
        Session, Selection.data(), Selection.size(), LLVM, &Report));
    auto Parsed = llvm::json::parse(take(Report));
    ASSERT_FALSE(Both.empty()) << take(neverd_last_error(Session));
    ASSERT_TRUE(bool(Parsed));
    auto *Contract = Parsed->getAsObject()->getObject("return_stack_events");
    ASSERT_NE(Contract, nullptr);
    ASSERT_EQ(Contract->getArray("entries")->size(), 2u);
    EXPECT_EQ(Contract->getString("resume"),
              "neverd_x64_resume_paged_transfers_v6");
    EXPECT_EQ(Parsed->getAsObject()->getString("call_return_policy"), "stop");
    EXPECT_NE(Both.find("neverd_x64_enter_paged_1001_v6("), std::string::npos);

    const auto One = take(neverd_x64_paged_return_entry_source_v1(
        Session, 0x1001, Selection.data(), Selection.size(), LLVM, &Report));
    Parsed = llvm::json::parse(take(Report));
    ASSERT_FALSE(One.empty()) << take(neverd_last_error(Session));
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(One.find("neverd_x64_enter_paged_1000_v6("), std::string::npos);
    EXPECT_NE(One.find("neverd_x64_enter_paged_1001_v6("), std::string::npos);
    EXPECT_EQ(Parsed->getAsObject()->getArray("units")->size(), 1u);

    for (const char *Conflict :
         {R"("entries":[4097])", R"("instructions":[4096])",
          R"("paged_memory":false)", R"("return_stack_events":false)",
          R"("scheduled_events":true)", R"("function_context":true)",
          R"("native_page_table_access":true)", R"("return_stack_events":1)"}) {
      SCOPED_TRACE(Conflict);
      const auto Bad = request(Conflict);
      Report = "old";
      EXPECT_EQ(neverd_x64_paged_return_entry_source_v1(
                    Session, 0x1000, Bad.data(), Bad.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      EXPECT_FALSE(take(neverd_last_error(Session)).empty());
    }
  }
}

TEST_F(X64ContinuationExportTest, PagedReturnEntryReadsTheCurrentFrame) {
  // The CALL and the RET use different guest stacks. The independent RBP load
  // precedes the RET and can fail before it consumes any return word.
  load({0xe8, 1, 0, 0, 0, 0xcc, 0x48, 0x89, 0xd4, 0x0f, 0xb6, 0x4d, 0xa9, 0xc3},
       {{0x1000, 6}, {0x1006, 8}});
  const auto Options =
      request(R"("follow_internal_calls":true,"callee_depth":1)");
  // Supply only the RAM exchanges of these original instructions. The step
  // bodies calculate their own addresses, widths, values and exits. Separate
  // probes exercise stop propagation independently of the paged runtime.
  const char Probes[] = R"C(
typedef struct {
    uint64_t low, high;
    void *transaction;
    uint64_t frame_byte, target, pushed, steps, accesses, invalid;
    uint64_t injected, attempted, pending;
} transfer_probe;
uint64_t neverd_x64_paged_access_v1(void *opaque, uint64_t address,
                                    uint64_t size, uint64_t flags) {
    transfer_probe *ram = opaque;
    ++ram->accesses;
    if (address == 0x2ff8 && size == 8 && flags == 9)
        ram->pushed = ram->low;
    else if (address == 0x40a9 && size == 1 && (flags & ~8ULL) == 0)
        ram->low = ram->frame_byte;
    else if (address == 0x5000 && size == 8 && flags == 8)
        ram->low = ram->target;
    else
        ++ram->invalid;
    return 0;
}
uint64_t neverd_x64_paged_step_v1(
    uint8_t *opaque, uint8_t *state, uint64_t address, const uint8_t *bytes,
    uint64_t size, neverd_x64_paged_body_v1 body,
    neverd_x64_paged_body_v1 validate, uint64_t mxcsr, uint64_t paging) {
    transfer_probe *ram = (transfer_probe *)opaque;
    ++ram->steps;
    if (ram->injected) return ram->injected;
    return body(state, opaque);
}
uint64_t neverd_x64_paged_pending_v1(uint8_t *opaque) {
    return ((transfer_probe *)opaque)->pending;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *opaque) {
    return ((transfer_probe *)opaque)->attempted;
}
static neverd_x64_state_v6 initial_state(void) {
    neverd_x64_state_v6 state = {0};
    state.PC=0x1000; state.RFlags=0x246; state.MXCSR=0x1f80;
    state.CS.Selector=8; state.CS.Attributes=0xa09b;
    state.SS.Selector=16; state.SS.Attributes=0xc093;
    state.GPR[4]=0x3000; state.GPR[2]=0x5000; state.GPR[5]=0x4100;
    return state;
}
int proof_switched_stack(unsigned char value) {
    neverd_x64_state_v6 state = initial_state();
    uint64_t budget = 8;
    transfer_probe ram = {0};
    ram.frame_byte=value; ram.target=0x1005; ram.attempted=1;
    if (neverd_x64_enter_paged_1000_v6(&state,(uint8_t *)&ram,&budget)!=1 ||
        state.PC!=0x1006 || state.GPR[4]!=0x2ff8 || ram.steps!=1 ||
        ram.accesses!=1 || ram.pushed!=0x1005 || budget!=7) return 1;
    return neverd_x64_resume_paged_transfers_v6(&state,(uint8_t *)&ram,&budget)!=3 ||
        state.PC!=0x1005 || state.GPR[4]!=0x5008 || state.GPR[1]!=value ||
        ram.invalid || ram.accesses!=3 || ram.steps!=4 || budget!=4;
}
int proof_wrong_entry(void) {
    neverd_x64_state_v6 state = initial_state();
    transfer_probe ram = {0};
    uint64_t budget=1; state.PC=0x100d;
    return neverd_x64_enter_paged_1000_v6(&state,(uint8_t *)&ram,&budget)!=5 ||
        ram.steps || ram.accesses || budget!=1 || state.GPR[4]!=0x3000;
}
int proof_no_budget(void) {
    neverd_x64_state_v6 state = initial_state();
    transfer_probe ram = {0}; uint64_t budget=0;
    return neverd_x64_enter_paged_1000_v6(&state,(uint8_t *)&ram,&budget)!=9 ||
        ram.steps || ram.accesses || budget;
}
#define STOP(name, event, charged) \
int proof_##name(void) { \
    neverd_x64_state_v6 state = initial_state(); transfer_probe ram={0}; \
    uint64_t budget=1; ram.injected=event; ram.attempted=charged; \
    return neverd_x64_enter_paged_1000_v6(&state,(uint8_t *)&ram,&budget)!=event || \
        ram.steps!=1 || ram.accesses || budget!=1-charged; \
}
STOP(privilege_return,6,1)
STOP(fault,7,1)
STOP(backing,18,0)
STOP(charged_backing,18,1)
STOP(changed_code,20,0)
STOP(native,4,0)
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto Source = take(neverd_x64_paged_return_entry_source_v1(
        Session, 0x1000, Options.data(), Options.size(), LLVM, nullptr));
    ASSERT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    const auto File = tmpFile("return-stack.c");
    const auto IR = tmpFile("return-stack.ll");
    std::ofstream(File) << Source << Probes;
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-mllvm",
              "-inline-cold-callsite-threshold=10000", "-S", "-emit-llvm",
              File.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
    ASSERT_NE(Module, nullptr);
    unsigned Count = 0;
    for (const auto &F : *Module) {
      if (!F.getName().starts_with("proof_"))
        continue;
      ++Count;
      SCOPED_TRACE(F.getName().str());
      ASSERT_EQ(F.size(), 1u);
      const auto *Ret =
          llvm::dyn_cast<llvm::ReturnInst>(F.front().getTerminator());
      ASSERT_NE(Ret, nullptr);
      const auto *Value =
          llvm::dyn_cast<llvm::ConstantInt>(Ret->getReturnValue());
      ASSERT_NE(Value, nullptr);
      EXPECT_TRUE(Value->isZero());
    }
    EXPECT_EQ(Count, 9u);
  }
}

TEST_F(X64ContinuationExportTest, PagedReturnEntryQualifiesRegisterEpilogue) {
  using namespace neverd::analysis::detail;
  using K = X64PagedRegisterTransfer;
  using R = X64PagedRegisterResult;
  static_assert(x64PagedRegisterResult(K::SysretQ, 0, 3, ~0ULL, 48) ==
                R::UndefinedOpcode);
  static_assert(x64PagedRegisterResult(K::SysretQ, 1, 3, 0, 48) ==
                R::GeneralProtection);
  static_assert(x64PagedRegisterResult(K::SwapGS, 0, 0, 0, 48) == R::Complete);
  static_assert(x64PagedRegisterResult(K::SwapGS, 1, 1, 0, 48) ==
                R::GeneralProtection);
  static_assert(x64PagedRegisterResult(K::SysretQ, 1, 0, (1ULL << 47) - 1,
                                       48) == R::Complete);
  static_assert(x64PagedRegisterResult(K::SysretQ, 1, 0, 1ULL << 47, 48) ==
                R::Native);
  static_assert(x64PagedRegisterResult(K::SysretQ, 1, 0, -(1ULL << 47), 48) ==
                R::Complete);
  static_assert(x64PagedRegisterResult(K::SysretQ, 1, 0, 1ULL << 47, 57) ==
                R::Complete);
  static_assert(x64PagedRegisterResult(K::SysretQ, 1, 0, 1ULL << 56, 57) ==
                R::Native);
  load({0x0f, 0x01, 0xf8, 0x48, 0x0f, 0x07}, {{0x1000, 6}});
  const auto Options = request(R"("entries":[4096],"paged_memory":true)");
  for (bool LLVM : {false, true}) {
    const char *Report = nullptr;
    for (bool Events : {false, true}) {
      auto Source =
          Events ? take(neverd_x64_paged_return_entry_source_v1(
                       Session, 0x1000, Options.data(), Options.size(), LLVM,
                       &Report))
                 : take(neverd_x64_continuation_source_v1(
                       Session, Options.data(), Options.size(), LLVM, &Report));
      auto Parsed = llvm::json::parse(take(Report));
      ASSERT_FALSE(Source.empty()) << take(neverd_last_error(Session));
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 2);
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"),
                Events ? 0 : 2);
      if (!Events)
        continue;
      // Prove the real generated bodies stage the register transition without
      // observing the guest stack, including for an arbitrary stack pointer.
      const auto File = tmpFile("register-return.c");
      const auto IR = tmpFile("register-return.ll");
      std::ofstream(File) << Source << R"C(
int proof_register_transfer(uint64_t rsp, uint64_t gs, uint64_t kernel) {
    neverd_x64_state_v6 s={0};
    s.PC=0x1000; s.RFlags=0x246; s.MXCSR=0x1f80;
    s.CS.Selector=8; s.CS.Attributes=0xa09b;
    s.SS.Selector=16; s.SS.Attributes=0xc093;
    s.GSBase=gs; s.KernelGSBase=kernel; s.GPR[4]=rsp;
    s.STAR=0x20ULL<<48; s.GPR[1]=0x7800; s.GPR[11]=0x246;
    if (neverd_paged_neverd_step_v6_1000((uint8_t *)&s,0)!=24 ||
        s.PC!=0x1003 || s.GSBase!=kernel || s.KernelGSBase!=gs) return 1;
    return neverd_paged_neverd_step_v6_1003((uint8_t *)&s,0)!=24 ||
        s.PC!=0x7800 || s.RFlags!=0x246 || s.GPR[4]!=rsp ||
        s.CS.Selector!=0x33 || s.SS.Selector!=0x2b ||
        s.GSBase!=kernel || s.KernelGSBase!=gs;
}
)C";
      const auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-S",
                "-emit-llvm", File.string(), "-o", IR.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
      ASSERT_NE(Module, nullptr);
      const auto *F = Module->getFunction("proof_register_transfer");
      ASSERT_NE(F, nullptr);
      ASSERT_EQ(F->size(), 1u);
      const auto *Ret =
          llvm::dyn_cast<llvm::ReturnInst>(F->front().getTerminator());
      ASSERT_NE(Ret, nullptr);
      const auto *Value =
          llvm::dyn_cast<llvm::ConstantInt>(Ret->getReturnValue());
      ASSERT_NE(Value, nullptr);
      EXPECT_TRUE(Value->isZero());
    }
  }
}
