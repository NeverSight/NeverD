//===- X64StackObserverProof.inc - Preserved native stack inputs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Static predicates over original partial stores, pointer spills and reloads.
/// The paged operand model supplies accessible RAM, not paging/debug delivery.
//===----------------------------------------------------------------------===//

const char StackObserverProbes[] = R"C(
#define BASE 0x10000ULL
#define STACK (BASE+160)
#define SLOT (STACK-24)
typedef struct {
    uint8_t bytes[256];
    uint64_t progress, transfer, instruction, pc, sp, rax;
    unsigned steps, reads, writes, bad;
} observer_memory;
typedef struct { uint64_t value, high; void *transaction; } observer_exchange;
static neverd_x64_state_v6 observer_seed(uint32_t low, uint64_t saved) {
    neverd_x64_state_v6 s={0};s.PC=0x1000;s.GPR[4]=STACK;
    s.GPR[1]=0xfeedface00000000ULL|low;s.GPR[7]=saved;
    s.RFlags=0x246;s.MXCSR=0x1f80;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};return s;
}
#if PAGED
#if DEBUG
#define ACCESS neverd_x64_paged_debug_access_v1
#define VALIDATE neverd_x64_paged_debug_validate_v2
#define STEP neverd_x64_paged_debug_step_v2
#define SHADOW_PARAMETER , uint64_t ss_blocked
#else
#define ACCESS neverd_x64_paged_access_v1
#define VALIDATE neverd_x64_paged_validate_v1
#define STEP neverd_x64_paged_step_v1
#define SHADOW_PARAMETER
#endif
uint64_t ACCESS(void *opaque,uint64_t address,uint64_t size,uint64_t flags) {
    observer_exchange *e=opaque;observer_memory *m=e->transaction;
    if(address<BASE || address>BASE+sizeof(m->bytes) ||
       size>BASE+sizeof(m->bytes)-address || (size!=4 && size!=8) ||
       (flags!=0 && flags!=8 && flags!=9)) {m->bad=1;return 1;}
    if(flags==9) {++m->writes;__builtin_memcpy(m->bytes+address-BASE,&e->value,size);}
    else {++m->reads;e->value=0;__builtin_memcpy(&e->value,m->bytes+address-BASE,size);}
    return 0;
}
uint64_t VALIDATE(uint8_t *context,uint8_t *raw,uint64_t address,
    neverd_x64_paged_body_v1 validate,uint64_t paging) {
    observer_memory *m=(observer_memory *)context;
    m->progress=m->transfer=0;return validate(raw,0);
}
uint64_t STEP(uint8_t *context,uint8_t *raw,uint64_t address,const uint8_t *bytes,
    uint64_t size,neverd_x64_paged_body_v1 body,neverd_x64_paged_body_v1 validate,
    uint64_t mxcsr,uint64_t paging SHADOW_PARAMETER) {
    observer_memory *m=(observer_memory *)context;
    neverd_x64_state_v6 *s=(neverd_x64_state_v6 *)raw;
    m->progress=m->transfer=0;++m->steps;
    uint64_t status=validate(raw,0);if(status)return status;
    observer_exchange e={0,0,m};status=body(s,&e);m->progress=status<=3;
    if(status==1 || status==3) {
        m->transfer=status;m->instruction=address;
        m->pc=s->PC;m->sp=s->GPR[4];m->rax=s->GPR[0];
    }
    return status;
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return ((observer_memory *)context)->progress;
}
#if DEBUG
uint64_t neverd_x64_paged_transfer_v1(uint8_t *context,uint64_t *instruction,
    uint64_t *pc,uint64_t *sp,uint64_t *rax) {
    observer_memory *m=(observer_memory *)context;if(!m->transfer)return 0;
    if(instruction)*instruction=m->instruction;if(pc)*pc=m->pc;
    if(sp)*sp=m->sp;if(rax)*rax=m->rax;return m->transfer;
}
#endif
uint64_t neverd_x64_paged_event_v1(uint8_t *context,uint8_t *raw,
    const uint8_t *event,uint64_t mxcsr) {return 5;}
#define RAM memory.bytes
#define MEMORY_ARGUMENT (uint8_t *)&memory,
#else
extern uint8_t scheduled_test_stack[256];
#define RAM scheduled_test_stack
#define MEMORY_ARGUMENT
#endif
typedef struct {unsigned polls,accepted,environment;} observer_producer;
#if SCHEDULED
static int observer_peek(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *shadows,
    neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    ++((observer_producer *)opaque)->polls;return 0;
}
static void observer_accept(const neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    ++((observer_producer *)opaque)->accepted;
}
#define SHADOWS neverd_x64_interruptibility_v1 shadows={0}
#define ENV_BUDGET
#define EXTRA_ARGUMENTS ,&shadows,&poll_budget,observer_peek,observer_accept,&producer
#elif PAGED
#define SHADOWS
#define ENV_BUDGET
#define EXTRA_ARGUMENTS
#else
static int observer_environment(neverd_x64_state_v6 *s,uint64_t status,void *opaque) {
    ++((observer_producer *)opaque)->environment;return 0;
}
#define SHADOWS
#define ENV_BUDGET &environment_budget,
#define EXTRA_ARGUMENTS ,observer_environment,&producer
#endif
static void observer_write(uint8_t *ram,uint64_t address,uint64_t value) {
    __builtin_memcpy(ram+address-BASE,&value,8);
}
static uint64_t observer_read(const uint8_t *ram,uint64_t address) {
    uint64_t value;__builtin_memcpy(&value,ram+address-BASE,8);return value;
}
static int observer_slot_matches(const uint8_t *ram,uint64_t value) {
    // Check halves separately: do not require a wide-load forwarding proof
    // across the original partial store in externally bound fixture RAM.
    uint32_t low,high;
    __builtin_memcpy(&low,ram+SLOT-BASE,4);
    __builtin_memcpy(&high,ram+SLOT-BASE+4,4);
    return low==(uint32_t)value && high==(uint32_t)(value>>32);
}
#define PREPARE() \
    observer_memory memory={0};observer_producer producer={0};SHADOWS; \
    neverd_x64_state_v6 state=observer_seed(low,saved); \
    CALL_CONTEXT root={0};SITE_CONTEXT site={0}; \
    uint64_t budget=64,poll_budget=64,environment_budget=64; \
    observer_write(RAM,STACK,0x9000);observer_write(RAM,SLOT,incoming)
