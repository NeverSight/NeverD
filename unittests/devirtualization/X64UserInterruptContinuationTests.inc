//===- X64UserInterruptContinuationTests.inc - UINTR state and frames ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp for both source routes.
//===----------------------------------------------------------------------===//

namespace {
X64ContinuationSourceOptions userInterruptOptions(bool RestoresUIF = false) {
  auto O = fastSystemOptions();
  O.Profile = X64ContinuationProfile::UserInterruptNoFaultV8;
  O.UiretRestoresUIF = RestoresUIF;
  return O;
}

const char UserInterruptHarness[] = R"C(
static user_interrupt_state_t user_seed(uint64_t memory,unsigned cpl) {
  user_interrupt_state_t s={0};s.core=fast_seed(memory);
  s.core.core.cs.selector|=cpl;s.core.core.cs.attributes|=cpl<<5;
  s.core.core.ss.selector|=cpl;s.core.core.ss.attributes|=cpl<<5;
  s.uif=1;s.monitor_active=1;memset(s.reserved,0x93,sizeof(s.reserved));
  return s;
}
)C";
} // namespace

TEST_F(X64ContinuationSourceTest, UserInterruptReturnRestoresExactState) {
  for (unsigned Bits : {48u, 57u})
    for (bool Enhanced : {false, true}) {
      SCOPED_TRACE(Bits);
      SCOPED_TRACE(Enhanced);
      auto O = userInterruptOptions(Enhanced);
      O.PagingAddressBits = O.SysenterMSRAddressBits = Bits;
      std::string Harness =
          std::string(EventHarness) + FastSystemHarness + UserInterruptHarness;
      Harness += "\n#define BITS " + std::to_string(Bits) +
                 "\n#define ENHANCED " + std::to_string(Enhanced) + "\n";
      Harness += R"C(
int main(void) {
  const uint64_t pcs[]={0,1,0x7fffffffffffULL,0xffff800000000000ULL,
    0x800000000000ULL,0x00ffffffffffffffULL,0xff00000000000000ULL,UINT64_MAX};
  const uint64_t stacks[]={0,UINT64_MAX,0x800000000000ULL,0x123456789abcdef0ULL};
  unsigned char memory[4096],saved[4096];
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned state=0;state<4;++state)
    for(unsigned bit=0;bit<64;++bit)for(unsigned target=0;target<8;++target)
      for(unsigned stack=0;stack<4;++stack) {
        user_interrupt_state_t s=user_seed((uintptr_t)memory,cpl),want;
        event_state_t *v=&s.core.core;
        s.uif=state&1;s.monitor_active=state>>1;
        v->flags=2|(((1ULL<<((bit+9)%64))^(0x3d7fd7ULL*(stack&1)))&0x3d7fd7);
        v->gpr[4]=(uintptr_t)memory+512+((v->flags&0x40000)?0:bit%8);
        const uint64_t restored=(1ULL<<bit)^(target&1?UINT64_MAX:0);
        event_word(v->gpr[4],pcs[target]);event_word(v->gpr[4]+8,restored);
        event_word(v->gpr[4]+16,stacks[stack]);
        want=s;memcpy(saved,memory,sizeof(memory));
        const unsigned good=canonical(pcs[target],BITS);
        if(good) {
          /* Independent bit-by-bit oracle, including all ignored saved bits. */
          const unsigned changed[]={0,2,4,6,7,8,10,11,14,16,18,21};
          for(unsigned i=0;i<sizeof(changed)/sizeof(*changed);++i) {
            const uint64_t mask=1ULL<<changed[i];
            want.core.core.flags=(want.core.core.flags&~mask)|(restored&mask);
          }
          want.core.core.pc=pcs[target];want.core.core.gpr[4]=stacks[stack];
          want.uif=ENHANCED?((restored>>1)&1):1;want.monitor_active=0;
        }
        CHECK(neverd_step_v8_1000(ARG(&s))==(good?11:5));
        CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
      }
  /* Invalid entry rejects before touching even an inaccessible guest stack. */
  for(unsigned bad=0;bad<12;++bad) {
    user_interrupt_state_t s=user_seed((uintptr_t)memory,3),want;
    s.core.core.gpr[4]=1;
    if(bad==0)s.uif=2;
    if(bad==1)s.monitor_active=255;
    if(bad==2)s.core.core.pc++;
    if(bad==3)s.core.core.flags&=~2ULL;
    if(bad==4)s.core.core.flags|=1ULL<<63;
    if(bad==5)s.core.core.cs.attributes|=0x4000;
    if(bad==6)s.core.core.cs.attributes&=~0x80;
    if(bad==7)s.core.core.cs.attributes=(s.core.core.cs.attributes&~0x60)|0x40;
    if(bad==8)s.core.core.unusable|=1;
    if(bad==9)s.core.core.nmi_blocked=2;
    if(bad==10)s.core.core.cr8=16;
    if(bad==11)s.core.sysenter_cs|=1ULL<<32;
    want=s;CHECK(neverd_step_v8_1000(ARG(&s))==5&&!memcmp(&s,&want,sizeof(s)));
  }
  const uint64_t bad_stacks[]={UINT64_MAX-7,UINT64_MAX-22,
    (1ULL<<(BITS-1))-8,1ULL<<(BITS-1)};
  for(unsigned i=0;i<4;++i) {
    user_interrupt_state_t s=user_seed((uintptr_t)memory,0),want;
    s.core.core.gpr[4]=bad_stacks[i];want=s;
    CHECK(neverd_step_v8_1000(ARG(&s))==5&&!memcmp(&s,&want,sizeof(s)));
  }
  return 0;
}
)C";
      run({0xf3, 0x0f, 0x01, 0xec}, {0x1000}, Harness, false, O);
    }
}

