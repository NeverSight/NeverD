//===- X64Cr8WriteNotificationTests.inc - Completed CR8 write binding ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove actual generated instruction/scheduler effects with an independent
/// producer. Compile and fold only; never execute generated code or callbacks.
//===----------------------------------------------------------------------===//

class X64Cr8WriteNotificationTest : public X64ScheduledCallTest {
protected:
  void loadPriority() {
    std::vector<uint8_t> Code(0x42, 0x90);
    const uint8_t Body[] = {0x44, 0x0f, 0x22, 0xc0, // MOV CR8,RAX.
                            0x44, 0x0f, 0x20, 0xc3, // MOV RBX,CR8.
                            0x44, 0x0f, 0x22, 0xc1, // MOV CR8,RCX.
                            0x90, 0xc3};
    std::copy(std::begin(Body), std::end(Body), Code.begin());
    Code[0x10] = 0x0f;
    Code[0x11] = 0x0b; // UD2.
    const uint8_t Call[] = {0xe8, 0xdb, 0xff, 0xff, 0xff, 0xc3};
    std::copy(std::begin(Call), std::end(Call), Code.begin() + 0x20);
    Code[0x30] = 0x0f;
    Code[0x31] = 0xa2; // CPUID stays a native request.
    Code[0x40] = 0x48;
    Code[0x41] = 0xcf; // Original IRETQ after event delivery.
    load(Code,
         {{0x1000, 14}, {0x1010, 2}, {0x1020, 6}, {0x1030, 2}, {0x1040, 2}});
  }

  static std::string selection(llvm::StringRef Extra = "") {
    return request(
        (R"("entries":[4096,4112,4128,4144,4160],"follow_internal_calls":true,"event_delivery":true,"scheduled_events":true)" +
         Extra)
            .str());
  }
};

TEST_F(X64Cr8WriteNotificationTest, ExplicitContractAndDefaultIdentity) {
  loadPriority();
  for (bool LLVM : {false, true}) {
    const auto Default = source(selection(), LLVM);
    const auto Disabled =
        source(selection(R"(,"notify_cr8_writes":false)"), LLVM);
    EXPECT_EQ(Default.first, Disabled.first);
    EXPECT_EQ(Default.first.find("neverd_x64_cr8_written_v1"),
              std::string::npos);
    auto DefaultManifest = llvm::json::parse(Default.second);
    auto DisabledManifest = llvm::json::parse(Disabled.second);
    ASSERT_TRUE(bool(DefaultManifest));
    ASSERT_TRUE(bool(DisabledManifest));
    DisabledManifest->getAsObject()->getObject("request")->erase(
        "notify_cr8_writes");
    EXPECT_TRUE(*DefaultManifest == *DisabledManifest);
    for (const char *Dispatch : {"switch", "direct"}) {
      const auto [Source, Report] =
          source(selection((R"(,"notify_cr8_writes":true,"dispatch":")" +
                            llvm::Twine(Dispatch) + "\"")
                               .str()),
                 LLVM);
      auto Manifest = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Manifest));
      EXPECT_TRUE(*Manifest->getAsObject()->get("units") ==
                  *DefaultManifest->getAsObject()->get("units"));
      EXPECT_EQ(
          Manifest->getAsObject()->getInteger("lowered_operations"),
          DefaultManifest->getAsObject()->getInteger("lowered_operations"));
      const auto *Contract =
          Manifest->getAsObject()->getObject("cr8_write_notification");
      ASSERT_NE(Contract, nullptr);
      EXPECT_EQ(Contract->getString("symbol"), "neverd_x64_cr8_written_v1");
      const auto *Sites = Contract->getArray("instructions");
      ASSERT_NE(Sites, nullptr);
      ASSERT_EQ(Sites->size(), 2u);
      EXPECT_EQ((*Sites)[0].getAsString(), "0x1000");
      EXPECT_EQ((*Sites)[1].getAsString(), "0x1008");
      const auto File = tmpFile("cr8-notification.c");
      std::ofstream(File) << Source;
      for (const char *Level : {"-O0", "-O2"}) {
        const auto Built = exec(
            NEVERD_TEST_CLANG,
            {"-std=c11", Level, "-Werror=implicit-function-declaration",
             "-Werror=incompatible-pointer-types", "-Werror=return-type", "-c",
             File.string(), "-o", tmpFile("cr8-notification.o").string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
      }
    }
  }
}

