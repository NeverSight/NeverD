//===- X64PagedFunctionCallTests.inc - Paged call view proofs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile checked paged call views without executing guest or generated code.
//===----------------------------------------------------------------------===//

class X64PagedFunctionCallTest : public X64ContinuationExportTest {
protected:
  void loadInputs() {
    // The upper DWORD of the fifth physical slot remains an original input.
    // The second root has no scalar view of any of its six physical slots.
    std::vector<uint8_t> Bytes(17, 0x90);
    const uint8_t Read[] = {0x8b, 0x44, 0x24, 0x2c, 0x31, 0xd0, 0xc3};
    std::copy(std::begin(Read), std::end(Read), Bytes.begin());
    Bytes[16] = 0xc3;
    load(Bytes, {{0x1000, 7}, {0x1010, 1}});
  }

  static std::string selection() {
    return R"("entries":[4096,4112],"follow_internal_calls":true,"function_context":true,"paged_memory":true)";
  }

  static std::string calls() {
    return R"(,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[1,null,4,8,null,null],"result_bytes":4},{"entry":4112,"abi":"win64","argument_bytes":[null,null,null,null,null,null],"result_bytes":8}])";
  }

  std::pair<std::string, std::string> exportCalls(bool LLVM, bool Enabled) {
    const auto Request = request(selection() + (Enabled ? calls() : ""));
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }
};

TEST_F(X64PagedFunctionCallTest, RegisterViewsRetainAllStackInputs) {
  loadInputs();
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Baseline, BaselineReport] = exportCalls(LLVM, false);
    const auto [Source, Report] = exportCalls(LLVM, true);
    ASSERT_FALSE(Source.empty());
    EXPECT_TRUE(llvm::StringRef(Source).starts_with(Baseline));
    const auto Wrappers = Source.substr(Baseline.size());
    EXPECT_EQ(Wrappers.find("__builtin_memcpy"), std::string::npos);
    EXPECT_EQ(Wrappers.find("state->GPR[4] >"), std::string::npos);
    EXPECT_EQ(Wrappers.find("environment_budget"), std::string::npos);
    EXPECT_NE(Wrappers.find("neverd_x64_run_paged_function_v6"),
              std::string::npos);
    auto Manifest = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Manifest));
    const auto *Views = Manifest->getAsObject()->getObject("function_calls");
    ASSERT_NE(Views, nullptr);
    EXPECT_EQ(Views->getBoolean("paged_memory"), true);
    EXPECT_EQ(Views->getBoolean("entry_guest_memory_reads"), false);
    const auto *Interfaces = Views->getArray("interfaces");
    ASSERT_NE(Interfaces, nullptr);
    ASSERT_EQ(Interfaces->size(), 2u);
    const auto *First = (*Interfaces)[0].getAsObject();
    ASSERT_NE(First, nullptr);
    EXPECT_EQ(First->getString("call"), "neverd_x64_call_paged_1000_v6");
    EXPECT_EQ(First->getString("resume"), "neverd_x64_resume_paged_1000_v6");
    const auto *Arguments = First->getArray("arguments");
    ASSERT_NE(Arguments, nullptr);
    ASSERT_EQ(Arguments->size(), 6u);
    for (unsigned P : {1u, 4u, 5u}) {
      const auto *Argument = (*Arguments)[P].getAsObject();
      ASSERT_NE(Argument, nullptr);
      EXPECT_EQ(Argument->getBoolean("retained_in_state"), true);
      EXPECT_FALSE(Argument->getInteger("bytes").has_value());
      if (P >= 4)
        EXPECT_EQ(Argument->getInteger("entry_sp_offset"), 40 + (P - 4) * 8);
    }
    const auto File = tmpFile("paged-function-calls.c");
    std::ofstream(File) << Source;
    for (const char *Level : {"-O0", "-O2"}) {
      const auto Built = exec(NEVERD_TEST_CLANG,
                              {"-std=c11", Level, "-Werror=uninitialized",
                               "-Werror=return-type", "-c", File.string(), "-o",
                               tmpFile("paged-function-calls.o").string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
    }
  }
}

