//===- X64PagedFunctionCallSiteTests.inc - Faultable CALL bridges --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile original CALL bridges and prove their paged boundary composition.
/// No generated program, instruction executor or guest is run by these tests.
//===----------------------------------------------------------------------===//

class X64PagedFunctionCallSiteTest : public X64ContinuationExportTest {
protected:
  void loadCalls() {
    std::vector<uint8_t> Code(41, 0x90);
    // The callee reads the retained high DWORD of its fifth physical slot.
    // RET 16 and the caller's SUB restore the parent's original return cell.
    const uint8_t Caller[] = {0xb9, 0x44, 0,    0,    0,    0xe8, 0x16, 0,
                              0,    0,    0x48, 0x83, 0xec, 0x10, 0xc3};
    const uint8_t Other[] = {0xe8, 0x0b, 0, 0, 0, 0xc3};
    const uint8_t Callee[] = {0x8b, 0x44, 0x24, 0x2c, 0x31,
                              0xd0, 0xc2, 0x10, 0};
    std::copy(std::begin(Caller), std::end(Caller), Code.begin());
    std::copy(std::begin(Other), std::end(Other), Code.begin() + 16);
    std::copy(std::begin(Callee), std::end(Callee), Code.begin() + 32);
    load(Code, {{0x1000, 15}, {0x1010, 6}, {0x1020, 9}});
  }

  static std::string selection(llvm::StringRef Sites = "") {
    return (R"("entries":[4096,4112,4128],"follow_internal_calls":true,"function_context":true,"paged_memory":true,"function_calls":[{"entry":4128,"abi":"win64","argument_bytes":[1,null,4,8,null,null],"result_bytes":4)" +
            Sites + "}]")
        .str();
  }

  std::pair<std::string, std::string> exportSites(bool LLVM,
                                                  llvm::StringRef Extra = "") {
    const auto Request =
        request(selection(",\"call_sites\":[4101,4112]") + Extra.str());
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }

  void prove(bool LLVM, llvm::StringRef Source, llvm::StringRef Probes,
             unsigned Expected) {
    const auto File = tmpFile("paged-call-sites-proof.c");
    const auto InitialIR = tmpFile("paged-call-sites-initial.ll");
    const auto IR = tmpFile("paged-call-sites-proof.ll");
    std::ofstream(File) << Source.str() << Probes.str();
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-mllvm",
              "-inline-cold-callsite-threshold=10000", "-S", "-emit-llvm",
              File.string(), "-o", InitialIR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    // The composed parent/child path leaves mask-of-select and known-bit
    // identities after inlining. A normal second IR simplification discharges
    // those identities without replacing any instruction or controller.
    const auto Simplified =
        exec(NEVERD_TEST_CLANG, {"-O2", "-S", "-emit-llvm", InitialIR.string(),
                                 "-o", IR.string()});
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      const fs::path Directory(Root);
      fs::create_directories(Directory);
      const auto Name =
          LLVM ? "paged-call-sites-llvm" : "paged-call-sites-high";
      fs::copy_file(File, Directory / (std::string(Name) + ".c"),
                    fs::copy_options::overwrite_existing);
      fs::copy_file(InitialIR, Directory / (std::string(Name) + ".initial.ll"),
                    fs::copy_options::overwrite_existing);
      if (fs::exists(IR))
        fs::copy_file(IR, Directory / (std::string(Name) + ".ll"),
                      fs::copy_options::overwrite_existing);
    }
    ASSERT_TRUE(Simplified.ok()) << Simplified.err;
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
    EXPECT_EQ(Count, Expected);
  }
};