TEST_F(X64Cr8WriteNotificationTest, RejectsUnsupportedBindingsAtomically) {
  loadPriority();
  for (bool LLVM : {false, true})
    for (
        const auto &Bad :
        {selection(R"(,"notify_cr8_writes":1)"),
         selection(R"(,"notify_cr8_writes":null)"),
         request(R"("instructions":[4096],"notify_cr8_writes":true)"),
         request(
             R"("instructions":[4096],"event_delivery":true,"notify_cr8_writes":true)"),
         request(
             R"("instructions":[4096],"paged_memory":true,"scheduled_events":true,"notify_cr8_writes":true)")}) {
      const char *Report = "old";
      EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(),
                                                  Bad.size(), LLVM, &Report),
                nullptr);
      EXPECT_EQ(Report, nullptr);
      EXPECT_TRUE(llvm::StringRef(take(neverd_last_error(Session)))
                      .contains_insensitive("cr8"));
    }
}

const char Cr8NotificationProbes[] = R"C(
/* Independent TPR oracle: import/read retain the byte, every completed write
 * replaces all eight bits. Peek tests coherence before selecting an event. */
typedef struct {
    unsigned polls,writes,acks,bad; int ready; uint8_t tpr,seen_tpr;
} producer;
void neverd_x64_cr8_written_v1(uint64_t value,void *opaque) {
    producer *p=opaque;++p->writes;
    if(value>15)p->bad=1;
    p->tpr=(uint8_t)(value<<4);
}
static int peek_event(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *shadows,
    neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    producer *p=opaque;++p->polls;p->seen_tpr=p->tpr;
    if(s->CR8!=(p->tpr>>4))p->bad=1;
    event->Kind=2;event->Vector=65;return p->ready;
}
static void accept_event(const neverd_x64_pending_interrupt_v1 *event,void *opaque) {
    producer *p=opaque;++p->acks;p->ready=0;
}
static neverd_x64_state_v6 seed(void) {
    neverd_x64_state_v6 s={0};s.PC=0x1000;s.RFlags=0x246;s.CR8=3;
    s.MXCSR=0x1f80;s.NMIBlocked=1;s.GPR[0]=3;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};s.DS=s.ES=s.SS;
    s.FS=(neverd_x64_segment_metadata){0xffffffff,16,0xc093};s.GS=s.FS;
    s.LDTR=(neverd_x64_segment){0,63,40,0x82};
    s.TR=(neverd_x64_segment){0,103,48,0x8b};
    s.STAR=0x0023001000000000ULL;return s;
}
#define PREPARE() neverd_x64_state_v6 state=seed(); \
    neverd_x64_interruptibility_v1 shadows={0}; \
    uint64_t budget=1,poll_budget=8;producer events={0,0,0,0,0,0x3b,0}
#define EVENTS &shadows,&poll_budget,peek_event,accept_event,&events
#define RUN() neverd_x64_continue_events_v6(&state,&budget,EVENTS)
#define BAD(P,W,A,T) (events.polls!=(P)||events.writes!=(W)||events.acks!=(A)|| \
                     events.tpr!=(T)||events.bad)