TEST_F(X64ContinuationSourceTest, UserInterruptFlagOperationsShareOneLiveCell) {
  run({0xf3, 0x0f, 0x01, 0xee, 0xf3, 0x0f, 0x01, 0xef, 0xf3, 0x0f, 0x01, 0xed},
      {0x1000, 0x1004, 0x1008},
      std::string(EventHarness) + FastSystemHarness + UserInterruptHarness +
          R"C(
int main(void) {
  unsigned char memory[4096],saved[4096];
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned dpl=0;dpl<4;++dpl)
    for(unsigned conforming=0;conforming<2;++conforming)
      for(unsigned uif=0;uif<2;++uif)for(unsigned flags=0;flags<64;++flags) {
        user_interrupt_state_t s=user_seed((uintptr_t)memory,cpl),want;
        event_state_t *v=&s.core.core; s.uif=uif;
        v->cs.attributes=(v->cs.attributes&~0x64)|(dpl<<5)|(conforming<<2);
        v->flags=0x3d7602|((flags&1)?1:0)|((flags&2)?4:0)|((flags&4)?16:0)|
          ((flags&8)?64:0)|((flags&16)?128:0)|((flags&32)?2048:0);
        v->gpr[4]=UINT64_MAX;want=s;memcpy(saved,memory,sizeof(memory));
        const unsigned good=conforming?dpl<=cpl:dpl==cpl;
        for(unsigned step=0;step<3;++step) {
          v->pc=0x1000+step*4;s.uif=uif;v->flags|=0x10000;want=s;
          if(good) {
            want.core.core.pc+=4;
            want.core.core.flags&=~0x10000ULL;
            if(step==0)want.uif=0;
            if(step==1)want.uif=1;
            if(step==2) {
              const unsigned arithmetic[]={0,2,4,6,7,11};
              for(unsigned i=0;i<6;++i)want.core.core.flags&=~(1ULL<<arithmetic[i]);
              want.core.core.flags|=s.uif;
            }
          }
          uint64_t result=step==0?neverd_step_v8_1000(ARG(&s)):
            step==1?neverd_step_v8_1004(ARG(&s)):neverd_step_v8_1008(ARG(&s));
          CHECK(result==(good?0:5)&&!memcmp(&s,&want,sizeof(s)));
          CHECK(!memcmp(memory,saved,sizeof(memory)));
        }
      }
  return 0;
}
)C",
      false, userInterruptOptions());
}