#define ROOT_START() START_ROOT(&state,MEMORY_ARGUMENT &budget,ENV_BUDGET &root EXTRA_ARGUMENTS,low)
#define ROOT_RESUME() RESUME_ROOT(&state,MEMORY_ARGUMENT &budget,ENV_BUDGET &root EXTRA_ARGUMENTS)
#define SITE_START() START_SITE(&state,MEMORY_ARGUMENT &budget,ENV_BUDGET &site EXTRA_ARGUMENTS)
#define SITE_RESUME() RESUME_SITE(&state,MEMORY_ARGUMENT &budget,ENV_BUDGET &site EXTRA_ARGUMENTS)
#define START_TO_PAUSE() \
    neverd_x64_call_result_v1 r=ROOT_START(); \
    if(r.Status!=21 || r.Completed || r.Value || budget!=60 || \
       state.PC!=0x100e || state.GPR[4]!=STACK-56 || state.GPR[7]!=SLOT || \
       !observer_slot_matches(RAM,(incoming&0xffffffff00000000ULL)|low) || \
       root.Function.Phase!=1)return 1
#define FINISH_PARENT(Expected) \
    if(r.Status!=3 || r.Completed!=1 || r.Value!=(Expected) || \
       state.PC!=0x1013 || state.GPR[4]!=STACK-56)return 1; \
    r=ROOT_RESUME(); \
    if(r.Status!=3 || r.Completed!=1 || r.Value!=(Expected) || \
       state.PC!=0x9000 || state.GPR[4]!=STACK+8 || state.GPR[7]!=saved || \
       root.Function.Phase!=2 || site.Callee.Function.Phase!=2 || \
       observer_read(RAM,STACK)!=0x9000 || observer_read(RAM,STACK-8)!=saved || \
       observer_read(RAM,STACK-64)!=0x1013 || producer.accepted || \
       producer.environment || memory.bad)return 1
int proof_partial_store_and_spilled_pointer(uint64_t incoming,uint32_t low,uint64_t saved) {
    PREPARE();START_TO_PAUSE();r=SITE_START();FINISH_PARENT(incoming>>32);
    return budget!=49 || state.GPR[1]!=(0xfeedface00000000ULL|low) ||
        !observer_slot_matches(RAM,(incoming&0xffffffff00000000ULL)|low) ||
        observer_read(RAM,STACK-72)!=SLOT
#if PAGED
        || memory.steps!=15 || memory.reads!=6 || memory.writes!=4
#endif
#if SCHEDULED
        || producer.polls!=15 || poll_budget!=49
#endif
        ;
}
int proof_live_high_word_after_pause(uint64_t incoming,uint32_t low,uint64_t saved,
                                   uint32_t replacement) {
    PREPARE();START_TO_PAUSE();budget=3;r=SITE_START();
    if(r.Status!=9 || r.Completed || budget || state.PC!=0x1025 ||
       state.GPR[4]!=STACK-104 || observer_read(RAM,STACK-72)!=SLOT ||
       site.Callee.Function.Phase!=1)return 1;
    observer_write(RAM,SLOT,((uint64_t)replacement<<32)|low);
    budget=8;r=SITE_RESUME();FINISH_PARENT(replacement);
    return budget || !observer_slot_matches(RAM,((uint64_t)replacement<<32)|low);
}
int proof_live_saved_pointer_after_pause(uint64_t incoming,uint32_t low,uint64_t saved,
                                      uint64_t alternate) {
    PREPARE();START_TO_PAUSE();budget=3;r=SITE_START();
    if(r.Status!=9 || r.Completed || budget || state.PC!=0x1025 ||
       observer_read(RAM,STACK-72)!=SLOT)return 1;
    observer_write(RAM,STACK-48,alternate);
    observer_write(RAM,STACK-72,STACK-48);
    budget=8;r=SITE_RESUME();FINISH_PARENT(alternate>>32);
    return budget || !observer_slot_matches(RAM,(incoming&0xffffffff00000000ULL)|low) ||
        observer_read(RAM,STACK-72)!=STACK-48 || observer_read(RAM,STACK-48)!=alternate;
}
int proof_preserved_input_cached_result(uint64_t incoming,uint32_t low,uint64_t saved) {
    PREPARE();START_TO_PAUSE();r=SITE_START();FINISH_PARENT(incoming>>32);
    observer_write(RAM,SLOT,~incoming);state.GPR[0]=~(incoming>>32);
    budget=poll_budget=environment_budget=0;r=ROOT_RESUME();
    return r.Status!=3 || r.Completed!=1 || r.Value!=(incoming>>32) ||
        budget || poll_budget || environment_budget || producer.environment;
}
)C";