TEST_F(X64PagedFunctionCallSiteTest, BindsOriginalCallsWithoutExtraGuestReads) {
  loadCalls();
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto Request = request(selection());
    const char *BaselineReport = nullptr;
    const auto Baseline = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &BaselineReport));
    take(BaselineReport);
    ASSERT_FALSE(Baseline.empty());
    const auto [Source, Report] = exportSites(LLVM);
    ASSERT_FALSE(Source.empty());
    EXPECT_TRUE(llvm::StringRef(Source).starts_with(Baseline));
    const auto Wrappers = Source.substr(Baseline.size());
    EXPECT_EQ(Wrappers.find("__builtin_memcpy"), std::string::npos);
    EXPECT_EQ(Wrappers.find("state->GPR[4] >"), std::string::npos);
    EXPECT_EQ(Wrappers.find("environment_budget"), std::string::npos);
    const auto Disabled = exportSites(LLVM, ",\"pause_call_sites\":false");
    EXPECT_EQ(Source, Disabled.first);
    auto Manifest = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Manifest));
    const auto *Sites =
        Manifest->getAsObject()->getObject("function_call_sites");
    ASSERT_NE(Sites, nullptr);
    EXPECT_EQ(Sites->getBoolean("paged_memory"), true);
    EXPECT_EQ(Sites->getBoolean("entry_guest_memory_reads"), false);
    const auto *Interfaces = Sites->getArray("interfaces");
    ASSERT_NE(Interfaces, nullptr);
    ASSERT_EQ(Interfaces->size(), 2u);
    const auto &First = *(*Interfaces)[0].getAsObject();
    EXPECT_EQ(First.getString("call"), "neverd_x64_call_paged_site_1005_v6");
    EXPECT_EQ(First.getString("resume"),
              "neverd_x64_resume_paged_site_1005_v6");
    const auto *Arguments = First.getArray("arguments");
    ASSERT_NE(Arguments, nullptr);
    ASSERT_EQ(Arguments->size(), 6u);
    for (unsigned P : {4u, 5u}) {
      const auto &Argument = *(*Arguments)[P].getAsObject();
      EXPECT_EQ(Argument.getBoolean("retained_in_state"), true);
      EXPECT_EQ(Argument.getInteger("call_sp_offset"), 32 + (P - 4) * 8);
    }
    const auto Paused = exportSites(LLVM, ",\"pause_call_sites\":true");
    auto PauseManifest = llvm::json::parse(Paused.second);
    ASSERT_TRUE(bool(PauseManifest));
    const auto *Pause =
        PauseManifest->getAsObject()->getObject("call_site_pause");
    ASSERT_NE(Pause, nullptr);
    EXPECT_EQ(Pause->getInteger("status"), 21);
    for (const auto &Text : {Source, Paused.first}) {
      const auto File = tmpFile("paged-call-sites.c");
      std::ofstream(File) << Text;
      for (const char *Level : {"-O0", "-O2"}) {
        const auto Built = exec(NEVERD_TEST_CLANG,
                                {"-std=c11", Level, "-Werror=uninitialized",
                                 "-Werror=return-type", "-c", File.string(),
                                 "-o", tmpFile("paged-call-sites.o").string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
      }
    }
    for (const char *Sites : {"[4096]", "[4101,4101]", "[4097]", "[4128]"}) {
      const auto Bad = request(
          selection((",\"call_sites\":" + llvm::StringRef(Sites)).str()));
      const char *Report = "old";
      EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(),
                                                  Bad.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      EXPECT_FALSE(take(neverd_last_error(Session)).empty());
    }
  }
}

