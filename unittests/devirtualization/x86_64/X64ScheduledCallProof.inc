//===- X64ScheduledCallProof.inc - Independent call boundary predicates --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep generated instructions/controllers intact. Model paged boundary
/// outcomes independently; this is not a page walker or a guest handler.
//===----------------------------------------------------------------------===//

const char ScheduledCallProbes[] = R"C(
typedef struct { unsigned polls,acks; int ready; uint8_t kind,vector; } producer;
static int peek_event(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *shadows,
    neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    producer *p=opaque;++p->polls;event->Kind=p->kind;event->Vector=p->vector;
    return p->ready;
}
static void accept_event(const neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    producer *p=opaque;++p->acks;p->ready=0;
}
#if PAGED
#define STACK 0xffffa00000100000ULL
typedef struct {
    uint64_t incoming,cell,progress,stop,attempted,stop_pc,stop_sp,validation_stop;
    uint64_t event_status,event_progress,event_pc,event_kind;
    unsigned steps,reads,writes,validations,events,bad;
} outcomes;
typedef struct { uint64_t value,high; void *transaction; } exchange;
uint64_t neverd_x64_paged_access_v1(void *opaque,uint64_t address,
                                  uint64_t size,uint64_t flags) {
    exchange *e=opaque;outcomes *m=e->transaction;
    if(address==STACK-8 && size==8 && flags==9) {
        ++m->writes;m->cell=e->value;return 0;
    }
    ++m->reads;
    if(address==STACK+36 && size==4 && flags==8) {e->value=m->incoming>>32;return 0;}
    if(address==STACK-8 && size==8 && flags==8) {e->value=m->cell;return 0;}
    if(address==STACK && size==8 && flags==8) {e->value=0x9000;return 0;}
    m->bad=1;return 1;
}
uint64_t neverd_x64_paged_validate_v1(uint8_t *context,uint8_t *raw,
    uint64_t address,neverd_x64_paged_body_v1 validate,uint64_t paging) {
    outcomes *m=(outcomes *)context;++m->validations;
    return m->validation_stop?m->validation_stop:validate(raw,0);
}
uint64_t neverd_x64_paged_step_v1(uint8_t *context,uint8_t *raw,
    uint64_t address,const uint8_t *bytes,uint64_t size,
    neverd_x64_paged_body_v1 body,neverd_x64_paged_body_v1 validate,
    uint64_t mxcsr,uint64_t paging) {
    outcomes *m=(outcomes *)context;neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    ++m->steps;m->progress=0;uint64_t status=validate(raw,0);
    if(status)return status;
    if(m->stop) {
        m->progress=m->attempted;s->PC=m->stop_pc;s->GPR[4]=m->stop_sp;return m->stop;
    }
    exchange e={0,0,m};status=body(s,&e);m->progress=status<=3;return status;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((outcomes *)context)->progress;
}
uint64_t neverd_x64_paged_event_v1(uint8_t *context,uint8_t *raw,
                                 const uint8_t *event,uint64_t mxcsr) {
    outcomes *m=(outcomes *)context;neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    const neverd_x64_event_v1 *e=(const neverd_x64_event_v1 *)event;
    ++m->events;m->event_pc=e->SavedPC;m->event_kind=e->Kind;m->progress=m->event_progress;
    if(m->event_status!=15) {s->PC=0x9000;s->GPR[4]=STACK-32;}
    return m->event_status;
}
#define STORAGE() outcomes memory={0};memory.incoming=0xfedcba9876543210ULL; \
    memory.cell=0xdeadbeef;memory.event_status=7;memory.event_progress=2; \
    memory.stop_pc=0x9000;memory.stop_sp=STACK-32
