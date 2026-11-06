//===- X64ScheduledFunctionContextTests.inc - Scheduled activations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile original instructions and prove scheduled function boundaries.
/// These checks execute no generated program, instruction executor or guest.
//===----------------------------------------------------------------------===//

class X64ScheduledFunctionContextTest : public X64ContinuationExportTest {
protected:
  void loadReturns() {
    load({0xc3, 0xc3, 0xeb, 0xfc, 0xcb, 0x90, 0xfb, 0xc2, 0x10, 0},
         {{0x1000, 1},
          {0x1001, 1},
          {0x1002, 2},
          {0x1004, 1},
          {0x1005, 1},
          {0x1006, 1},
          {0x1007, 3}});
  }

  static std::string selection(bool Paged, llvm::StringRef Extra = "") {
    return request(
        (R"("entries":[4096,4097,4098,4100,4101,4102,4103],"follow_internal_calls":true,"scheduled_events":true,"paged_memory":)" +
         llvm::StringRef(Paged ? "true" : "false,\"event_delivery\":true") +
         Extra)
            .str());
  }

  std::pair<std::string, std::string> exportSource(bool Paged, bool LLVM,
                                                   llvm::StringRef Extra) {
    const auto Request = selection(Paged, Extra);
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    return {std::move(Source), take(Report)};
  }

