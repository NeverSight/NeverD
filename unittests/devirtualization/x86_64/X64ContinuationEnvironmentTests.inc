//===- X64ContinuationEnvironmentTests.inc - Complete state resumption ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent state/memory oracles for explicit environment handoffs.
/// Included beside the shared SDK/CLI fixture, without a second image loader.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, EnvironmentResumesTheActualPhysicalContext) {
  load({0xe8, 0x1b, 0, 0, 0,          // call 1020 (outside selection)
        0x48, 0x8b, 0x44, 0x24, 0x08, // mov rax,[rsp+8]
        0xc3});                       // ret
  for (bool LLVM : {false, true})
    for (bool Direct : {false, true})
      for (bool Follow : {false, true}) {
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Direct);
        SCOPED_TRACE(Follow);
        const auto Request = request(
            std::string(R"("instructions":[4096,4101,4106],"dispatch":")") +
            (Direct ? "direct" : "switch") + R"(","follow_internal_calls":)" +
            (Follow ? "true" : "false"));
        auto [Source, Manifest] = source(Request, LLVM);
        auto Parsed = llvm::json::parse(Manifest);
        ASSERT_TRUE(bool(Parsed));
        EXPECT_EQ(Parsed->getAsObject()->getString("environment_driver"),
                  "neverd_x64_run_v6");
        execute(Source, std::string(Seed) + R"C(
struct environment {
    uint64_t *original, *replacement;
    unsigned calls;
    neverd_x64_state_v6 resumed;
};
static int provide(neverd_x64_state_v6 *s, uint64_t status, void *opaque) {
    struct environment *e=opaque;
    ++e->calls;
    if (status==3) {
        CHECK(s->PC==0x9999);
        return 0;
    }
    CHECK(status==1 && s->PC==0x1020 && e->calls==1);
    CHECK(s->GPR[4]==(uintptr_t)&e->original[3]);
    CHECK(e->original[3]==0x1005);
    // A provider owns actual machine inputs, including a full-width stack
    // word absent from an ordinary call signature. It may replace the stack
    // and clobber any represented register. No host ABI is inferred here.
    e->replacement[4]=0x9999;
    e->replacement[5]=e->original[9]^s->GPR[7];
    s->PC=e->original[3];s->GPR[4]=(uintptr_t)&e->replacement[4];
    s->GPR[9]=0xfedcba9876543210ULL;s->RFlags=0x202;s->MXCSR=0x1f80;
    for(unsigned i=0;i<sizeof(s->ZMM[7]);++i)s->ZMM[7][i]^=0xa5;
    e->resumed=*s;
    return 1;
}
int main(void) {
    const uint64_t inputs[]={0,1,0x80000000ULL,0xffffffffULL,
      0x1234567880000000ULL,0xffffffffffffffffULL};
    for(unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        uint64_t a[16],b[16],expected_a[16],expected_b[16];
        for(unsigned j=0;j<16;++j)a[j]=j*71+5,b[j]=j*31+7;
        a[4]=0x8888;a[9]=inputs[i];
        memcpy(expected_a,a,sizeof(a));memcpy(expected_b,b,sizeof(b));
        neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&a[4];
        struct environment e={.original=a,.replacement=b};
        uint64_t budget=16,handoffs=3;
        CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==3);
        CHECK(budget==13 && handoffs==1 && e.calls==2);
        neverd_x64_state_v6 expected=e.resumed;
        expected.GPR[0]=inputs[i]^expected.GPR[7];
        expected.GPR[4]=(uintptr_t)&b[5];expected.PC=0x9999;
        CHECK(!memcmp(&s,&expected,sizeof(s)));
        expected_a[3]=0x1005;expected_b[4]=0x9999;
        expected_b[5]=expected.GPR[0];
        CHECK(!memcmp(a,expected_a,sizeof(a)));
        CHECK(!memcmp(b,expected_b,sizeof(b)));
    }
    return 0;
}
)C");
      }
}