TEST_F(X64ContinuationSourceTest, UserInterruptReturnReadsExactlyThreeWords) {
#if !defined(__linux__) && !defined(__APPLE__)
  GTEST_SKIP() << "guard-page fixture requires mmap";
#else
  run({0xf3, 0x0f, 0x01, 0xec}, {0x1000},
      std::string(EventHarness) + FastSystemHarness + UserInterruptHarness +
          R"C(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  const long page=sysconf(_SC_PAGESIZE);CHECK(page>24);
  unsigned char *p=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
  CHECK(p!=MAP_FAILED&&mprotect(p+page,page,PROT_READ|PROT_WRITE)==0);
  unsigned char memory[4096];
  for(unsigned end=0;end<2;++end) {
    uint64_t *frame=(uint64_t *)(p+page+(end?page-24:0));
    frame[0]=0x1234;frame[1]=0x10003;frame[2]=(uintptr_t)(p+page*2);
    user_interrupt_state_t s=user_seed((uintptr_t)memory,3),want;
    s.core.core.gpr[4]=(uintptr_t)frame;want=s;
    want.core.core.pc=0x1234;want.core.core.flags=0x10203;
    want.core.core.gpr[4]=frame[2];want.monitor_active=0;
    CHECK(neverd_step_v8_1000(ARG(&s))==11&&!memcmp(&s,&want,sizeof(s)));
  }
  CHECK(munmap(p,page*3)==0);return 0;
}
)C",
      false, userInterruptOptions());
#endif
}

TEST_F(X64ContinuationSourceTest, UserInterruptFlagStepsRejectDebugDelivery) {
  run({0xf3, 0x0f, 0x01, 0xee, 0xf3, 0x0f, 0x01, 0xef, 0xf3, 0x0f, 0x01, 0xed},
      {0x1000, 0x1004, 0x1008},
      std::string(EventHarness) + FastSystemHarness + UserInterruptHarness +
          R"C(
int main(void) {
  unsigned char memory[4096];
  for(unsigned step=0;step<3;++step) {
    user_interrupt_state_t s=user_seed((uintptr_t)memory,0),want;
    s.core.core.pc=0x1000+step*4;s.core.core.flags|=0x100;s.core.core.gpr[4]=1;
    want=s;
    const uint64_t result=step==0?neverd_step_v8_1000(ARG(&s)):
      step==1?neverd_step_v8_1004(ARG(&s)):neverd_step_v8_1008(ARG(&s));
    CHECK(result==5&&!memcmp(&s,&want,sizeof(s)));
  }
  return 0;
}
)C",
      false, userInterruptOptions());
}

TEST_F(X64ContinuationSourceTest, UserInterruptProfileRequiresExplicitFeature) {
  auto O = userInterruptOptions();
  O.UiretRestoresUIF.reset();
  auto Missing =
      buildX64ContinuationSource(image({0xf3, 0x0f, 0x01, 0xec}), {0x1000}, O);
  ASSERT_FALSE(bool(Missing));
  EXPECT_NE(llvm::toString(Missing.takeError()).find("UIRET_UIF"),
            std::string::npos);
  O = fastSystemOptions();
  O.UiretRestoresUIF = false;
  auto Old = buildX64ContinuationSource(image({0x90}), {0x1000}, O);
  ASSERT_FALSE(bool(Old));
  llvm::consumeError(Old.takeError());
  for (auto Earlier : {options(), eventOptions(), fastSystemOptions()}) {
    auto Units = buildX64ContinuationSource(image({0xf3, 0x0f, 0x01, 0xec}),
                                            {0x1000}, Earlier);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
}
