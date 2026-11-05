//===- X64PagedFunctionContextTests.inc - Paged activation proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile exported bodies and prove the activation controller at the paged
/// runtime boundary. No generated program or instruction executor is run.
//===----------------------------------------------------------------------===//

class X64PagedFunctionContextTest : public X64ContinuationExportTest {
protected:
  void loadReturns() {
    // Two near returns, a jump to the first, and a far return. Completion must
    // depend on the selected original near RET, not just a runtime status.
    load({0xc3, 0xc3, 0xeb, 0xfc, 0xcb},
         {{0x1000, 1}, {0x1001, 1}, {0x1002, 2}, {0x1004, 1}});
  }

  static std::string pagedRequest(llvm::StringRef Extra) {
    return request(
        (R"("entries":[4096,4097,4098,4100],"follow_internal_calls":true,"paged_memory":true)" +
         Extra)
            .str());
  }

  std::pair<std::string, std::string> pagedSource(bool LLVM,
                                                  llvm::StringRef Extra) {
    const auto Request = pagedRequest(Extra);
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }
};

TEST_F(X64PagedFunctionContextTest, AdmissionAndBothSourceRoutes) {
  loadReturns();
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto Default = pagedSource(LLVM, "");
    const auto Disabled = pagedSource(LLVM, ",\"function_context\":false");
    EXPECT_TRUE(Default.first == Disabled.first);
    EXPECT_EQ(Default.first.find("neverd_x64_function_context_v1"),
              std::string::npos);
    auto DefaultManifest = llvm::json::parse(Default.second);
    auto DisabledManifest = llvm::json::parse(Disabled.second);
    ASSERT_TRUE(bool(DefaultManifest));
    ASSERT_TRUE(bool(DisabledManifest));
    DisabledManifest->getAsObject()->getObject("request")->erase(
        "function_context");
    EXPECT_TRUE(*DefaultManifest == *DisabledManifest);
    const auto [Source, Report] =
        pagedSource(LLVM, ",\"function_context\":true");
    ASSERT_FALSE(Source.empty());
    const auto Boundary = Source.find("/* Caller-owned activation,");
    ASSERT_NE(Boundary, std::string::npos);
    EXPECT_TRUE(Source.substr(0, Boundary) ==
                Default.first.substr(0, Boundary));
    auto Manifest = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Manifest)) << llvm::toString(Manifest.takeError());
    EXPECT_EQ(Manifest->getAsObject()->getString("environment_driver"),
              "neverd_x64_run_paged_v6");
    const auto *Contract =
        Manifest->getAsObject()->getObject("function_context");
    ASSERT_NE(Contract, nullptr);
    EXPECT_EQ(Contract->getString("driver"),
              "neverd_x64_run_paged_function_v6");
    EXPECT_EQ(Contract->getString("environment_driver"),
              "neverd_x64_run_paged_function_v6");
    EXPECT_EQ(Contract->getBoolean("frame_ownership_proven"), false);
    EXPECT_TRUE(Contract->getString("paged_domain").has_value());
    const auto File = tmpFile("paged-function-context.c");
    std::ofstream(File) << Source;
    for (const char *Level : {"-O0", "-O2"}) {
      const auto Built = exec(NEVERD_TEST_CLANG,
                              {"-std=c11", Level, "-Werror=uninitialized",
                               "-Werror=return-type", "-c", File.string(), "-o",
                               tmpFile("paged-function-context.o").string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
    }
    for (
        const char *Extra :
        {R"(,"dispatch":"direct")", R"(,"scheduled_events":true)",
         R"(,"native_rtm_regions":true)", R"(,"pause_call_sites":true)",
         R"(,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8}])"}) {
      const auto Bad = pagedRequest(
          (",\"function_context\":true" + llvm::StringRef(Extra)).str());
      const char *Report = "old";
      EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(),
                                                  Bad.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      EXPECT_FALSE(take(neverd_last_error(Session)).empty());
    }
  }
}