  void prove(bool Paged, bool LLVM, llvm::StringRef Source,
             llvm::StringRef Probes, unsigned Expected) {
    if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
      GTEST_SKIP() << "folding this model to a constant needs a current "
                      "optimizer, and this Clang is older";
    const auto File = tmpFile("scheduled-function-proof.c");
    const auto InitialIR = tmpFile("scheduled-function-initial.ll");
    const auto IR = tmpFile("scheduled-function-proof.ll");
    std::ofstream(File) << Source.str() << "\n#define PAGED " << Paged << "\n"
                        << Probes.str();
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", "-O2", "-mllvm", "-inline-threshold=10000", "-mllvm",
              "-inline-cold-callsite-threshold=10000", "-S", "-emit-llvm",
              File.string(), "-o", InitialIR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    const auto Simplified =
        exec(NEVERD_TEST_CLANG, {"-O2", "-S", "-emit-llvm", InitialIR.string(),
                                 "-o", IR.string()});
    if (const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      const fs::path Directory(Root);
      fs::create_directories(Directory);
      const std::string Name = std::string("scheduled-function-") +
                               (Paged ? "paged-" : "plain-") +
                               (LLVM ? "llvm" : "high");
      fs::copy_file(File, Directory / (Name + ".c"),
                    fs::copy_options::overwrite_existing);
      fs::copy_file(InitialIR, Directory / (Name + ".initial.ll"),
                    fs::copy_options::overwrite_existing);
      if (fs::exists(IR))
        fs::copy_file(IR, Directory / (Name + ".ll"),
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

TEST_F(X64ScheduledFunctionContextTest, AdmissionAndBothSourceRoutes) {
  loadReturns();
  for (bool Paged : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Paged);
      SCOPED_TRACE(LLVM);
      const auto Default = exportSource(Paged, LLVM, "");
      const auto Disabled =
          exportSource(Paged, LLVM, ",\"function_context\":false");
      EXPECT_EQ(Default.first, Disabled.first);
      auto DefaultManifest = llvm::json::parse(Default.second);
      auto DisabledManifest = llvm::json::parse(Disabled.second);
      ASSERT_TRUE(bool(DefaultManifest));
      ASSERT_TRUE(bool(DisabledManifest));
      DisabledManifest->getAsObject()->getObject("request")->erase(
          "function_context");
      EXPECT_TRUE(*DefaultManifest == *DisabledManifest);
      const auto [Source, Report] =
          exportSource(Paged, LLVM, ",\"function_context\":true");
      ASSERT_FALSE(Source.empty()) << take(neverd_last_error(Session));
      auto Manifest = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Manifest));
      const auto *Contract =
          Manifest->getAsObject()->getObject("function_context");
      ASSERT_NE(Contract, nullptr);
      EXPECT_EQ(Contract->getString("scheduled_driver"),
                Paged ? "neverd_x64_continue_paged_events_function_v6"
                      : "neverd_x64_continue_events_function_v6");
      const auto File = tmpFile("scheduled-function.c");
      std::ofstream(File) << Source;
      for (const char *Level : {"-O0", "-O2"}) {
        const auto Built =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c11", Level, "-Werror=uninitialized",
                  "-Werror=return-type", "-c", File.string(), "-o",
                  tmpFile("scheduled-function.o").string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
      }
    }
}

TEST_F(X64ScheduledFunctionContextTest, ProvesScheduleAndReturnComposition) {
  loadReturns();
  // Keep every emitted body, validation function, event-delivery function and
  // controller intact. The paged boundary model supplies operand reads and
  // independent fault/backing/event outcomes, not a page walker or guest.
  // Its memory context is disjoint from the event producer's context.
  const char Probes[] = R"C(
static neverd_x64_state_v6 seed(void) {
    neverd_x64_state_v6 s={0};s.RFlags=0x246;s.PC=0x1000;s.CR8=3;s.MXCSR=0x1f80;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};
    s.DS=s.ES=s.SS;s.FS=(neverd_x64_segment_metadata){0xffffffff,16,0xc093};s.GS=s.FS;
    s.LDTR=(neverd_x64_segment){0,63,40,0x82};s.TR=(neverd_x64_segment){0,103,48,0x8b};
    s.STAR=0x0023001000000000ULL;s.NMIBlocked=1;return s;
}
typedef struct { unsigned polls, acks, sti_polls; int ready; uint8_t kind, vector; } producer;
static int peek_event(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *shadows,
    neverd_x64_pending_interrupt_v1 *pending,void *opaque) {
    producer *p=opaque;++p->polls;p->sti_polls+=shadows->STIBlocked;
    pending->Kind=p->kind;pending->Vector=p->vector;return p->ready;
}
static void accept_event(const neverd_x64_pending_interrupt_v1 *pending,void *opaque) {
    producer *p=opaque;++p->acks;p->ready=0;
}
#if PAGED
#define STACK 0xffffa00000100000ULL
typedef struct {
    unsigned validations, steps, reads, events, bad;
    uint64_t progress, validation_stop, override_step, status, attempted, pc, sp;
    uint64_t event_status, event_progress;
    uint64_t event_pc, event_kind, event_vector, event_sti, event_ss, event_policy;
} boundary_outcomes;
typedef struct { uint64_t value, high; void *transaction; } operand_exchange;
uint64_t neverd_x64_paged_access_v1(void *opaque,uint64_t address,
                                  uint64_t size,uint64_t flags) {
    operand_exchange *e=opaque;boundary_outcomes *m=e->transaction;
    /* Ring-zero STI's authorized IF mask, supplied by the runtime boundary. */
    if(address==1 && size==0 && flags==5) {e->value=512;return 0;}
    ++m->reads;
    if(size==8 && flags==8) {
        if(address==STACK-8) {e->value=0x1000;return 0;}
        if(address==STACK) {e->value=0x1001;return 0;}
        if(address==STACK+8) {e->value=0x9000;return 0;}
    }
    m->bad=1;return 1;
}
uint64_t neverd_x64_paged_validate_v1(uint8_t *context,uint8_t *raw,
    uint64_t address,neverd_x64_paged_body_v1 validate,uint64_t paging) {
    boundary_outcomes *m=(boundary_outcomes *)context;++m->validations;
    return m->validation_stop?m->validation_stop:validate(raw,0);
}
uint64_t neverd_x64_paged_step_v1(uint8_t *context,uint8_t *raw,
    uint64_t address,const uint8_t *bytes,uint64_t size,
    neverd_x64_paged_body_v1 body,neverd_x64_paged_body_v1 validate,
    uint64_t mxcsr,uint64_t paging) {
    boundary_outcomes *m=(boundary_outcomes *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    ++m->steps;m->progress=0;uint64_t status=validate(raw,0);
    if(status)return status;
    if(m->override_step) {
        m->progress=m->attempted;s->PC=m->pc;s->GPR[4]=m->sp;return m->status;
    }
    operand_exchange e={0,0,m};status=body(s,&e);
    m->progress=status<=3;return status;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((boundary_outcomes *)context)->progress;
}
uint64_t neverd_x64_paged_event_v1(uint8_t *context,uint8_t *raw,
                                 const uint8_t *event,uint64_t mxcsr) {
    boundary_outcomes *m=(boundary_outcomes *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;++m->events;
    const neverd_x64_event_v1 *e=(const neverd_x64_event_v1 *)event;
    m->event_pc=e->SavedPC;m->event_kind=e->Kind;m->event_vector=e->Vector;
    m->event_sti=e->STIBlocked;m->event_ss=e->SSBlocked;m->event_policy=e->NMIBlockedBySTI;
    m->progress=m->event_progress;
    if(m->event_status!=15) {s->PC=0x1001;s->GPR[4]=STACK-32;}
    return m->event_status;
}
#define STORAGE() boundary_outcomes memory={0};memory.event_status=7;memory.event_progress=2
#define INNER_STACK (STACK-8)
#define DRIVER neverd_x64_continue_paged_events_function_v6
#define MEMORY_ARG (uint8_t *)&memory,
#define NULL_MEMORY_ARG 0,
#define RAW_DRIVER neverd_x64_continue_paged_events_v6
#else
#define STORAGE() uint64_t cells[6]={0x1000,0x1001,0x9000,0xfedcba9876543210ULL,0,0}
#define STACK ((uint64_t)(uintptr_t)&cells[1])
#define INNER_STACK ((uint64_t)(uintptr_t)&cells[0])
#define DRIVER neverd_x64_continue_events_function_v6
#define MEMORY_ARG
#define NULL_MEMORY_ARG
#define RAW_DRIVER neverd_x64_continue_events_v6
#endif
#define PREPARE() \
    STORAGE();neverd_x64_state_v6 state=seed();state.GPR[4]=STACK; \
    neverd_x64_function_context_v1 function={0}; \
    neverd_x64_interruptibility_v1 shadows={1,1,1,{0}}; \
    uint64_t budget=5,poll_budget=10;producer events={0,0,0,0,2,65}; \
    neverd_x64_begin_function_v6(&state,&function)
#define RUN() DRIVER(&state,MEMORY_ARG &budget,&shadows,&poll_budget,peek_event,accept_event,&events,&function)
#define NO_RUNTIME(Function,Budget) DRIVER(0,NULL_MEMORY_ARG Budget,0,0,0,0,0,Function)
int proof_return_to_selected_target(void) {
    PREPARE();
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        budget!=4 || poll_budget!=9 || events.polls!=1 || events.acks ||
        shadows.STIBlocked || shadows.SSBlocked || shadows.NMIBlockedBySTI!=1 ||
        state.PC!=0x1001 || state.GPR[4]!=STACK+8 || function.ReturnSlot!=STACK;
}
int proof_return_immediate(void) {
    PREPARE();state.PC=0x1007;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        budget!=4 || poll_budget!=9 || events.polls!=1 ||
        shadows.STIBlocked || shadows.SSBlocked || state.PC!=0x1001 ||
        state.GPR[4]!=STACK+24;
}
int proof_nested_return(void) {
    PREPARE();
#if PAGED
    state.PC=0x1001;state.GPR[4]=INNER_STACK;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        budget!=3 || poll_budget!=8 || events.polls!=2 ||
        state.PC!=0x1001 || state.GPR[4]!=STACK+8 || function.ReturnSlot!=STACK;
#else
    /* Declare the same adjacent return cell in integer stack coordinates.
       A host-pointer GEP/ptrtoint identity is outside this controller proof. */
    state.GPR[4]=INNER_STACK+8;neverd_x64_begin_function_v6(&state,&function);
    cells[0]=0x9000;state.PC=0x1001;state.GPR[4]=INNER_STACK;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        budget!=4 || poll_budget!=9 || events.polls!=1 ||
        state.PC!=0x9000 || state.GPR[4]!=INNER_STACK+8 ||
        function.ReturnSlot!=INNER_STACK+8;
#endif
}
int proof_post_sp_is_not_return_ownership(void) {
    PREPARE();state.GPR[4]=INNER_STACK+8;
    neverd_x64_begin_function_v6(&state,&function);
    state.GPR[4]=INNER_STACK;budget=1;
    return RUN()!=9 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        budget || poll_budget!=9 || events.polls!=1 ||
        state.PC!=0x1000 || state.GPR[4]!=function.ReturnSlot;
}
int proof_completed_before_runtime_access(void) {
    neverd_x64_function_context_v1 f={0,NEVERD_X64_FUNCTION_RETURNED};
    return NO_RUNTIME(&f,0)!=3;
}
int proof_uninitialized_before_runtime_access(void) {
    neverd_x64_function_context_v1 f={0,NEVERD_X64_FUNCTION_UNINITIALIZED};
    return NO_RUNTIME(&f,0)!=5;
}
int proof_bad_phase_before_runtime_access(void) {
    neverd_x64_function_context_v1 f={0,3};return NO_RUNTIME(&f,0)!=5;
}
int proof_zero_budget_before_runtime_access(void) {
    neverd_x64_function_context_v1 f={0,NEVERD_X64_FUNCTION_ACTIVE};uint64_t budget=0;
    return NO_RUNTIME(&f,&budget)!=9 || budget;
}
int proof_poll_budget_retains_activation_and_shadows(void) {
    PREPARE();poll_budget=0;
    return RUN()!=16 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        budget!=5 || poll_budget || events.polls || events.acks ||
        !shadows.STIBlocked || !shadows.SSBlocked || state.PC!=0x1000;
}
int proof_bad_shadow(void) {
    PREPARE();shadows.STIBlocked=2;
    return RUN()!=5 || budget!=5 || poll_budget!=10 || events.polls ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_reserved_shadow(void) {
    PREPARE();shadows.Reserved[4]=1;
    return RUN()!=5 || budget!=5 || poll_budget!=10 || events.polls;
}
int proof_sti_shadow_requires_if(void) {
    PREPARE();state.RFlags=2;
    return RUN()!=5 || budget!=5 || poll_budget!=10 || events.polls;
}
int proof_missing_peek(void) {
    PREPARE();return DRIVER(&state,MEMORY_ARG &budget,&shadows,&poll_budget,
        0,accept_event,&events,&function)!=5 || budget!=5 || poll_budget!=10;
}
int proof_missing_accept(void) {
    PREPARE();return DRIVER(&state,MEMORY_ARG &budget,&shadows,&poll_budget,
        peek_event,0,&events,&function)!=5 || budget!=5 || poll_budget!=10;
}
int proof_invalid_state_before_polling(void) {
    PREPARE();shadows.STIBlocked=0;state.RFlags=0;
    return RUN()!=5 || budget!=5 || poll_budget!=10 || events.polls;
}
int proof_unselected_before_polling(void) {
    PREPARE();state.PC=0x9000;
    return RUN()!=8 || budget!=5 || poll_budget!=10 || events.polls;
}
int proof_bad_peek(void) {
    PREPARE();events.ready=2;
    return RUN()!=5 || budget!=5 || poll_budget!=9 || events.polls!=1 ||
        events.acks || !shadows.STIBlocked || !shadows.SSBlocked;
}
int proof_bad_event_kind(void) {
    PREPARE();events.ready=1;events.kind=1;
    return RUN()!=5 || budget!=5 || poll_budget!=9 || events.polls!=1 || events.acks;
}
int proof_bad_nmi_vector(void) {
    PREPARE();events.ready=1;events.kind=3;events.vector=65;
    return RUN()!=5 || budget!=5 || poll_budget!=9 || events.polls!=1 || events.acks;
}
int proof_sti_then_ret_immediate(void) {
    PREPARE();state.PC=0x1006;state.RFlags=2;shadows.STIBlocked=shadows.SSBlocked=0;
    budget=1;
    if(RUN()!=9 || budget || poll_budget!=9 || state.PC!=0x1007 ||
       !shadows.STIBlocked || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE)return 1;
    budget=1;
    return RUN()!=3 || budget || poll_budget!=8 || events.polls!=2 ||
        events.sti_polls!=1 || shadows.STIBlocked || shadows.SSBlocked ||
        shadows.NMIBlockedBySTI!=1 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        state.RFlags!=514 || state.PC!=0x1001 || state.GPR[4]!=STACK+24;
}
int proof_raw_adapter_keeps_following_returns(void) {
    PREPARE();budget=1;uint64_t status=RAW_DRIVER(&state,MEMORY_ARG &budget,&shadows,
        &poll_budget,peek_event,accept_event,&events);
    return status!=9 || budget || poll_budget!=9 || events.polls!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        state.PC!=0x1001 || state.GPR[4]!=STACK+8;
}
#if PAGED
int proof_ret_uses_one_operand_read(void) {
    PREPARE();return RUN()!=3 || memory.steps!=1 || memory.validations!=1 ||
        memory.reads!=1 || memory.events || memory.bad || memory.progress!=1;
}
#define STOP(Name,Status,Progress,Remaining,Shadow) \
int proof_##Name(void) { \
    PREPARE();memory.override_step=1;memory.status=Status;memory.attempted=Progress; \
    memory.pc=0x1001;memory.sp=STACK-32; \
    return RUN()!=Status || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || \
        budget!=Remaining || poll_budget!=9 || events.polls!=1 || events.acks || \
        memory.steps!=1 || memory.reads || memory.bad || \
        shadows.STIBlocked!=Shadow || shadows.SSBlocked!=Shadow || \
        shadows.NMIBlockedBySTI!=1 || state.PC!=0x1001 || state.GPR[4]!=STACK-32; \
}
STOP(fault_keeps_activation,7,1,4,0)
STOP(attempted_backing,18,1,4,0)
STOP(shutdown,19,1,4,0)
STOP(unattempted_backing,18,0,5,1)
STOP(changed_code,20,0,5,1)
STOP(native_refusal,4,0,5,1)
STOP(invalid_step,5,0,5,1)
STOP(return_without_attempt,3,0,5,1)
STOP(event_progress_is_not_instruction_progress,3,2,5,1)
int proof_validation_backing_stop_before_poll(void) {
    PREPARE();memory.validation_stop=18;
    return RUN()!=18 || budget!=5 || poll_budget!=10 || events.polls ||
        memory.validations!=1 || memory.steps || !shadows.STIBlocked ||
        !shadows.SSBlocked || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_far_return_cannot_complete(void) {
    PREPARE();state.PC=0x1004;memory.override_step=1;memory.status=3;
    memory.attempted=1;memory.pc=0x9000;memory.sp=STACK+8;
    return RUN()!=3 || budget!=4 || memory.steps!=1 || events.polls!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || shadows.STIBlocked || shadows.SSBlocked;
}
int proof_jump_cannot_complete(void) {
    PREPARE();state.PC=0x1002;memory.override_step=1;memory.status=3;
    memory.attempted=1;memory.pc=0x9000;memory.sp=STACK+8;
    return RUN()!=3 || budget!=4 || memory.steps!=1 || events.polls!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE;
}
int proof_repeat_chunk_cannot_complete(void) {
    PREPARE();state.PC=0x1005;budget=1;memory.override_step=1;memory.status=21;
    memory.attempted=1;memory.pc=0x1005;memory.sp=STACK;
    return RUN()!=9 || budget || memory.steps!=1 || events.polls!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || shadows.STIBlocked || shadows.SSBlocked;
}
#define EVENT_STOP(Name,Status,Progress,Acks) \
int proof_##Name(void) { \
    PREPARE();events.ready=1;memory.event_status=Status;memory.event_progress=Progress; \
    shadows.STIBlocked=shadows.SSBlocked=0; \
    return RUN()!=Status || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || \
        budget!=5 || poll_budget!=9 || events.polls!=1 || events.acks!=Acks || \
        events.ready!=!Acks || memory.events!=1 || memory.steps || memory.reads || \
        shadows.STIBlocked || shadows.SSBlocked || \
        state.PC!=0x1001 || state.GPR[4]!=STACK-32; \
}
EVENT_STOP(accepted_event_is_not_a_return,7,2,1)
EVENT_STOP(accepted_event_backing_stop,18,2,1)
EVENT_STOP(accepted_event_shutdown,19,2,1)
EVENT_STOP(event_without_acceptance,7,0,0)
int proof_nmi_admission_keeps_activation(void) {
    PREPARE();events.ready=1;events.kind=3;events.vector=2;
    state.NMIBlocked=0;shadows.STIBlocked=shadows.SSBlocked=0;
    return RUN()!=7 || budget!=5 || events.polls!=1 || events.acks!=1 ||
        memory.steps || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE ||
        memory.event_pc!=0x1000 || memory.event_kind!=3 || memory.event_vector!=2 ||
        memory.event_sti || memory.event_ss || memory.event_policy!=1;
}
int proof_nmi_inhibition_reaches_event_owner(void) {
    PREPARE();events.ready=1;events.kind=3;events.vector=2;
    memory.event_status=15;memory.event_progress=0;
    return RUN()!=3 || budget!=4 || events.polls!=1 || events.acks || events.ready!=1 ||
        function.Phase!=NEVERD_X64_FUNCTION_RETURNED || memory.event_pc!=0x1000 ||
        memory.event_kind!=3 || memory.event_vector!=2 || memory.event_sti!=1 ||
        memory.event_ss!=1 || memory.event_policy!=1 || state.NMIBlocked!=1;
}
int proof_fault_then_restored_retry(void) {
    PREPARE();memory.override_step=1;memory.status=7;memory.attempted=1;
    memory.pc=0x1800;memory.sp=STACK-32;
    if(RUN()!=7 || function.Phase!=NEVERD_X64_FUNCTION_ACTIVE || budget!=4)return 1;
    /* The producer has restored the original faulting context before retry. */
    state.PC=0x1000;state.GPR[4]=STACK;memory.override_step=0;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED || budget!=3 ||
        poll_budget!=8 || events.polls!=2 || memory.steps!=2 || memory.reads!=1;
}
int proof_event_then_restored_resume(void) {
    PREPARE();events.ready=1;shadows.STIBlocked=shadows.SSBlocked=0;
    if(RUN()!=7 || events.acks!=1 || budget!=5 || memory.steps ||
       function.Phase!=NEVERD_X64_FUNCTION_ACTIVE)return 1;
    /* Handler/IRET effects are supplied by the producer, not inferred here. */
    state.PC=0x1000;state.GPR[4]=STACK;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED || budget!=4 ||
        events.acks!=1 || events.polls!=2 || memory.steps!=1 || memory.events!=1;
}
#else
int proof_stack_bytes_are_not_initialized(uint64_t incoming) {
    PREPARE();cells[3]=incoming;
    return RUN()!=3 || cells[0]!=0x1000 || cells[1]!=0x1001 ||
        cells[2]!=0x9000 || cells[3]!=incoming;
}
#endif
#if PAGED
int proof_blocked_event_then_return(void) {
    PREPARE();events.ready=1;
    memory.event_status=15;memory.event_progress=0;
    return RUN()!=3 || function.Phase!=NEVERD_X64_FUNCTION_RETURNED ||
        budget!=4 || poll_budget!=9 || events.polls!=1 || events.acks ||
        events.ready!=1 || shadows.STIBlocked || shadows.SSBlocked ||
        state.PC!=0x1001 || state.GPR[4]!=STACK+8;
}
#endif
)C";
  for (bool Paged : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Paged);
      SCOPED_TRACE(LLVM);
      const auto [Source, Report] =
          exportSource(Paged, LLVM, ",\"function_context\":true");
      ASSERT_FALSE(Source.empty());
      prove(Paged, LLVM, Source, Probes, Paged ? 44 : 22);
    }
}
