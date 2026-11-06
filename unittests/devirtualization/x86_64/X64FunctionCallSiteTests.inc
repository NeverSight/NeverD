//===- X64FunctionCallSiteTests.inc - Native CALL bridge proofs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Static request, object-compilation and call-site controller proofs.
/// No specimen, generated program or instruction emulator is executed.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, FunctionCallSitesRejectUnboundInstructions) {
  // Direct CALL, direct JMP, register CALL, RET. Only the first can bind 1020.
  std::vector<uint8_t> Code(33, 0x90);
  const uint8_t Calls[] = {0xe8, 0x1b, 0, 0,    0,    0xe9, 0x16,
                           0,    0,    0, 0xff, 0xd0, 0xc3};
  std::copy(std::begin(Calls), std::end(Calls), Code.begin());
  Code[32] = 0xc3;
  load(Code, {{0x1000, 13}, {0x100a, 3}, {0x1020, 1}});
  const std::string Prefix =
      R"("entries":[4096,4106,4128],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":)";
  for (const char *Bad : {"null", "false", "{}", "[]", "[null]", "[true]",
                          "[-1]", "[1.5]", "[{}]", "[4096,4096]", "[4097]",
                          "[4101]", "[4106]", "[4108]", "[4128]", "[8192]"}) {
    SCOPED_TRACE(Bad);
    const auto Text = request(Prefix + Bad + "}]");
    for (bool LLVM : {false, true}) {
      const char *Report = "old";
      EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Text.data(),
                                                  Text.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      EXPECT_FALSE(take(neverd_last_error(Session)).empty());
    }
  }
  const auto WrongTarget = request(
      R"("entries":[4096,4128],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096]}])");
  const auto Duplicate = request(
      R"("entries":[4096,4128],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096]},{"entry":4128,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096]}])");
  const auto Unselected = request(
      R"("entries":[4128],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096]}])");
  const auto OverBudget = request(
      R"("entries":[4096,4128],"max_instructions":2,"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096,4101,4106]}])");
  for (const auto &Text : {WrongTarget, Duplicate, Unselected, OverBudget}) {
    const char *Report = "old";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Text.data(),
                                                Text.size(), 0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_FALSE(take(neverd_last_error(Session)).empty());
  }
}

TEST_F(X64ContinuationExportTest, FunctionCallSitesCompileAndProveBoundaries) {
  std::vector<uint8_t> Code(41, 0x90);
  const uint8_t First[] = {0xe8, 0x1b, 0, 0, 0, 0xc3};
  const uint8_t Second[] = {0xe8, 0x0b, 0, 0, 0, 0xc3};
  // The undeclared fifth slot's high DWORD and RDX are observable; RET 16
  // must retain its actual cleanup, not assume SP returns to the call point.
  const uint8_t Body[] = {0x8b, 0x44, 0x24, 0x2c, 0x31, 0xd0, 0xc2, 0x10, 0};
  std::copy(std::begin(First), std::end(First), Code.begin());
  std::copy(std::begin(Second), std::end(Second), Code.begin() + 16);
  std::copy(std::begin(Body), std::end(Body), Code.begin() + 32);
  load(Code, {{0x1000, 6}, {0x1010, 6}, {0x1020, 9}});
  const std::string Selection =
      R"("entries":[4096,4112,4128],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[1,null,4,8,null,2,null],"result_bytes":4)";
  // Controller-only proofs use independent RAM, near-CALL equations and
  // activation outcomes. The complete original instruction bodies are compiled
  // separately below. This oracle does not qualify arbitrary native providers.
  const char Oracle[] = R"C(
typedef struct { uint64_t GPR[16], PC; } neverd_x64_state_v6;
)C";
  const char Driver[] = R"C(
