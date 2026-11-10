//===- X64CallSiteHandlerProof.inc - Original handler boundary predicates ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain original nonpaged MOV/IRET/CALL/RET bodies and native RAM. Paged
/// predicates independently supply return/fault outcomes, not a page walker.
//===----------------------------------------------------------------------===//

const char CallSiteHandlerProbes[] = R"C(
#define STACK 0x10200ULL
#define FRAME 0x10180ULL
static neverd_x64_state_v6 seed_handler(void) {
    neverd_x64_state_v6 s={0};s.PC=0x1005;s.RFlags=0x246;s.MXCSR=0x1f80;s.NMIBlocked=1;
    s.GPR[4]=STACK;s.GPR[1]=0x44;s.GPR[2]=0xabcdef;s.GPR[8]=0x12345678;
    s.GPR[9]=0x87654321;s.GPR[11]=0x10203040;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};s.DS=s.ES=s.SS;
    s.FS=(neverd_x64_segment_metadata){0xffffffff,16,0xc093};s.GS=s.FS;
    s.LDTR=(neverd_x64_segment){0,63,40,0x82};s.TR=(neverd_x64_segment){0,103,48,0x8b};
    s.GDTR=(neverd_x64_descriptor_table){0x10400,63,0,0};
    s.STAR=0x0023001000000000ULL;return s;
}
typedef struct { unsigned polls,acks; } handler_producer;
#if SCHEDULED
#define HANDLER_SHADOWS neverd_x64_interruptibility_v1 shadows={0}
static int handler_peek(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *shadows,
    neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    ++((handler_producer *)opaque)->polls;return 0;
}
static void handler_accept(const neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    ++((handler_producer *)opaque)->acks;
}
#define EVENT_ARGS &shadows,&poll_budget,handler_peek,handler_accept,&events
#else
#define HANDLER_SHADOWS
static int handler_environment(neverd_x64_state_v6 *s,uint64_t status,void *opaque) {
    ++*(unsigned *)opaque;return 0;
}
#endif
#if PAGED
typedef struct {
    uint64_t incoming,cell,progress,return_pc,return_sp,return_progress;
    uint64_t stop,attempted,call_stop,call_attempted;
    unsigned steps,reads,writes,calls,returns,bad;
} handler_outcomes;
typedef struct { uint64_t value,high;void *transaction; } handler_exchange;
uint64_t neverd_x64_paged_access_v1(void *opaque,uint64_t address,
                                  uint64_t size,uint64_t flags) {
    handler_exchange *e=opaque;handler_outcomes *m=e->transaction;
    if(address==STACK-8 && size==8 && flags==9) {++m->writes;m->cell=e->value;return 0;}
    ++m->reads;
    if(address==STACK+36 && size==4 && flags==8) {e->value=m->incoming>>32;return 0;}
    if(address==STACK-8 && size==8 && flags==8) {e->value=m->cell;return 0;}
    m->bad=1;return 1;
}
uint64_t neverd_x64_paged_validate_v1(uint8_t *context,uint8_t *raw,
    uint64_t address,neverd_x64_paged_body_v1 validate,uint64_t paging) {
    return validate(raw,0);
}
uint64_t neverd_x64_paged_step_v1(uint8_t *context,uint8_t *raw,
    uint64_t address,const uint8_t *bytes,uint64_t size,
    neverd_x64_paged_body_v1 body,neverd_x64_paged_body_v1 validate,
    uint64_t mxcsr,uint64_t paging) {
    handler_outcomes *m=(handler_outcomes *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    ++m->steps;m->progress=0;uint64_t status=validate(raw,0);if(status)return status;
    if(address==0x1005) {
        ++m->calls;
        if(m->call_stop) {
            m->progress=m->call_attempted;
            if(m->call_stop==7) {s->PC=0x1030;s->GPR[4]=FRAME;}
            return m->call_stop;
        }
    }
    if(m->stop) {
        m->progress=m->attempted;s->PC=m->return_pc;s->GPR[4]=m->return_sp;
        return m->stop;
    }
    if(address==0x1033) {
        ++m->returns;m->progress=m->return_progress;
        s->PC=m->return_pc;s->GPR[4]=m->return_sp;s->RFlags=0x246;s->NMIBlocked=0;
        return 6;
    }
    handler_exchange e={0,0,m};status=body(s,&e);m->progress=status<=3;return status;
}
uint64_t neverd_x64_paged_pending_v1(uint8_t *context) {
  (void)context;
  return 0;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((handler_outcomes *)context)->progress;
}
uint64_t neverd_x64_paged_event_v1(uint8_t *context,uint8_t *raw,
                                 const uint8_t *event,uint64_t mxcsr) { return 5; }
#define STORAGE() handler_outcomes memory={0};memory.return_pc=0x1005; \
    memory.return_sp=STACK;memory.return_progress=5;memory.cell=0xdeadbeef
#define SET_INPUT(Value) (memory.incoming=(Value))
#define INPUT memory.incoming
#define CELL memory.cell
#define SET_RETURN(PC,SP) (memory.return_pc=(PC),memory.return_sp=(SP))
#define MEMORY_ARG (uint8_t *)&memory,
#define NULL_MEMORY_ARG 0,
#define CHECK_MEMORY (memory.reads!=2||memory.writes!=1||memory.bad)
#else
static void put_handler_word(uint64_t address,uint64_t value) {
    __builtin_memcpy((void *)(uintptr_t)address,&value,8);
}
static void put_handler_frame_at(uint64_t frame,uint64_t pc,uint64_t sp) {
    put_handler_word(frame,pc);put_handler_word(frame+8,8);
    put_handler_word(frame+16,0x246);put_handler_word(frame+24,sp);
    put_handler_word(frame+32,16);
}
#define STORAGE() put_handler_word(STACK-8,0xdeadbeef); \
    put_handler_word(0x10408,0x00af9b000000ffffULL); \
    put_handler_word(0x10410,0x00cf93000000ffffULL);put_handler_frame_at(FRAME,0x1005,STACK)
#define SET_INPUT(Value) put_handler_word(STACK+32,(Value))
#define INPUT (*(uint64_t *)(uintptr_t)(STACK+32))
#define CELL (*(uint64_t *)(uintptr_t)(STACK-8))
#define SET_RETURN(PC,SP) put_handler_frame_at(FRAME,(PC),(SP))
#define MEMORY_ARG
#define NULL_MEMORY_ARG
#define CHECK_MEMORY 0
#endif
#if SCHEDULED
#if PAGED
#define START_SITE neverd_x64_call_paged_events_site_1005_v6
#define RESUME_HANDLER neverd_x64_resume_handler_paged_events_site_1005_v6
#else
#define START_SITE neverd_x64_call_events_site_1005_v6
#define RESUME_HANDLER neverd_x64_resume_handler_events_site_1005_v6
#endif
#define ARGS &state,MEMORY_ARG &budget,&site,EVENT_ARGS
#define NULL_ARGS 0,NULL_MEMORY_ARG 0,&site,0,0,0,0,0
#define CHECK_POLLS(Count) (events.polls!=(Count)||poll_budget!=20-(Count)||events.acks)
#elif PAGED
#define START_SITE neverd_x64_call_paged_site_1005_v6
#define RESUME_HANDLER neverd_x64_resume_handler_paged_site_1005_v6
#define ARGS &state,MEMORY_ARG &budget,&site
#define NULL_ARGS 0,NULL_MEMORY_ARG 0,&site
#define CHECK_POLLS(Count) 0
#else
#define START_SITE neverd_x64_call_site_1005_v6
#define RESUME_HANDLER neverd_x64_resume_handler_site_1005_v6
#define ARGS &state,&budget,&environment_budget,&site,handler_environment,&providers
#define NULL_ARGS 0,0,0,&site,0,0
#define CHECK_POLLS(Count) 0
#endif
#define PREPARE() STORAGE();SET_INPUT(0xfedcba9876543210ULL); \
    neverd_x64_state_v6 state=seed_handler();neverd_x64_call_site_context_v1 site={0}; \
    neverd_x64_call_site_handler_context_v1 handler={0}; \
    uint64_t budget=0,environment_budget=23,poll_budget=20;unsigned providers=0; \
    handler_producer events={0};HANDLER_SHADOWS; \
    START_SITE(ARGS,0x44,0x12345678,0x87654321); \
    if(!neverd_x64_begin_call_site_handler_v1(&site,&handler))return 99; \
    state.PC=0x1030;state.GPR[4]=FRAME;budget=10
#define RESUME() RESUME_HANDLER(ARGS,&handler)
#define RESULT(Input,Value) ((uint32_t)((Input)>>32)^(uint32_t)(Value))
int proof_handler_returns_then_calls(uint64_t incoming,uint32_t value) {
    PREPARE();SET_INPUT(incoming);state.GPR[11]=value;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=3||r.Completed!=1||r.Value!=RESULT(incoming,value)||
        state.PC!=0x100a||state.GPR[4]!=STACK+16||state.GPR[2]!=value||state.NMIBlocked||
        budget!=4||site.Callee.Function.Phase!=2||CELL!=0x100a||INPUT!=incoming||
        CHECK_MEMORY||CHECK_POLLS(6)||providers||environment_budget!=23||!handler.Ready;
}
int proof_handler_budget_then_resume(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);budget=1;
    neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=9||a.Completed||budget||state.PC!=0x1033||handler.Ready||CELL!=0xdeadbeef)
        return 1;
    budget=5;state.GPR[11]=0x99887766;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x10203040)||
        budget||CHECK_MEMORY||CHECK_POLLS(6)||INPUT!=incoming;
}
int proof_handler_last_budget_return(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);budget=2;neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=9||a.Completed||budget||!handler.Ready||state.PC!=0x1005||
       state.GPR[4]!=STACK||site.Callee.Function.Phase||CELL!=0xdeadbeef)return 1;
    budget=4;state.GPR[11]=0x99887766;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x10203040)||
        budget||CHECK_MEMORY||CHECK_POLLS(6)||INPUT!=incoming;
}
int proof_handler_return_wrong_pc(void) {
    PREPARE();SET_RETURN(0x1010,STACK);neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=6||r.Completed||handler.Ready||site.Callee.Function.Phase||
        budget!=8||state.PC!=0x1010||CELL!=0xdeadbeef||CHECK_POLLS(2);
}
int proof_handler_return_wrong_sp(void) {
    PREPARE();SET_RETURN(0x1005,STACK+16);neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=6||r.Completed||handler.Ready||site.Callee.Function.Phase||
        budget!=8||state.GPR[4]!=STACK+16||CELL!=0xdeadbeef||CHECK_POLLS(2);
}
int proof_handler_entry_coordinates_do_not_complete(void) {
    PREPARE();state.PC=0x1005;state.GPR[4]=STACK;neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=21||r.Completed||handler.Ready||site.Callee.Function.Phase||
        budget!=10||CELL!=0xdeadbeef||CHECK_POLLS(0);
}
int proof_handler_outside_selection(void) {
    PREPARE();state.PC=0x9000;neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=8||r.Completed||handler.Ready||budget!=10||CELL!=0xdeadbeef||CHECK_POLLS(0);
}
int proof_handler_wrong_binding(void) {
    PREPARE();handler.ReturnSlot+=8;neverd_x64_call_result_v1 r=RESUME_HANDLER(NULL_ARGS,&handler);
    return r.Status!=5||r.Completed||handler.Ready||budget!=10||CHECK_POLLS(0);
}
int proof_handler_wrong_site(void) {
    PREPARE();handler.Site=0x1010;neverd_x64_call_result_v1 r=RESUME_HANDLER(NULL_ARGS,&handler);
    return r.Status!=5||r.Completed||handler.Ready||budget!=10||CHECK_POLLS(0);
}
int proof_handler_invalid_phase(void) {
    PREPARE();handler.Ready=2;neverd_x64_call_result_v1 r=RESUME_HANDLER(NULL_ARGS,&handler);
    return r.Status!=5||r.Completed||handler.Ready!=2||budget!=10||CHECK_POLLS(0);
}
int proof_handler_completed_cache(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);neverd_x64_call_result_v1 a=RESUME();
    neverd_x64_call_result_v1 b=RESUME_HANDLER(NULL_ARGS,0);
    return a.Status!=3||b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x10203040)||
        budget!=4||CHECK_MEMORY||CHECK_POLLS(6);
}
int proof_handler_near_return_is_not_privilege_return(void) {
    PREPARE();state.PC=0x100e;state.GPR[4]=STACK-8;CELL=0x1005;budget=1;
    neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=9||a.Completed||handler.Ready||site.Callee.Function.Phase||
       budget||CELL!=0x1005||state.PC!=0x1005||state.GPR[4]!=STACK)return 1;
    budget=1;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=21||b.Completed||handler.Ready||site.Callee.Function.Phase||
        budget!=1||CELL!=0x1005||state.PC!=0x1005||state.GPR[4]!=STACK||CHECK_POLLS(1);
}
int proof_handler_nested_privilege_returns(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);SET_RETURN(0x1030,FRAME+64);
#if !PAGED
    put_handler_frame_at(FRAME+64,0x1005,STACK);