int proof_same_value_last_budget(void) {
    PREPARE();return RUN()!=9||budget||poll_budget!=7||state.PC!=0x1004||
        state.CR8!=3||BAD(1,1,0,0x30);
}
int proof_legal_value(uint64_t value) {
    PREPARE();state.GPR[0]=value&15;
    return RUN()!=9||budget||state.PC!=0x1004||state.CR8!=(value&15)||
        BAD(1,1,0,(value&15)<<4);
}
int proof_invalid_write(uint64_t value) {
    if(value<=15)return 0;
    PREPARE();state.GPR[0]=value;neverd_x64_state_v6 before=state;
    return RUN()!=5||budget!=1||poll_budget!=7||BAD(1,0,0,0x3b)||
        __builtin_memcmp(&state,&before,sizeof(state));
}
int proof_invalid_entry(uint64_t value) {
    if(value<=15)return 0;
    PREPARE();state.CR8=value;
    return RUN()!=5||budget!=1||poll_budget!=8||state.PC!=0x1000||BAD(0,0,0,0x3b);
}
int proof_wrong_privilege(void) {
    PREPARE();state.CS.Selector=11;state.CS.Attributes=0xa0fb;
    return RUN()!=5||budget!=1||poll_budget!=8||state.PC!=0x1000||BAD(0,0,0,0x3b);
}
int proof_bad_shadows(void) {
    PREPARE();shadows.SSBlocked=2;
    return RUN()!=5||budget!=1||poll_budget!=8||state.PC!=0x1000||BAD(0,0,0,0x3b);
}
int proof_no_instruction_budget(void) {
    PREPARE();budget=0;return RUN()!=9||poll_budget!=8||state.PC!=0x1000||BAD(0,0,0,0x3b);
}
int proof_no_poll_budget(void) {
    PREPARE();poll_budget=0;return RUN()!=16||budget!=1||state.PC!=0x1000||BAD(0,0,0,0x3b);
}
int proof_invalid_producer(void) {
    PREPARE();events.ready=2;
    return RUN()!=5||budget!=1||poll_budget!=7||state.PC!=0x1000||BAD(1,0,0,0x3b);
}
int proof_unselected(void) {
    PREPARE();state.PC=0x9000;return RUN()!=8||budget!=1||poll_budget!=8||BAD(0,0,0,0x3b);
}
int proof_native_request(void) {
    PREPARE();state.PC=0x1030;return RUN()!=4||budget!=1||state.PC!=0x1030||BAD(1,0,0,0x3b);
}
int proof_read_only(void) {
    PREPARE();state.PC=0x1004;return RUN()!=9||budget||state.PC!=0x1008||
        state.GPR[3]!=3||state.CR8!=3||BAD(1,0,0,0x3b);
}
int proof_ordinary_instruction(void) {
    PREPARE();state.PC=0x100c;return RUN()!=9||budget||state.PC!=0x100d||BAD(1,0,0,0x3b);
}
int proof_before_next_poll(void) {
    PREPARE();budget=2;return RUN()!=9||budget||poll_budget!=6||state.PC!=0x1008||
        state.GPR[3]!=3||events.seen_tpr!=0x30||BAD(2,1,0,0x30);
}
int proof_two_writes(void) {
    PREPARE();budget=3;state.GPR[1]=0;
    return RUN()!=9||budget||poll_budget!=5||state.PC!=0x100c||state.CR8||BAD(3,2,0,0);
}
int proof_reentry_not_replay(void) {
    PREPARE();if(RUN()!=9)return 1;
    budget=1;if(RUN()!=9||BAD(2,1,0,0x30))return 2;
    budget=1;state.PC=0x1000;
    return RUN()!=9||budget||poll_budget!=5||state.PC!=0x1004||BAD(3,2,0,0x30);
}
int proof_deferred_irq(void) {
    PREPARE();state.RFlags&=~512ULL;events.ready=1;
    return RUN()!=9||budget||state.PC!=0x1004||BAD(1,1,0,0x30);
}
/* One declared nonfaulting RAM object supplies original GDT, IDT and stack.
 * The harness binds only fully contained constant operands to its provenance. */
static void descriptors(neverd_x64_state_v6 *s,unsigned vector) {
    uint64_t *gdt=(uint64_t *)(uintptr_t)0x10000;
    uint64_t *idt=(uint64_t *)(uintptr_t)0x11000;
    gdt[1]=gdt[3]=0x00af9b000000ffffULL;gdt[2]=0x00cf93000000ffffULL;
    idt[vector*2]=0x1040ULL|(24ULL<<16)|(0x8eULL<<40);idt[vector*2+1]=0;
    s->GDTR.Base=0x10000;s->GDTR.Limit=63;s->IDTR.Base=0x11000;s->IDTR.Limit=4095;
    s->GPR[4]=0x14000;
}
int proof_accepted_irq_preempts_write(void) {
    PREPARE();events.ready=1;descriptors(&state,65);
    return RUN()!=7||budget!=1||poll_budget!=7||state.PC!=0x1040||
        state.GPR[4]!=0x13fd8||state.CR8!=3||BAD(1,0,1,0x3b);
}
int proof_original_iret_then_write(void) {
    PREPARE();events.ready=1;descriptors(&state,65);
    if(RUN()!=7||budget!=1||state.PC!=0x1040||BAD(1,0,1,0x3b))return 1;
    if(RUN()!=6||budget||state.PC!=0x1000||state.GPR[4]!=0x14000||
       BAD(2,0,1,0x3b))return 2;
    budget=1;return RUN()!=9||budget||poll_budget!=5||state.PC!=0x1004||
        state.GPR[4]!=0x14000||BAD(3,1,1,0x30);
}
int proof_delivered_exception(void) {
    PREPARE();state.PC=0x1010;descriptors(&state,6);
    return RUN()!=7||budget||poll_budget!=7||state.PC!=0x1040||
        state.GPR[4]!=0x13fd8||state.CR8!=3||BAD(1,0,0,0x3b);
}
)C";