typedef int (*neverd_x64_environment_v6)(neverd_x64_state_v6 *,uint64_t,void *);
/* These controller-only predicates do not invoke selected-handler resumes. */
uint64_t neverd_x64_continue_v6(neverd_x64_state_v6 *,uint64_t *);
static uint64_t ram[24];
static unsigned copies,steps,runs,mode,invalid;
static uintptr_t copied_address;
static size_t copied_bytes;
static void model_copy(void *dest,uintptr_t address,size_t bytes) {
    if(address>sizeof(ram) || bytes>sizeof(ram)-address) __builtin_trap();
    ++copies;copied_address=address;copied_bytes=bytes;
    __builtin_memcpy(dest,(uint8_t *)ram+address,bytes);
}
#define __builtin_memcpy(D,S,N) model_copy(D,(uintptr_t)(S),N)
__attribute__((always_inline))
static uint64_t model_call(uint8_t *bytes,uint64_t next) {
    neverd_x64_state_v6 *s=(void *)bytes;
    if(invalid)return 5;
    ++steps;s->GPR[4]-=8;ram[s->GPR[4]/8]=next;s->PC=0x1020;return 1;
}
__attribute__((always_inline))
uint64_t neverd_step_v6_1000(uint8_t *s){return model_call(s,0x1005);}
__attribute__((always_inline))
uint64_t neverd_step_v6_1010(uint8_t *s){return model_call(s,0x1015);}
__attribute__((always_inline))
uint64_t neverd_x64_run_function_v6(neverd_x64_state_v6 *s,uint64_t *b,
    uint64_t *e,neverd_x64_function_context_v1 *f,
    neverd_x64_environment_v6 provider,void *opaque) {
    if(f->Phase!=NEVERD_X64_FUNCTION_ACTIVE)
        return f->Phase==NEVERD_X64_FUNCTION_RETURNED?3:5;
    if(!*b)return 9;
    ++runs;
    if(mode==1)return 3; /* External return, not declared-cell completion. */
    if(mode==2){s->PC=0x8000;return 4;}
    s->GPR[0]=(uint32_t)(ram[12]>>32)^(uint32_t)s->GPR[2];
    s->PC=ram[s->GPR[4]/8];s->GPR[4]+=24;
    --*b;f->Phase=NEVERD_X64_FUNCTION_RETURNED;return 3;
}
)C";
  const char Probes[] = R"C(
#define PREPARE() \
    uint64_t budget=2,env_budget=3; \
    for(unsigned i=0;i<24;++i)ram[i]=0; \
    neverd_x64_state_v6 state={0};neverd_x64_call_site_context_v1 site={0}; \
    state.PC=0x1000;state.GPR[4]=64; \
    state.GPR[1]=0x1122334455667788ULL;state.GPR[2]=0x9988776655443322ULL; \
    state.GPR[8]=0xabcdef0187654321ULL;state.GPR[9]=UINT64_MAX; \
    ram[12]=0xfedcba9876543210ULL;ram[13]=0x1122334455667788ULL; \
    ram[7]=0xdeadbeef;ram[6]=0xcafebabe; \
    copies=steps=runs=mode=invalid=0;copied_address=0;copied_bytes=0
#define CALL() neverd_x64_call_site_1000_v6(&state,&budget,&env_budget,&site, \
    0,0,0x88,0x87654321,UINT64_MAX,0x7788)