TEST_F(X64PagedFunctionCallSiteTest, ProvesPendingCallAndActivationBoundaries) {
  loadCalls();
  // Normal paths retain the actual CALL/MOV/XOR/RET/SUB and dispatcher source.
  // The operand oracle tracks return-cell writes and arbitrary incoming bits.
  // Stop paths supply independent paged runtime outcomes; they prove controller
  // composition, not page walks, fault delivery or guest handler execution.
  const char Probes[] = R"C(
#define STACK 0xffffa00000100000ULL
typedef struct {
    uint64_t incoming, cell, progress, stop, attempted, stop_pc, stop_sp;
    unsigned steps, reads, writes, bad;
} site_outcomes;
typedef struct { uint64_t value, high; void *transaction; } site_exchange;
uint64_t neverd_x64_paged_access_v1(void *opaque,uint64_t address,
                                  uint64_t size,uint64_t flags) {
    site_exchange *e=opaque;site_outcomes *m=e->transaction;
    if(address==STACK-8 && size==8 && flags==9) {
        ++m->writes;m->cell=e->value;return 0;
    }
    ++m->reads;
    if(address==STACK+36 && size==4 && flags==8) {
        e->value=m->incoming>>32;return 0;
    }
    if(address==STACK-8 && size==8 && flags==8) {
        e->value=m->cell;return 0;
    }
    if(address==STACK && size==8 && flags==8) {
        e->value=0x9000;return 0;
    }
    m->bad=1;return 1;
}
uint64_t neverd_x64_paged_step_v1(
    uint8_t *context,uint8_t *raw,uint64_t address,const uint8_t *bytes,
    uint64_t size,neverd_x64_paged_body_v1 body,
    neverd_x64_paged_body_v1 validate,uint64_t mxcsr,uint64_t paging) {
    site_outcomes *m=(site_outcomes *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    ++m->steps;m->progress=0;
    uint64_t status=validate(s,0);
    if(status)return status;
    if(m->stop) {
        m->progress=m->attempted;s->PC=m->stop_pc;s->GPR[4]=m->stop_sp;
        return m->stop;
    }
    site_exchange e={0,0,m};status=body(s,&e);
    m->progress=status<=3;return status;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((site_outcomes *)context)->progress;
}
static neverd_x64_state_v6 site_seed(void) {
    neverd_x64_state_v6 s={0};s.PC=0x1005;s.GPR[4]=STACK;
    s.RFlags=0x246;s.MXCSR=0x1f80;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.GPR[1]=0x12340044;s.GPR[2]=0xabcdef;s.GPR[8]=0xffff000012345678ULL;
    s.GPR[9]=0x87654321;return s;
}
#define PREPARE() \
    neverd_x64_state_v6 state=site_seed(); \
    neverd_x64_call_site_context_v1 site={0};uint64_t budget=10; \
    site_outcomes model={0xfedcba9876543210ULL,0xdeadbeef,0,0,0,0x9000,STACK-32,0,0,0,0}
#define CALL() neverd_x64_call_paged_site_1005_v6( \
    &state,(uint8_t *)&model,&budget,&site,0x44,0x12345678,0x87654321)
#define RESUME() neverd_x64_resume_paged_site_1005_v6( \
    &state,(uint8_t *)&model,&budget,&site)
#define PARENT() neverd_x64_run_paged_function_v6( \
    &state,(uint8_t *)&model,&budget,&parent)