#define MEMORY_ARG (uint8_t *)&memory,
#define NULL_MEMORY_ARG 0,
#define CALL_SITE neverd_x64_call_paged_events_site_1005_v6
#define RESUME_SITE neverd_x64_resume_paged_events_site_1005_v6
#define OTHER_SITE neverd_x64_resume_paged_events_site_1010_v6
#define CALL_ENTRY neverd_x64_call_paged_events_1020_v6
#define RESUME_ENTRY neverd_x64_resume_paged_events_1020_v6
#define DRIVER neverd_x64_continue_paged_events_function_v6
#define INCOMING memory.incoming
#define SET_INCOMING(Value) (memory.incoming=(Value))
#define CELL memory.cell
#define COUNTERS(S,R,W) (memory.steps!=(S)||memory.reads!=(R)||memory.writes!=(W)||memory.bad)
#else
/* Declared nonfaulting fixture RAM, mapped as one object by the proof harness. */
#define STORAGE() uint64_t *cells=(uint64_t *)(uintptr_t)0x10000; \
    cells[0]=0xdeadbeef;cells[1]=0x9000;cells[5]=0xfedcba9876543210ULL
#define STACK 0x10008ULL
#define MEMORY_ARG
#define NULL_MEMORY_ARG
#define CALL_SITE neverd_x64_call_events_site_1005_v6
#define RESUME_SITE neverd_x64_resume_events_site_1005_v6
#define OTHER_SITE neverd_x64_resume_events_site_1010_v6
#define CALL_ENTRY neverd_x64_call_events_1020_v6
#define RESUME_ENTRY neverd_x64_resume_events_1020_v6
#define DRIVER neverd_x64_continue_events_function_v6
#define INCOMING cells[5]
#define SET_INCOMING(Value) (cells[5]=(Value))
#define CELL cells[0]
#define COUNTERS(S,R,W) 0
#endif
static neverd_x64_state_v6 seed(void) {
    neverd_x64_state_v6 s={0};s.PC=0x1005;s.RFlags=0x246;s.MXCSR=0x1f80;s.NMIBlocked=1;
    s.GPR[1]=0x12340044;s.GPR[2]=0xabcdef;s.GPR[8]=0xffff000012345678ULL;
    s.GPR[9]=0x87654321;s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};s.DS=s.ES=s.SS;
    s.FS=(neverd_x64_segment_metadata){0xffffffff,16,0xc093};s.GS=s.FS;
    s.LDTR=(neverd_x64_segment){0,63,40,0x82};s.TR=(neverd_x64_segment){0,103,48,0x8b};
    s.STAR=0x0023001000000000ULL;return s;
}
#define PREPARE() STORAGE();neverd_x64_state_v6 state=seed();state.GPR[4]=STACK; \
    neverd_x64_call_site_context_v1 site={0};neverd_x64_call_context_v1 call={0}; \
    uint64_t budget=10,poll_budget=20;producer events={0,0,0,2,65}; \
    neverd_x64_interruptibility_v1 shadows={1,1,1,{0}}