TEST_F(X64ContinuationExportTest, EnvironmentBudgetsBoundEveryProviderAttempt) {
  load({0xe8, 0x1b, 0, 0, 0, // call 1020
        0xc3,                // ret
        0x0f, 0x33});        // rdpmc: native request
  for (bool LLVM : {false, true})
    for (bool Direct : {false, true}) {
      auto [Source, Manifest] = source(
          request(
              std::string(R"("instructions":[4096,4101,4102],"dispatch":")") +
              (Direct ? "direct" : "switch") + '"'),
          LLVM);
      execute(Source, std::string(Seed) + R"C(
struct environment {unsigned calls;uint64_t status;int resume,invalidate;};
static int provide(neverd_x64_state_v6 *s, uint64_t status, void *opaque) {
    struct environment *e=opaque;
    ++e->calls;e->status=status;
    if(e->invalidate) {s->PC=0x1005;s->RFlags|=1u<<17;}
    return e->resume;
}
int main(void) {
    uint64_t stack[16]={0},budget,handoffs;
    neverd_x64_state_v6 s,saved;
    struct environment e={0};
    // A native request executes no translated instruction. A provider that
    // asks to resume without changing PC still cannot loop without bound.
    s=seed();s.PC=0x1006;saved=s;budget=7;handoffs=4;e.resume=1;
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==4);
    CHECK(e.calls==4 && e.status==4 && !handoffs && budget==7);
    CHECK(!memcmp(&s,&saved,sizeof(s)));
    // Declining consumes one attempt. Null providers and empty environment
    // budgets retain the original stop without invoking anything.
    e=(struct environment){0};handoffs=3;
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==4);
    CHECK(e.calls==1 && handoffs==2 && budget==7);
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,0,0)==4 && handoffs==2);
    handoffs=0;
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==4 && e.calls==1);
    CHECK(!memcmp(&s,&saved,sizeof(s)));
    // Out-of-selection dispatch uses the same independent attempt budget.
    s.PC=0x7777;saved=s;handoffs=2;e=(struct environment){.resume=1};
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==8);
    CHECK(e.calls==2 && e.status==8 && !handoffs && budget==7);
    CHECK(!memcmp(&s,&saved,sizeof(s)));
    // A provider cannot bypass the selected instruction's invocation checks.
    // Its own changes remain visible when the next step refuses that state.
    s=seed();s.PC=0x1006;s.GPR[4]=(uintptr_t)&stack[4];saved=s;
    saved.PC=0x1005;saved.RFlags|=1u<<17;
    e=(struct environment){.resume=1,.invalidate=1};handoffs=3;
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==5);
    CHECK(e.calls==1 && handoffs==2 && budget==7);
    CHECK(!memcmp(&s,&saved,sizeof(s)));
    // A final translated CALL retains its original boundary and stack write.
    s=seed();s.GPR[4]=(uintptr_t)&stack[4];budget=1;handoffs=5;e=(struct environment){0};
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==1);
    CHECK(!budget && handoffs==5 && !e.calls && stack[3]==0x1005);
    CHECK(s.PC==0x1020 && s.GPR[4]==(uintptr_t)&stack[3]);
    saved=s;
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==9);
    CHECK(handoffs==5 && !e.calls && !memcmp(&s,&saved,sizeof(s)));
    // Invalid state never gets treated as a provider-repairable native step.
    s=seed();s.RFlags|=1u<<17;s.GPR[4]=(uintptr_t)&stack[4];saved=s;budget=9;
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==5);
    CHECK(budget==9 && handoffs==5 && !e.calls);
    CHECK(!memcmp(&s,&saved,sizeof(s)));
    return 0;
}
)C");
    }
}

TEST_F(X64ContinuationExportTest, EnvironmentReceivesCompletedPrivilegeState) {
  load({0x48, 0x0f, 0x07,          // sysretq
        0xb8, 44, 0, 0, 0, 0xc3}); // mov eax,44; ret
  for (bool LLVM : {false, true})
    for (bool Direct : {false, true}) {
      auto [Source, Manifest] = source(
          request(
              std::string(R"("instructions":[4096,4099,4104],"dispatch":")") +
              (Direct ? "direct" : "switch") + '"'),
          LLVM);
      execute(Source, std::string(Seed) + R"C(
struct environment {unsigned calls;uint64_t *stack;};
static int provide(neverd_x64_state_v6 *s,uint64_t status,void *opaque) {
    struct environment *e=opaque;++e->calls;
    if(status==3) {CHECK(s->PC==0x7777);return 0;}
    CHECK(status==6 && s->PC==0x3000 && (s->CS.Selector&3)==3);
    // Only this explicit environment establishes a new supervisor entry.
    // The generated driver must not run the selected successor on its own.
    *s=seed();s->PC=0x1003;s->GPR[4]=(uintptr_t)e->stack;
    return 1;
}
int main(void) {
    uint64_t stack[]={0x7777},budget=8,handoffs=3;
    neverd_x64_state_v6 s=seed();s.GPR[1]=0x3000;s.GPR[11]=0x202;
    s.GPR[4]=(uintptr_t)stack;
    struct environment e={.stack=stack};
    CHECK(neverd_x64_run_v6(&s,&budget,&handoffs,provide,&e)==3);
    CHECK(e.calls==2 && budget==5 && handoffs==1);
    CHECK(s.GPR[0]==44 && s.PC==0x7777 && (s.CS.Selector&3)==0);
    CHECK(s.GPR[4]==(uintptr_t)(stack+1) && stack[0]==0x7777);
    return 0;
}
)C");
    }
}