TEST_F(X64PagedFunctionContextTest, ProvesRuntimeBoundaryAccounting) {
  loadReturns();
  // The runtime is independently qualified by X64PagedExecutionTests. These
  // compile-time predicates supply its boundary outcomes, including retained
  // fault-entry state and attempted/non-attempted backing stops. They prove
  // only the composition/controller; they do not emulate page walks or claim
  // an original instruction or a guest handler ran. Production bodies and
  // wrappers are retained unchanged and also compiled separately above.
  const char Probes[] = R"C(
typedef struct {
    uint64_t status[2], progress[2], pc[2], sp[2], calls, last;
} boundary_outcomes;
uint64_t neverd_x64_paged_step_v1(
    uint8_t *context,uint8_t *raw,uint64_t address,const uint8_t *bytes,
    uint64_t size,neverd_x64_paged_body_v1 body,
    neverd_x64_paged_body_v1 validate,uint64_t mxcsr,uint64_t paging) {
    boundary_outcomes *o=(boundary_outcomes *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    uint64_t i=o->calls++;o->last=o->progress[i];
    s->PC=o->pc[i];s->GPR[4]=o->sp[i];return o->status[i];
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((boundary_outcomes *)context)->last;
}
#define STACK 0xffffa00000100000ULL
#define PREPARE() \
    neverd_x64_state_v6 state={0};state.PC=0x1000;state.GPR[4]=STACK; \
    neverd_x64_function_context_v1 function={0};uint64_t budget=4; \
    boundary_outcomes outcomes={{3,3},{1,1},{0x1001,0x9000}, \
                                {STACK+8,STACK+16},0,0}; \
    neverd_x64_begin_function_v6(&state,&function)
#define RUN() neverd_x64_run_paged_function_v6( \
    &state,(uint8_t *)&outcomes,&budget,&function)
int proof_return_to_selected_target(void) {
    PREPARE();budget=1;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        outcomes.calls!=1 || budget || state.PC!=0x1001 ||
        state.GPR[4]!=STACK+8 || function.ReturnSlot!=STACK;
}
int proof_completed_is_idempotent(void) {
    PREPARE();RUN();budget=0;uint64_t before=outcomes.calls;
    return RUN()!=3 || outcomes.calls!=before || budget || state.PC!=0x1001;
}
int proof_parent_survives_nested_return(void) {
    PREPARE();state.PC=0x1001;state.GPR[4]=STACK-8;
    outcomes.pc[0]=0x1000;outcomes.sp[0]=STACK;
    outcomes.pc[1]=0x1002;outcomes.sp[1]=STACK+8;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        outcomes.calls!=2 || budget!=2 || state.PC!=0x1002 ||
        state.GPR[4]!=STACK+8 || function.ReturnSlot!=STACK;
}
int proof_post_sp_is_not_return_ownership(void) {
    PREPARE();state.GPR[4]=STACK-8;
    outcomes.pc[0]=0x9000;outcomes.sp[0]=STACK;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        outcomes.calls!=1 || budget!=3 || state.GPR[4]!=STACK;
}
#define STOP(Name,Status,Progress,Remaining) \
int proof_##Name(void) { \
    PREPARE();outcomes.status[0]=Status;outcomes.progress[0]=Progress; \
    outcomes.sp[0]=STACK-32; \
    return RUN()!=Status || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || \
        outcomes.calls!=1 || budget!=Remaining || function.ReturnSlot!=STACK || \
        state.GPR[4]!=STACK-32 || state.PC!=0x1001; \
}
STOP(fault_delivery,7,1,3)
STOP(attempted_backing_stop,18,1,3)
STOP(shutdown,19,1,3)
STOP(operand_backing_stop,18,0,4)
STOP(changed_code,20,0,4)
STOP(native_refusal,4,0,4)
STOP(invalid_state,5,0,4)
STOP(device_unavailable,22,0,4)
STOP(return_without_attempt,3,0,4)
STOP(event_acceptance_is_not_instruction_progress,3,2,4)
int proof_fault_then_retry(void) {
    PREPARE();outcomes.status[0]=7;outcomes.sp[0]=STACK-32;
    if(RUN()!=7 || budget!=3 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE)
        return 1;
    /* A producer has restored the faulting context before retrying. */
    state.PC=0x1000;state.GPR[4]=STACK;
    return RUN()!=3 || budget!=2 || outcomes.calls!=2 ||
        function.Phase!=NEVERD_X64_FUNCTION_RETURNED || state.PC!=0x9000;
}
int proof_jump_cannot_complete(void) {
    PREPARE();state.PC=0x1002;outcomes.pc[0]=0x9000;
    return RUN()!=3 || budget!=3 || outcomes.calls!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_far_return_cannot_complete(void) {
    PREPARE();state.PC=0x1004;outcomes.pc[0]=0x9000;
    return RUN()!=3 || budget!=3 || outcomes.calls!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_zero_budget(void) {
    PREPARE();budget=0;
    return RUN()!=9 || outcomes.calls || budget || state.PC!=0x1000 ||
        state.GPR[4]!=STACK || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_uninitialized(void) {
    PREPARE();function.Phase=NEVERD_X64_FUNCTION_UNINITIALIZED;
    return RUN()!=5 || outcomes.calls || budget!=4 || state.PC!=0x1000;
}
int proof_raw_adapter_preserves_following(void) {
    PREPARE();
    uint64_t status=neverd_x64_run_paged_v6(&state,(uint8_t *)&outcomes,&budget);
    return status!=3 || outcomes.calls!=2 || budget!=2 || state.PC!=0x9000 ||
        state.GPR[4]!=STACK+16 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Report] =
        pagedSource(LLVM, ",\"function_context\":true");
    ASSERT_FALSE(Source.empty());
    const auto File = tmpFile("paged-function-proof.c");
    const auto IR = tmpFile("paged-function-proof.ll");
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
    for (const auto &Function : *Module)
      if (Function.getName().starts_with("proof_")) {
        SCOPED_TRACE(Function.getName().str());
        ++Count;
        ASSERT_EQ(Function.size(), 1u);
        const auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(Function.front().getTerminator());
        ASSERT_NE(Return, nullptr);
        const auto *Value =
            llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
        ASSERT_NE(Value, nullptr);
        EXPECT_TRUE(Value->isZero());
      }
    EXPECT_EQ(Count, 20u);
  }
}