TEST_F(X64PagedFunctionCallTest, RejectsExtraStackObservations) {
  loadInputs();
  for (bool LLVM : {false, true})
    for (
        const char *Call :
        {R"({"entry":4096,"abi":"win64","argument_bytes":[8,8,8,8,1],"result_bytes":8})",
         R"({"entry":4096,"abi":"win64","argument_bytes":[null,null,null,null,null,8],"result_bytes":8})"}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Call);
      const auto Request =
          request(selection() + ",\"function_calls\":[" + Call + "]");
      const char *Report = "old";
      EXPECT_EQ(neverd_x64_continuation_source_v1(
                    Session, Request.data(), Request.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      const auto Error = take(neverd_last_error(Session));
      EXPECT_NE(Error.find("paged function"), std::string::npos) << Error;
    }
}

TEST_F(X64PagedFunctionCallTest, ProvesValuesAndPagedBoundaryComposition) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  loadInputs();
  // Normal paths compile the actual MOV/XOR/RET bodies against independent
  // operand equations. Stop paths supply the paged runtime's documented
  // outcomes. These are value and composition proofs, not page-walk, fault
  // delivery, native-CPU or guest-execution tests.
  const char Probes[] = R"C(
#define STACK 0xffffa00000100000ULL
typedef struct {
    uint64_t incoming, return_cell, progress, stop, attempted;
    unsigned steps, reads, bad;
} call_outcomes;
typedef struct { uint64_t value, high; void *transaction; } call_exchange;
uint64_t neverd_x64_paged_access_v1(void *opaque, uint64_t address,
                                  uint64_t size, uint64_t flags) {
    call_exchange *e=opaque;call_outcomes *m=e->transaction;
    ++m->reads;
    if(address==STACK+44 && size==4 && flags==8) {
        e->value=m->incoming>>32;return 0;
    }
    if(address==m->return_cell && size==8 && flags==8) {
        e->value=0x9000;return 0;
    }
    m->bad=1;return 1;
}
uint64_t neverd_x64_paged_step_v1(
    uint8_t *context,uint8_t *raw,uint64_t address,const uint8_t *bytes,
    uint64_t size,neverd_x64_paged_body_v1 body,
    neverd_x64_paged_body_v1 validate,uint64_t mxcsr,uint64_t paging) {
    call_outcomes *m=(call_outcomes *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    ++m->steps;m->progress=m->attempted;
    if(m->stop) { s->PC=0x9000;s->GPR[4]=STACK-32;return m->stop; }
    call_exchange e={0,0,m};
    uint64_t status=body(s,&e);
    m->progress=status==17?0:1;
    return status;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((call_outcomes *)context)->progress;
}
static neverd_x64_state_v6 call_seed(void) {
    neverd_x64_state_v6 s={0};
    s.PC=0x1000;s.GPR[4]=STACK;s.RFlags=0x246;s.MXCSR=0x1f80;
    s.CS.Selector=8;s.CS.Attributes=0xa09b;
    s.SS.Selector=16;s.SS.Attributes=0xc093;
    s.GPR[1]=0x1122334455667788ULL;s.GPR[2]=0xabcdef;
    s.GPR[8]=0xfedcba9876543210ULL;s.GPR[9]=UINT64_MAX;
    return s;
}
#define PREPARE() \
    neverd_x64_state_v6 state=call_seed(); \
    neverd_x64_call_context_v1 call={0};uint64_t budget=4; \
    call_outcomes model={0xfedcba9876543210ULL,STACK,0,0,0,0,0,0}
#define CALL() neverd_x64_call_paged_1000_v6( \
    &state,(uint8_t *)&model,&budget,&call,0x88,0x76543210,UINT64_MAX)
#define RESUME() neverd_x64_resume_paged_1000_v6( \
    &state,(uint8_t *)&model,&budget,&call)