int proof_parent_handoff(uint64_t incoming) {
    PREPARE();model.incoming=incoming;state.PC=0x1000;
    neverd_x64_function_context_v1 parent={0};
    neverd_x64_begin_function_v6(&state,&parent);
    if(PARENT()!=21 || budget!=9 || model.steps!=1 || model.reads ||
       model.writes || model.cell!=0xdeadbeef || state.PC!=0x1005 ||
       state.GPR[4]!=STACK || parent.Phase!=1)return 1;
    if(PARENT()!=21 || budget!=9 || model.steps!=1)return 2;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=3 || r.Completed!=1 || r.Value!=((uint32_t)(incoming>>32)^0xabcdefU) ||
       budget!=5 || model.steps!=5 || model.writes!=1 || model.reads!=2 ||
       model.cell!=0x100a || state.PC!=0x100a || state.GPR[4]!=STACK+16 ||
       site.Callee.Function.ReturnSlot!=STACK-8 || parent.Phase!=1)return 3;
    return PARENT()!=3 || budget!=3 || model.steps!=7 || model.writes!=1 ||
        model.reads!=3 || model.bad || state.PC!=0x9000 || state.GPR[4]!=STACK+8 ||
        parent.Phase!=2 || parent.ReturnSlot!=STACK || model.incoming!=incoming;
}
int proof_budget_before_handoff(void) {
    PREPARE();state.PC=0x1000;budget=1;
    neverd_x64_function_context_v1 parent={0};
    neverd_x64_begin_function_v6(&state,&parent);
    if(PARENT()!=9 || budget || model.steps!=1 || state.PC!=0x1005)return 1;
    budget=1;
    return PARENT()!=21 || budget!=1 || model.steps!=1 || model.writes || model.reads;
}
int proof_pause_does_not_validate_instruction(void) {
    PREPARE();state.PC=0x1000;
    neverd_x64_function_context_v1 parent={0};
    neverd_x64_begin_function_v6(&state,&parent);state.PC=0x1005;state.RFlags=0;
    if(PARENT()!=21 || model.steps || budget!=10)return 1;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || model.steps!=1 || model.writes ||
        model.reads || budget!=10 || site.Prepared!=1 || site.Callee.Function.Phase;
}
int proof_wrong_entry(void) {
    PREPARE();state.PC=0x1010;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || model.steps || site.Prepared || budget!=10;
}
int proof_register_mismatch(void) {
    PREPARE();state.GPR[1]^=1;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || model.steps || site.Prepared || budget!=10;
}
int proof_zero_budget_no_stack_observation(void) {
    PREPARE();budget=0;state.GPR[4]=UINT64_MAX;model.progress=1;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=9 || r.Completed || r.Value || model.steps || model.reads ||
        model.writes || budget || site.Prepared!=1 || site.Callee.Function.Phase ||
        site.Callee.Function.ReturnSlot!=UINT64_MAX || state.GPR[4]!=UINT64_MAX;
}
int proof_call_budget_pause_and_resume(uint64_t incoming) {
    PREPARE();model.incoming=incoming;budget=1;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=9 || r.Completed || r.Value || budget || model.steps!=1 ||
       model.writes!=1 || model.reads || model.cell!=0x100a || state.PC!=0x1020 ||
       state.GPR[4]!=STACK-8 || site.Callee.Function.Phase!=1)return 1;
    state.GPR[1]^=1;budget=3;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || r.Value!=((uint32_t)(incoming>>32)^0xabcdefU) ||
        budget || model.steps!=4 || model.writes!=1 || model.reads!=2 || model.bad;
}
int proof_cached_result(uint64_t incoming) {
    PREPARE();model.incoming=incoming;neverd_x64_call_result_v1 a=CALL();
    neverd_x64_call_result_v1 b=neverd_x64_resume_paged_site_1005_v6(0,0,0,&site);
    return a.Status!=3 || a.Completed!=1 || b.Status!=3 || b.Completed!=1 ||
        b.Value!=a.Value || b.Value!=((uint32_t)(incoming>>32)^0xabcdefU) ||
        budget!=6 || model.writes!=1 || model.reads!=2 || model.steps!=4 || model.bad;
}
int proof_second_start_refused(void) {
    PREPARE();budget=1;CALL();budget=10;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5 || r.Completed || model.steps!=1 || model.writes!=1 ||
        budget!=10 || state.PC!=0x1020 || state.GPR[4]!=STACK-8;
}
int proof_wrong_site_refused(void) {
    PREPARE();budget=0;CALL();budget=10;
    neverd_x64_call_result_v1 r=neverd_x64_resume_paged_site_1010_v6(
        &state,(uint8_t *)&model,&budget,&site);
    return r.Status!=5 || r.Completed || model.steps || model.writes || budget!=10;
}
int proof_wrong_pending_sp(void) {
    PREPARE();budget=0;CALL();budget=10;state.GPR[4]-=8;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || model.steps || model.writes || budget!=10;
}
int proof_wrong_phase(void) {
    PREPARE();budget=0;CALL();budget=10;site.Callee.Function.Phase=77;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5 || r.Completed || model.steps || model.writes || budget!=10;
}
#define STOP(Name,ExpectedStatus,Progress,Remaining) \
int proof_##Name(void) { \
    PREPARE();model.stop=ExpectedStatus;model.attempted=Progress; \
    neverd_x64_call_result_v1 r=CALL(); \
    return r.Status!=ExpectedStatus || r.Completed || r.Value || budget!=Remaining || \
        model.steps!=1 || model.reads || model.writes || model.cell!=0xdeadbeef || \
        site.Prepared!=1 || site.Callee.Function.Phase || \
        site.Callee.Function.ReturnSlot!=STACK || state.PC!=0x9000 || \
        state.GPR[4]!=STACK-32; \
}
STOP(call_fault,7,1,9)
STOP(operand_backing_stop,18,0,10)
STOP(fault_delivery_backing_stop,18,1,9)
STOP(shutdown,19,1,9)
STOP(changed_code,20,0,10)
STOP(native_refusal,4,0,10)
STOP(invalid_state,5,0,10)
STOP(event_acceptance_is_not_call_progress,7,2,10)
STOP(call_without_attempt,1,0,10)
STOP(call_with_only_event_acceptance,1,2,10)
STOP(return_is_not_call,3,1,9)
int proof_fault_then_restored_retry(uint64_t incoming) {
    PREPARE();model.incoming=incoming;model.stop=7;model.attempted=1;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=7 || budget!=9 || site.Callee.Function.Phase)return 1;
    r=RESUME();
    if(r.Status!=5 || budget!=9 || model.steps!=1 || state.PC!=0x9000 ||
       state.GPR[4]!=STACK-32)return 2;
    /* The producer has handled the fault and restored the interrupted CALL. */
    state.PC=0x1005;state.GPR[4]=STACK;state.GPR[1]^=1;model.stop=0;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || r.Value!=((uint32_t)(incoming>>32)^0xabcdefU) ||
        budget!=5 || model.steps!=5 || model.writes!=1 || model.reads!=2 ||
        model.bad || site.Callee.Function.ReturnSlot!=STACK-8;
}
int proof_backing_then_retry(void) {
    PREPARE();model.stop=18;model.stop_pc=0x1005;model.stop_sp=STACK;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=18 || budget!=10 || model.writes || site.Callee.Function.Phase)return 1;
    model.stop=0;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || budget!=6 || model.steps!=5 ||
        model.writes!=1 || model.reads!=2 || model.bad;
}
int proof_fault_matching_callee_pc_is_not_success(void) {
    PREPARE();model.stop=7;model.attempted=1;
    model.stop_pc=0x1020;model.stop_sp=STACK-8;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=7 || r.Completed || site.Callee.Function.Phase ||
        site.Callee.Function.ReturnSlot!=STACK || budget!=9 || model.writes;
}
int proof_callee_fault_never_repeats_call(void) {
    PREPARE();budget=1;CALL();budget=10;model.stop=7;model.attempted=1;
    neverd_x64_call_result_v1 r=RESUME();
    if(r.Status!=7 || r.Completed || site.Callee.Function.Phase!=1 ||
       budget!=9 || model.steps!=2 || model.writes!=1)return 1;
    /* The producer restored the callee's interrupted MOV, not its CALL. */
    state.PC=0x1020;state.GPR[4]=STACK-8;model.stop=0;r=RESUME();
    return r.Status!=3 || r.Completed!=1 || budget!=6 || model.steps!=5 ||
        model.writes!=1 || model.reads!=2 || model.bad;
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto [Source, Report] =
        exportSites(LLVM, ",\"pause_call_sites\":true");
    ASSERT_FALSE(Source.empty());
    prove(LLVM, Source, Probes, 27);
  }
}