#endif
    neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=6||a.Completed||handler.Ready||site.Callee.Function.Phase||
       budget!=8||state.PC!=0x1030||state.GPR[4]!=FRAME+64||CELL!=0xdeadbeef)return 1;
#if PAGED
    SET_RETURN(0x1005,STACK);
#endif
    state.GPR[11]=0x99887766;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x99887766)||budget!=2||
        INPUT!=incoming||CHECK_MEMORY||CHECK_POLLS(8);
}
int proof_handler_begin_rejects_active(void) {
    PREPARE();site.Callee.Function.Phase=1;handler.Site=0x8765;handler.ReturnSlot=0x4321;handler.Ready=1;
    return neverd_x64_begin_call_site_handler_v1(&site,&handler)!=0||
        handler.Site!=0x8765||handler.ReturnSlot!=0x4321||handler.Ready!=1;
}
#if SCHEDULED
int proof_handler_poll_budget_after_return(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);poll_budget=2;
    neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=16||a.Completed||!handler.Ready||budget!=8||poll_budget||events.polls!=2||
       state.PC!=0x1005||site.Callee.Function.Phase||CELL!=0xdeadbeef)return 1;
    poll_budget=18;state.GPR[11]=0x99887766;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x10203040)||
        budget!=4||CHECK_MEMORY||CHECK_POLLS(6);
}
#endif
#if PAGED
int proof_handler_return_needs_completion(void) {
    PREPARE();state.PC=0x1033;memory.return_progress=2;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=6||r.Completed||handler.Ready||budget!=10||memory.calls||memory.writes;
}
int proof_handler_fault_at_matching_coordinates(void) {
    PREPARE();memory.stop=7;memory.attempted=1;neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=7||r.Completed||handler.Ready||budget!=9||memory.calls||memory.writes||
        state.PC!=0x1005||state.GPR[4]!=STACK;
}
int proof_handler_backing_stop(void) {
    PREPARE();memory.stop=18;memory.attempted=0;neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=18||r.Completed||handler.Ready||budget!=10||memory.calls||memory.writes;
}
int proof_handler_call_fault_restarts_handler(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);memory.call_stop=7;memory.call_attempted=1;
    neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=7||a.Completed||handler.Ready||budget!=7||memory.calls!=1||memory.writes||
       site.Callee.Function.Phase||state.PC!=0x1030||state.GPR[4]!=FRAME)return 1;
    memory.call_stop=0;state.GPR[11]=0x99887766;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x99887766)||budget!=1||
        memory.calls!=2||memory.returns!=2||CHECK_MEMORY||CHECK_POLLS(9);
}
int proof_handler_call_backing_does_not_replay_handler(uint64_t incoming) {
    PREPARE();SET_INPUT(incoming);memory.call_stop=18;memory.call_attempted=0;
    neverd_x64_call_result_v1 a=RESUME();
    if(a.Status!=18||a.Completed||!handler.Ready||budget!=8||memory.calls!=1||memory.writes||
       site.Callee.Function.Phase||state.PC!=0x1005)return 1;
    memory.call_stop=0;state.GPR[11]=0x99887766;neverd_x64_call_result_v1 b=RESUME();
    return b.Status!=3||b.Completed!=1||b.Value!=RESULT(incoming,0x10203040)||budget!=4||
        memory.calls!=2||memory.returns!=1||CHECK_MEMORY||CHECK_POLLS(7);
}
#endif
)C";