int proof_original_input(uint64_t incoming,uint64_t rdx) {
    PREPARE();model.incoming=incoming;state.GPR[2]=rdx;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=3 || r.Completed!=1 ||
        r.Value!=((uint32_t)(incoming>>32)^(uint32_t)rdx) ||
        call.Value!=r.Value || call.Entry!=0x1000 ||
        call.Function.ReturnSlot!=STACK || model.incoming!=incoming ||
        model.bad || model.steps!=3 || model.reads!=2 || budget!=1 ||
        state.PC!=0x9000 || state.GPR[4]!=STACK+8 || state.GPR[2]!=rdx ||
        state.GPR[1]!=0x1122334455667788ULL ||
        state.GPR[8]!=0xfedcba9876543210ULL || state.GPR[9]!=UINT64_MAX;
}
int proof_all_context_only(uint64_t value) {
    PREPARE();state.PC=0x1010;state.GPR[0]=value;
    neverd_x64_call_result_v1 r=neverd_x64_call_paged_1010_v6(
        &state,(uint8_t *)&model,&budget,&call);
    return r.Status!=3 || r.Completed!=1 || r.Value!=value || model.bad ||
        model.steps!=1 || model.reads!=1 || budget!=3 || state.PC!=0x9000;
}
int proof_zero_budget_no_guest_read(void) {
    PREPARE();budget=0;state.GPR[4]=UINT64_MAX;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=9 || r.Completed || r.Value || model.reads || model.steps ||
        budget || call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        call.Function.ReturnSlot!=UINT64_MAX || state.GPR[4]!=UINT64_MAX;
}
int proof_register_mismatch(void) {
    PREPARE();state.GPR[1]^=1;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || r.Value || model.reads || model.steps ||
        budget!=4 || call.Function.Phase || call.Entry || call.Value;
}
int proof_wrong_entry(void) {
    PREPARE();state.PC=0x1010;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || r.Value || model.reads || model.steps ||
        budget!=4 || call.Function.Phase || call.Entry || call.Value;
}
int proof_repeated_start(void) {
    PREPARE();budget=0;CALL();budget=4;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || r.Value || model.reads || model.steps ||
        budget!=4 || call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_resume_without_rechecking(void) {
    PREPARE();budget=1;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=9 || r.Completed || r.Value || state.PC!=0x1004 ||
       model.steps!=1 || model.reads!=1 || budget)return 1;
    state.GPR[1]^=1;budget=2;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || r.Value!=(0xfedcba98U^0xabcdefU) ||
        budget || model.steps!=3 || model.reads!=2 || model.bad;
}
int proof_cached_result(uint64_t incoming) {
    PREPARE();model.incoming=incoming;
    neverd_x64_call_result_v1 a=CALL();state.GPR[0]=0;
    neverd_x64_call_result_v1 b=neverd_x64_resume_paged_1000_v6(0,0,0,&call);
    return a.Status!=3 || a.Completed!=1 || b.Status!=3 || b.Completed!=1 ||
        b.Value!=a.Value || b.Value!=((uint32_t)(incoming>>32)^0xabcdefU) ||
        model.steps!=3 || model.reads!=2 || model.bad || budget!=1;
}
int proof_wrong_root_resume(void) {
    PREPARE();budget=0;CALL();budget=4;
    neverd_x64_call_result_v1 r=neverd_x64_resume_paged_1010_v6(
        &state,(uint8_t *)&model,&budget,&call);
    return r.Status!=5 || r.Completed || r.Value || budget!=4 ||
        model.steps || model.reads;
}
int proof_invalid_activation(void) {
    PREPARE();call.Entry=0x1000;call.Function.Phase=77;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || r.Value || budget!=4 ||
        model.steps || model.reads || call.Function.Phase!=77;
}
#define STOP(Name,ExpectedStatus,Attempted,Remaining) \
int proof_##Name(void) { \
    PREPARE();model.stop=ExpectedStatus;model.attempted=Attempted; \
    neverd_x64_call_result_v1 r=CALL(); \
    return r.Status!=ExpectedStatus || r.Completed || r.Value || budget!=Remaining || \
        model.steps!=1 || model.reads || model.bad || \
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || \
        call.Function.ReturnSlot!=STACK || state.GPR[4]!=STACK-32; \
}
STOP(fault,7,1,3)
STOP(backing_before_attempt,18,0,4)
STOP(backing_after_attempt,18,1,3)
STOP(changed_code,20,0,4)
STOP(shutdown,19,1,3)
STOP(native_refusal,4,0,4)
STOP(invalid_state,5,0,4)
STOP(external_return_is_not_completion,3,1,3)
int proof_return_without_attempt(void) {
    PREPARE();state.PC=0x1010;model.stop=3;
    neverd_x64_call_result_v1 r=neverd_x64_call_paged_1010_v6(
        &state,(uint8_t *)&model,&budget,&call);
    return r.Status!=3 || r.Completed || r.Value || budget!=4 ||
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || model.reads;
}
int proof_fault_then_resume(void) {
    PREPARE();model.stop=7;model.attempted=1;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=7 || r.Completed || budget!=3 || model.steps!=1)return 1;
    /* The producer restores the interrupted instruction before retrying. */
    state.PC=0x1000;state.GPR[4]=STACK;state.GPR[1]^=1;model.stop=0;
    r=RESUME();
    return r.Status!=3 || r.Completed!=1 || r.Value!=(0xfedcba98U^0xabcdefU) ||
        budget || model.steps!=4 || model.reads!=2 || model.bad ||
        call.Function.ReturnSlot!=STACK;
}
int proof_other_return_cell(void) {
    PREPARE();budget=0;CALL();budget=1;
    state.PC=0x1010;state.GPR[4]=model.return_cell=STACK+128;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=3 || r.Completed || r.Value || budget || model.steps!=1 ||
        model.reads!=1 || model.bad || state.GPR[4]!=STACK+136 ||
        call.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        call.Function.ReturnSlot!=STACK;
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Report] = exportCalls(LLVM, true);
    ASSERT_FALSE(Source.empty());
    const auto File = tmpFile("paged-function-calls-proof.c");
    const auto IR = tmpFile("paged-function-calls-proof.ll");
    std::ofstream(File) << Source << Probes;
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-mllvm",
              "-inline-cold-callsite-threshold=10000", "-S", "-emit-llvm",
              File.string(), "-o", IR.string()});
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      const fs::path Directory(Root);
      fs::create_directories(Directory);
      const auto Name = LLVM ? "paged-calls-llvm" : "paged-calls-high";
      fs::copy_file(File, Directory / (std::string(Name) + ".c"),
                    fs::copy_options::overwrite_existing);
      if (fs::exists(IR))
        fs::copy_file(IR, Directory / (std::string(Name) + ".ll"),
                      fs::copy_options::overwrite_existing);
    }
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
    EXPECT_EQ(Count, 21u);
  }
}