const char Cr8NotificationCallProbes[] = R"C(
#define STACK 0x13008ULL
#define CELLS() uint64_t *cells=(uint64_t *)(uintptr_t)(STACK-8); \
    cells[0]=0xdeadbeef;cells[1]=0x9000;state.GPR[4]=STACK
#define SITE() neverd_x64_call_events_site_1020_v6(&state,&budget,&site,EVENTS,0)
#define RESUME_SITE() neverd_x64_resume_events_site_1020_v6(&state,&budget,&site,EVENTS)
#define ENTRY() neverd_x64_call_events_1000_v6(&state,&budget,&call,EVENTS,0)
#define RESUME_ENTRY() neverd_x64_resume_events_1000_v6(&state,&budget,&call,EVENTS)
int proof_checked_entry_resumption(void) {
    PREPARE();CELLS();neverd_x64_call_context_v1 call={0};
    neverd_x64_call_result_v1 a=ENTRY();
    if(a.Status!=9||a.Completed||state.PC!=0x1004||BAD(1,1,0,0x30))return 1;
    budget=4;a=RESUME_ENTRY();
    if(a.Status!=3||!a.Completed||a.Value!=3||budget||state.PC!=0x9000||
       state.GPR[4]!=STACK+8||BAD(5,2,0,0))return 2;
    a=RESUME_ENTRY();return a.Status!=3||!a.Completed||a.Value!=3||BAD(5,2,0,0);
}
int proof_original_call_resumption(void) {
    PREPARE();CELLS();state.PC=0x1020;neverd_x64_call_site_context_v1 site={0};
    neverd_x64_call_result_v1 a=SITE();
    if(a.Status!=9||a.Completed||state.PC!=0x1000||cells[0]!=0x1025||
       state.GPR[4]!=STACK-8||BAD(1,0,0,0x3b))return 1;
    budget=1;a=RESUME_SITE();
    if(a.Status!=9||a.Completed||state.PC!=0x1004||BAD(2,1,0,0x30))return 2;
    budget=4;a=RESUME_SITE();
    if(a.Status!=3||!a.Completed||a.Value!=3||budget||state.PC!=0x1025||
       state.GPR[4]!=STACK||cells[0]!=0x1025||BAD(6,2,0,0))return 3;
    a=RESUME_SITE();return a.Status!=3||!a.Completed||a.Value!=3||BAD(6,2,0,0);
}
int proof_function_context(void) {
    PREPARE();CELLS();budget=5;neverd_x64_function_context_v1 function={0};
    if(!neverd_x64_begin_function_v6(&state,&function))return 1;
    return neverd_x64_continue_events_function_v6(&state,&budget,EVENTS,&function)!=3||
        budget||state.PC!=0x9000||function.Phase!=2||state.GPR[4]!=STACK+8||BAD(5,2,0,0);
}
)C";

TEST_F(X64Cr8WriteNotificationTest, ProvesCompletionAndCallComposition) {
  if (!neverd::test::compilerReadsCurrentIR(NEVERD_TEST_CLANG))
    GTEST_SKIP() << "this Clang cannot read the linked LLVM's IR";
  loadPriority();
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(Dispatch);
      const auto [Source, Report] = source(
          selection(
              (R"(,"notify_cr8_writes":true,"dispatch":")" +
               llvm::Twine(Dispatch) +
               R"(","function_context":true,"pause_call_sites":true,"function_calls":[{"entry":4096,"abi":"win64","argument_bytes":[8],"result_bytes":8,"call_sites":[4128]}])")
                  .str()),
          LLVM);
      provePredicates(false,
                      std::string("cr8-notification-") +
                          (LLVM ? "llvm-" : "high-") + Dispatch,
                      Source,
                      std::string(Cr8NotificationProbes) +
                          Cr8NotificationCallProbes,
                      23, 0x4000, 4, /*CloseHelpers=*/true,
                      /*InlineEventControllers=*/true);
    }
}