#define ARGS &state,MEMORY_ARG &budget
#define EVENTS &shadows,&poll_budget,peek_event,accept_event,&events
#define CALL() CALL_SITE(ARGS,&site,EVENTS,0x44,0x12345678,0x87654321)
#define RESUME() RESUME_SITE(ARGS,&site,EVENTS)
#define ENTRY() CALL_ENTRY(ARGS,&call,EVENTS,0x44,0x12345678,0x87654321)
#define PARENT() DRIVER(ARGS,EVENTS,&parent)
#define NULL_SITE() RESUME_SITE(0,NULL_MEMORY_ARG 0,&site,0,0,0,0,0)
#define NULL_ENTRY() RESUME_ENTRY(0,NULL_MEMORY_ARG 0,&call,0,0,0,0,0)
#define VALUE(Input) ((uint32_t)((Input)>>32)^0xabcdefU)
int proof_parent_handoff(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);state.PC=0x1000;
    neverd_x64_function_context_v1 parent={0};neverd_x64_begin_function_v6(&state,&parent);
    if(PARENT()!=21||budget!=9||poll_budget!=19||events.polls!=1||events.acks||
       COUNTERS(1,0,0)||state.PC!=0x1005||parent.Phase!=1)return 1;
    if(PARENT()!=21||events.polls!=1||budget!=9)return 2;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=3||r.Completed!=1||r.Value!=VALUE(incoming)||budget!=5||poll_budget!=15||
       events.polls!=5||events.acks||CELL!=0x100a||state.PC!=0x100a||
       state.GPR[4]!=STACK+16||site.Callee.Function.ReturnSlot!=STACK-8||
       parent.Phase!=1||COUNTERS(5,2,1))return 3;
    return PARENT()!=3||parent.Phase!=2||state.PC!=0x9000||state.GPR[4]!=STACK+8||
        budget!=3||events.polls!=7||poll_budget!=13||COUNTERS(7,3,1)||INCOMING!=incoming;
}
int proof_budget_before_handoff(void) {
    PREPARE();state.PC=0x1000;budget=1;
    neverd_x64_function_context_v1 parent={0};neverd_x64_begin_function_v6(&state,&parent);
    if(PARENT()!=9||budget||events.polls!=1||state.PC!=0x1005)return 1;
    budget=1;return PARENT()!=21||budget!=1||events.polls!=1||COUNTERS(1,0,0);
}
int proof_checked_entry(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);state.PC=0x1020;state.GPR[4]=STACK-8;CELL=0x100a;
    neverd_x64_call_result_v1 r=ENTRY();
    return r.Status!=3||r.Completed!=1||r.Value!=VALUE(incoming)||budget!=7||
        poll_budget!=17||events.polls!=3||events.acks||shadows.STIBlocked||shadows.SSBlocked||
        state.PC!=0x100a||state.GPR[4]!=STACK+16||COUNTERS(3,2,0)||INCOMING!=incoming;
}
int proof_entry_cached(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);state.PC=0x1020;state.GPR[4]=STACK-8;CELL=0x100a;
    neverd_x64_call_result_v1 a=ENTRY(),b=NULL_ENTRY();
    return a.Status!=3||b.Status!=3||b.Completed!=1||b.Value!=VALUE(incoming)||
        budget!=7||events.polls!=3||COUNTERS(3,2,0);
}
int proof_wrong_argument(void) {
    PREPARE();state.GPR[1]^=1;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5||r.Completed||site.Prepared||budget!=10||events.polls||COUNTERS(0,0,0);
}
int proof_wrong_site(void) {
    PREPARE();budget=0;CALL();budget=10;
    neverd_x64_call_result_v1 r=OTHER_SITE(ARGS,&site,EVENTS);
    return r.Status!=5||r.Completed||budget!=10||events.polls||COUNTERS(0,0,0);
}
int proof_pending_pc(void) {
    PREPARE();budget=0;CALL();budget=10;state.PC=0x1010;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5||r.Completed||budget!=10||events.polls||COUNTERS(0,0,0);
}
int proof_pending_sp(void) {
    PREPARE();budget=0;CALL();budget=10;state.GPR[4]-=8;
    neverd_x64_call_result_v1 r=RESUME();
    return r.Status!=5||r.Completed||budget!=10||events.polls||COUNTERS(0,0,0);
}
int proof_wrong_phase(void) {
    PREPARE();budget=0;CALL();site.Callee.Function.Phase=77;
    neverd_x64_call_result_v1 r=NULL_SITE();
    return r.Status!=5||r.Completed||events.polls||COUNTERS(0,0,0);
}
int proof_second_start(void) {
    PREPARE();budget=1;CALL();budget=10;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5||r.Completed||budget!=10||events.polls!=1||COUNTERS(1,0,1);
}
int proof_zero_budget(void) {
    PREPARE();budget=0;state.GPR[4]=UINT64_MAX;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=9||r.Completed||budget||poll_budget!=20||events.polls||
        site.Prepared!=1||site.Callee.Function.Phase||site.Callee.Function.ReturnSlot!=UINT64_MAX||
        !shadows.STIBlocked||!shadows.SSBlocked||COUNTERS(0,0,0);
}
int proof_poll_budget(void) {
    PREPARE();poll_budget=0;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=16||r.Completed||budget!=10||events.polls||
        site.Callee.Function.Phase||!shadows.STIBlocked||!shadows.SSBlocked||COUNTERS(0,0,0);
}
int proof_poll_budget_after_call(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);poll_budget=1;neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=16||r.Completed||budget!=9||poll_budget||events.polls!=1||
       site.Callee.Function.Phase!=1||CELL!=0x100a||COUNTERS(1,0,1))return 1;
    poll_budget=3;r=RESUME();return r.Status!=3||r.Completed!=1||r.Value!=VALUE(incoming)||
        budget!=6||poll_budget||events.polls!=4||COUNTERS(4,2,1);
}
int proof_call_budget_pause(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);budget=1;neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=9||r.Completed||budget||events.polls!=1||site.Callee.Function.Phase!=1||
       state.PC!=0x1020||state.GPR[4]!=STACK-8||CELL!=0x100a||COUNTERS(1,0,1))return 1;
    state.GPR[1]^=1;budget=3;r=RESUME();
    return r.Status!=3||r.Completed!=1||r.Value!=VALUE(incoming)||budget||
        events.polls!=4||COUNTERS(4,2,1);
}
int proof_site_cached(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);neverd_x64_call_result_v1 a=CALL(),b=NULL_SITE();
    return a.Status!=3||b.Status!=3||b.Completed!=1||b.Value!=VALUE(incoming)||
        budget!=6||events.polls!=4||COUNTERS(4,2,1);
}
int proof_invalid_producer(void) {
    PREPARE();events.ready=2;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5||r.Completed||budget!=10||poll_budget!=19||events.polls!=1||events.acks||
        site.Callee.Function.Phase||!shadows.STIBlocked||!shadows.SSBlocked||COUNTERS(0,0,0);
}
int proof_bad_shadow(void) {
    PREPARE();shadows.STIBlocked=2;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5||r.Completed||budget!=10||poll_budget!=20||events.polls||
        site.Callee.Function.Phase||COUNTERS(0,0,0);
}
int proof_invalid_instruction(void) {
    PREPARE();state.RFlags=0;shadows.STIBlocked=0;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=5||r.Completed||budget!=10||events.polls||site.Callee.Function.Phase||
        COUNTERS(0,0,0);
}
int proof_blocked_nmi(void) {
    PREPARE();events.ready=1;events.kind=3;events.vector=2;
#if PAGED
    memory.event_status=15;memory.event_progress=0;
#endif
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=3||r.Completed!=1||events.polls!=4||events.acks||events.ready!=1||
        budget!=6||poll_budget!=16||shadows.STIBlocked||shadows.SSBlocked||
        shadows.NMIBlockedBySTI!=1||COUNTERS(4,2,1);
}
int proof_cached_entry_without_runtime(uint64_t value) {
    PREPARE();call.Entry=0x1020;call.Function.Phase=2;call.Value=value;
    neverd_x64_call_result_v1 r=NULL_ENTRY();
    return r.Status!=3||r.Completed!=1||r.Value!=value||budget!=10||events.polls;
}
#if PAGED
int proof_validation_backing_stop(void) {
    PREPARE();memory.validation_stop=18;neverd_x64_call_result_v1 r=CALL();
    return r.Status!=18||r.Completed||budget!=10||events.polls||memory.validations!=1||
        site.Callee.Function.Phase||!shadows.STIBlocked||COUNTERS(0,0,0);
}
#define STOP(Name,ExpectedStatus,Progress,Remaining) \
int proof_##Name(void) { \
    PREPARE();memory.stop=ExpectedStatus;memory.attempted=Progress; \
    neverd_x64_call_result_v1 r=CALL(); \
    return r.Status!=ExpectedStatus||r.Completed||r.Value||budget!=Remaining||events.polls!=1|| \
        poll_budget!=19||site.Callee.Function.Phase||site.Callee.Function.ReturnSlot!=STACK|| \
        state.PC!=0x9000||state.GPR[4]!=STACK-32||COUNTERS(1,0,0); \
}
STOP(call_fault,7,1,9)
STOP(attempted_backing,18,1,9)
STOP(unattempted_backing,18,0,10)
STOP(shutdown,19,1,9)
STOP(changed_code,20,0,10)
STOP(native_refusal,4,0,10)
STOP(invalid_state,5,0,10)
STOP(event_only_progress,7,2,10)
STOP(return_is_not_call,3,1,9)
STOP(call_without_attempt,1,0,10)
STOP(call_with_event_progress,1,2,10)
#define EVENT(Name,ExpectedStatus,Progress,Acks) \
int proof_##Name(void) { \
    PREPARE();events.ready=1;shadows.STIBlocked=shadows.SSBlocked=0; \
    memory.event_status=ExpectedStatus;memory.event_progress=Progress; \
    neverd_x64_call_result_v1 r=CALL(); \
    return r.Status!=ExpectedStatus||r.Completed||budget!=10||events.polls!=1||events.acks!=Acks|| \
        memory.events!=1||memory.event_pc!=0x1005||memory.event_kind!=2|| \
        site.Callee.Function.Phase||site.Callee.Function.ReturnSlot!=STACK|| \
        state.PC!=0x9000||state.GPR[4]!=STACK-32||COUNTERS(0,0,0); \
}
EVENT(accepted_event_before_call,7,2,1)
EVENT(accepted_event_backing,18,2,1)
EVENT(unaccepted_event_backing,18,0,0)
int proof_event_then_restored_call(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);events.ready=1;shadows.STIBlocked=shadows.SSBlocked=0;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=7||budget!=10||events.acks!=1||site.Callee.Function.Phase)return 1;
    r=RESUME();if(r.Status!=5||events.polls!=1||budget!=10)return 2;
    state.PC=0x1005;state.GPR[4]=STACK;state.GPR[1]^=1;r=RESUME();
    return r.Status!=3||r.Completed!=1||r.Value!=VALUE(incoming)||budget!=6||
        events.polls!=5||events.acks!=1||COUNTERS(4,2,1);
}
int proof_fault_then_restored_call(uint64_t incoming) {
    PREPARE();SET_INCOMING(incoming);memory.stop=7;memory.attempted=1;
    neverd_x64_call_result_v1 r=CALL();
    if(r.Status!=7||budget!=9||site.Callee.Function.Phase)return 1;
    r=RESUME();if(r.Status!=5||budget!=9||events.polls!=1)return 2;
    state.PC=0x1005;state.GPR[4]=STACK;state.GPR[1]^=1;memory.stop=0;r=RESUME();
    return r.Status!=3||r.Completed!=1||r.Value!=VALUE(incoming)||budget!=5||
        events.polls!=5||COUNTERS(5,2,1);
}
int proof_event_in_callee_never_repeats_call(void) {
    PREPARE();budget=1;CALL();budget=10;events.ready=1;
    neverd_x64_call_result_v1 r=RESUME();
    if(r.Status!=7||budget!=10||site.Callee.Function.Phase!=1||events.acks!=1||
       memory.event_pc!=0x1020||COUNTERS(1,0,1))return 1;
    state.PC=0x1020;state.GPR[4]=STACK-8;r=RESUME();
    return r.Status!=3||r.Completed!=1||budget!=7||events.polls!=5||COUNTERS(4,2,1);
}
int proof_fault_in_callee_never_repeats_call(void) {
    PREPARE();budget=1;CALL();budget=10;memory.stop=7;memory.attempted=1;
    neverd_x64_call_result_v1 r=RESUME();
    if(r.Status!=7||budget!=9||site.Callee.Function.Phase!=1||COUNTERS(2,0,1))return 1;
    state.PC=0x1020;state.GPR[4]=STACK-8;memory.stop=0;r=RESUME();
    return r.Status!=3||r.Completed!=1||budget!=6||events.polls!=5||COUNTERS(5,2,1);
}
int proof_fault_at_callee_coordinates_is_not_call_success(void) {
    PREPARE();memory.stop=7;memory.attempted=1;memory.stop_pc=0x1020;memory.stop_sp=STACK-8;
    neverd_x64_call_result_v1 r=CALL();
    return r.Status!=7||r.Completed||budget!=9||site.Callee.Function.Phase||
        site.Callee.Function.ReturnSlot!=STACK||COUNTERS(1,0,0);
}
#endif
)C";