#define RESUME() neverd_x64_resume_site_1000_v6(&state,&budget,&env_budget,&site,0,0)
int proof_call_cell_and_holes(uint64_t incoming,uint64_t reg) {
    PREPARE();ram[12]=incoming;state.GPR[2]=reg;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=3 || r.Completed!=1 ||
        r.Value!=((uint32_t)(incoming>>32)^(uint32_t)reg) ||
        site.Callee.Value!=r.Value || site.Callee.Entry!=0x1020 ||
        site.Callee.Function.ReturnSlot!=56 ||
        state.GPR[4]!=80 || state.PC!=0x1005 || ram[7]!=0x1005 ||
        ram[12]!=incoming || ram[6]!=0xcafebabe ||
        ram[13]!=0x1122334455667788ULL ||
        copies!=1 || copied_address!=104 || copied_bytes!=2 ||
        budget || env_budget!=3 || steps!=1 || runs!=1;
}
int proof_argument_rejection_before_call(void) {
    PREPARE();ram[13]^=1;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || r.Value || site.Prepared ||
        site.Callee.Entry || site.Callee.Function.Phase || site.Site ||
        budget!=2 || env_budget!=3 || steps || runs || ram[7]!=0xdeadbeef ||
        state.GPR[4]!=64 || state.PC!=0x1000;
}
int proof_register_rejection_before_stack_read(void) {
    PREPARE();state.GPR[1]^=1;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || site.Prepared || steps || copies ||
        state.GPR[4]!=64 || ram[7]!=0xdeadbeef || budget!=2;
}
int proof_pre_call_budget_pause(uint64_t incoming) {
    PREPARE();budget=0;neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=9 || r.Completed || steps || runs || budget ||
        site.Prepared!=1 || site.Site!=0x1000 || site.Callee.Entry!=0x1020 ||
        site.Callee.Function.Phase || site.Callee.Function.ReturnSlot!=64 ||
        ram[7]!=0xdeadbeef || state.GPR[4]!=64 || state.PC!=0x1000)return 1;
    budget=2;ram[12]=incoming;r=RESUME();
    return r.Status!=3 || r.Completed!=1 ||
        r.Value!=((uint32_t)(incoming>>32)^0x55443322) ||
        steps!=1 || runs!=1 || copies!=1 || budget;
}
int proof_post_call_budget_pause(uint64_t incoming) {
    PREPARE();budget=1;neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=9 || r.Completed || steps!=1 || runs || budget ||
        site.Callee.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        site.Callee.Function.ReturnSlot!=56 || state.GPR[4]!=56 ||
        state.PC!=0x1020 || ram[7]!=0x1005)return 1;
    budget=1;ram[12]=incoming;r=RESUME();
    return r.Status!=3 || r.Completed!=1 ||
        r.Value!=((uint32_t)(incoming>>32)^0x55443322) ||
        steps!=1 || runs!=1 || copies!=1 || budget || state.GPR[4]!=80;
}
int proof_rejected_instruction_retry(void) {
    PREPARE();invalid=1;neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=5 || r.Completed || steps || runs || budget!=2 ||
        site.Callee.Function.Phase || ram[7]!=0xdeadbeef)return 1;
    invalid=0;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || steps!=1 || runs!=1 || copies!=1;
}
int proof_pending_pc_guard(void) {
    PREPARE();budget=0;CALL();state.PC=0x1010;budget=2;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || steps || runs || budget!=2 ||
        state.PC!=0x1010 || state.GPR[4]!=64 || ram[7]!=0xdeadbeef;
}
int proof_pending_sp_guard(void) {
    PREPARE();budget=0;CALL();state.GPR[4]=72;budget=2;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || steps || runs || budget!=2 ||
        state.GPR[4]!=72 || ram[7]!=0xdeadbeef;
}
int proof_wrong_site_resume(void) {
    PREPARE();budget=1;CALL();budget=1;
    neverd_x64_call_result_v1 r=neverd_x64_resume_site_1010_v6(
        &state,&budget,&env_budget,&site,0,0);
    return r.Status!=5 || r.Completed || steps!=1 || runs || budget!=1;
}
int proof_external_return_is_not_completion(void) {
    PREPARE();mode=1;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=3 || r.Completed || r.Value || site.Callee.Value ||
        site.Callee.Function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        budget!=1 || steps!=1 || runs!=1;
}
int proof_resume_does_not_replay_call(void) {
    PREPARE();mode=2;neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=4 || r.Completed || state.PC!=0x8000 || budget!=1)return 1;
    mode=0;state.PC=0x1020;ram[7]=0x7777;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || state.PC!=0x7777 ||
        steps!=1 || runs!=2 || copies!=1 || state.GPR[4]!=80 || budget;
}
int proof_completed_result_is_cached(void) {
    PREPARE();neverd_x64_call_result_v1 first=CALL();
    state.GPR[0]=0;state.GPR[4]=UINT64_MAX;state.PC=0;ram[12]=0;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=3 || r.Completed!=1 || r.Value!=first.Value ||
        copies!=1 || steps!=1 || runs!=1 || budget || env_budget!=3 ||
        state.GPR[4]!=UINT64_MAX || state.PC;
}
int proof_wrapped_stack_guard(void) {
    PREPARE();state.GPR[4]=UINT64_MAX-40;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || site.Prepared || steps || copies;
}
int proof_restart_refuses_pending_context(void) {
    PREPARE();budget=0;CALL();budget=2;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || steps || copies!=1 || budget!=2;
}
int proof_unprepared_resume(void) {
    PREPARE();neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || steps || copies || runs || budget!=2;
}
int proof_second_call_site(void) {
    PREPARE();state.PC=0x1010;
    neverd_x64_call_result_v1 r=neverd_x64_call_site_1010_v6(
        &state,&budget,&env_budget,&site,0,0,0x88,0x87654321,UINT64_MAX,0x7788);
    return r.Status!=3 || r.Completed!=1 || state.PC!=0x1015 ||
        ram[7]!=0x1015 || site.Site!=0x1010 || steps!=1 || copies!=1;
}
)C";
  for (bool LLVM : {false, true})
    for (const std::string Dispatch : {"", ",\"dispatch\":\"direct\""}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Dispatch);
      const auto [Source, Manifest] = source(
          request(Selection + R"(,"call_sites":[4096,"0x1010"]}])" + Dispatch),
          LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Sites = Parsed->getAsObject()
                              ->getObject("function_call_sites")
                              ->getArray("interfaces");
      ASSERT_NE(Sites, nullptr);
      ASSERT_EQ(Sites->size(), 2u);
      EXPECT_EQ((*Sites)[0].getAsObject()->getString("return_pc"), "0x1005");
      EXPECT_EQ((*Sites)[1].getAsObject()->getString("return_pc"), "0x1015");
      const auto *Arguments =
          Sites->front().getAsObject()->getArray("arguments");
      ASSERT_NE(Arguments, nullptr);
      EXPECT_EQ((*Arguments)[4].getAsObject()->getInteger("call_sp_offset"),
                32);
      EXPECT_EQ((*Arguments)[4].getAsObject()->getBoolean("retained_in_state"),
                true);
      EXPECT_FALSE((*Arguments)[4].getAsObject()->get("bytes"));
      EXPECT_EQ((*Arguments)[5].getAsObject()->getInteger("call_sp_offset"),
                40);
      const auto Marker = Source.find("/* Original CALL bridges.");
      ASSERT_NE(Marker, std::string::npos);
      const auto Base = source(request(Selection + "}]" + Dispatch), LLVM);
      EXPECT_EQ(Source.substr(0, Marker), Base.first + "\n");
      const auto File = tmpFile("call-site-bridge.c");
      std::ofstream(File) << Source;
      for (const char *Level : {"-O0", "-O2"}) {
        const auto Built = exec(NEVERD_TEST_CLANG,
                                {"-std=c11", Level, "-Werror=uninitialized",
                                 "-Werror=return-type", "-c", File.string(),
                                 "-o", tmpFile("call-site-bridge.o").string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
      }
      const auto Begin = Source.find("enum neverd_x64_function_phase_v1");
      const auto Function =
          Source.find("int neverd_x64_begin_function_v6", Begin);
      const auto End = Source.find("\n}\n", Function);
      const auto Calls = Source.find("/* Checked scalar call interfaces.");
      ASSERT_NE(Begin, std::string::npos);
      ASSERT_NE(End, std::string::npos);
      ASSERT_NE(Calls, std::string::npos);
      std::string Wrappers = Source.substr(Calls);
      const std::string Definition = "\nneverd_x64_call_result_v1 neverd_x64_";
      const std::string Inline = "__attribute__((always_inline)) ";
      for (size_t At = 0;
           (At = Wrappers.find(Definition, At)) != std::string::npos;
           At += Definition.size() + Inline.size())
        Wrappers.insert(At + 1, Inline);
      const auto Proof = tmpFile("call-site-proofs.c");
      std::ofstream(Proof) << "#include <stdint.h>\n#include <stddef.h>\n"
                           << Oracle << Source.substr(Begin, End + 3 - Begin)
                           << Driver << Wrappers << Probes;
      const auto IR = tmpFile("call-site-proofs.ll");
      const auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-S",
                "-emit-llvm", Proof.string(), "-o", IR.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
      ASSERT_NE(Module, nullptr);
      unsigned Count = 0;
      for (const auto &F : *Module)
        if (F.getName().starts_with("proof_")) {
          SCOPED_TRACE(F.getName().str());
          ++Count;
          ASSERT_EQ(F.size(), 1u);
          const auto *Return =
              llvm::dyn_cast<llvm::ReturnInst>(F.front().getTerminator());
          ASSERT_NE(Return, nullptr);
          const auto *Value =
              llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
          ASSERT_NE(Value, nullptr);
          EXPECT_TRUE(Value->isZero());
        }
      EXPECT_EQ(Count, 16u);
    }
}

TEST_F(X64ContinuationExportTest, FunctionCallSitesProveOriginalStackTransfer) {
  load({0xe8, 0x01, 0, 0, 0, 0xc3, 0xc2, 0x10, 0}, {{0x1000, 6}, {0x1006, 3}});
  const auto Request = request(
      R"("entries":[4096,4102],"follow_internal_calls":true,"function_context":true,"function_calls":[{"entry":4102,"abi":"win64","argument_bytes":[],"result_bytes":8,"call_sites":[4096]}])");
  // Fixed accessible guest addresses make original stack arithmetic visible
  // to the compiler without assuming identities for unrelated host pointers.
  // These are compilation-only predicates; the mapped RAM is never accessed.
  const char Probes[] = R"C(
static neverd_x64_state_v6 proof_seed(void) {
    neverd_x64_state_v6 s={0};s.RFlags=2;s.PC=0x1000;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};return s;
}
int proof_original_call(uint64_t word,uint64_t reg) {
    uint64_t *stack=(uint64_t *)(uintptr_t)0x10000;
    neverd_x64_state_v6 state=proof_seed();state.GPR[4]=(uintptr_t)&stack[8];
    state.GPR[2]=reg;state.GPR[0]=word;
    uint64_t status=neverd_step_v6_1000((uint8_t *)&state);
    return status!=1 || state.PC!=0x1006 ||
        state.GPR[4]!=(uintptr_t)&stack[7] || stack[7]!=0x1005 ||
        state.GPR[0]!=word || state.GPR[2]!=reg;
}
int proof_original_ret_cleanup(uint64_t target,uint64_t word) {
    uint64_t *stack=(uint64_t *)(uintptr_t)0x10000;
    neverd_x64_state_v6 state=proof_seed();state.PC=0x1006;
    state.GPR[4]=(uintptr_t)&stack[7];stack[7]=target;state.GPR[0]=word;
    uint64_t status=neverd_step_v6_1006((uint8_t *)&state);
    return status!=3 || state.PC!=target ||
        state.GPR[4]!=(uintptr_t)&stack[10] || state.GPR[0]!=word;
}
int proof_original_bridge(uint64_t result) {
    uint64_t *stack=(uint64_t *)(uintptr_t)0x10000;
    uint64_t budget=2,environment_budget=3;
    neverd_x64_state_v6 state=proof_seed();state.GPR[4]=(uintptr_t)&stack[8];
    state.GPR[0]=result;neverd_x64_call_site_context_v1 site={0};
    neverd_x64_call_result_v1 r=neverd_x64_call_site_1000_v6(
        &state,&budget,&environment_budget,&site,0,0);
    return r.Status!=3 || r.Completed!=1 || r.Value!=result || budget ||
        environment_budget!=3 || state.PC!=0x1005 ||
        state.GPR[4]!=(uintptr_t)&stack[10] || stack[7]!=0x1005 ||
        site.Callee.Function.ReturnSlot!=(uintptr_t)&stack[7];
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Manifest] = source(Request, LLVM);
    const auto File = tmpFile("original-call-transfer.c");
    std::ofstream(File) << Source << Probes;
    const auto IR = tmpFile("original-call-transfer.ll");
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-S",
              "-emit-llvm", File.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const auto Module = llvm::parseIRFile(IR.string(), Diagnostic, Context);
    ASSERT_NE(Module, nullptr);
    unsigned Count = 0;
    for (const auto &F : *Module)
      if (F.getName().starts_with("proof_")) {
        SCOPED_TRACE(F.getName().str());
        ++Count;
        ASSERT_EQ(F.size(), 1u);
        const auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(F.front().getTerminator());
        ASSERT_NE(Return, nullptr);
        const auto *Value =
            llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
        ASSERT_NE(Value, nullptr);
        EXPECT_TRUE(Value->isZero());
      }
    EXPECT_EQ(Count, 3u);
  }
}
