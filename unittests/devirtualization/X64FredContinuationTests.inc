//===- X64FredContinuationTests.inc - FRED frames and event state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp for both C routes and sanitizers.
//===----------------------------------------------------------------------===//

namespace {
X64ContinuationSourceOptions fredOptions() {
  auto O = eventOptions();
  O.Profile = X64ContinuationProfile::FredReturnNoFaultV9;
  return O;
}

const char FredHarness[] = R"C(
static fred_state_t fred_seed(uint64_t memory) {
  fred_state_t s={0};s.core.core.core=event_seed(memory);
  s.core.core.sysenter_cs=0xdeadbeef;s.core.core.sysenter_esp=UINT64_MAX;
  s.core.core.sysenter_eip=0x123456789abcdef0ULL;
  s.core.uif=s.core.monitor_active=1;
  memset(s.core.reserved,0x93,sizeof(s.core.reserved));
  s.fred_config=0xffff800012340000ULL;
  memset(s.reserved,0x51,sizeof(s.reserved));
  return s;
}
static void fred_frame(fred_state_t *s,const uint64_t *words) {
  memcpy((void *)(uintptr_t)(s->core.core.core.gpr[4]+8),words,40);
}
static void fred_finish(fred_state_t *s,const uint64_t *w,unsigned user) {
  event_state_t *v=&s->core.core.core;
  s->pending_debug=((v->flags>>8)&1)||(((w[4]>>17)&1)&&((w[2]>>8)&1));
  s->sti_blocked=!user&&!s->sti_blocked&&((w[4]>>16)&1)&&((w[2]>>9)&1);
  if((w[4]>>18)&1)v->nmi_blocked=0;
  v->pc=w[0];v->flags=w[2];v->gpr[4]=w[3];
  if(user) {
    uint64_t tmp=v->gs_base;v->gs_base=v->kernel_gs;v->kernel_gs=tmp;
    s->core.monitor_active=0;
    v->unusable&=~3;
  } else {
    unsigned old=s->fred_config&3,requested=(w[1]>>16)&3;
    s->fred_config=(s->fred_config&~3ULL)|(old<requested?old:requested);
  }
}
)C";
} // namespace

TEST_F(X64ContinuationSourceTest, FredSupervisorRestoresFramesAndEventState) {
  for (unsigned Bits : {48u, 57u}) {
    auto O = fredOptions();
    O.PagingAddressBits = Bits;
    std::string Harness =
        std::string(EventHarness) + FastSystemHarness + FredHarness;
    Harness += "\n#define BITS " + std::to_string(Bits) + "\n";
    Harness += R"C(
int main(void) {
  unsigned char memory[4096],saved[4096];
  const uint64_t targets[]={0x1800,UINT64_MAX,0x800000000000ULL,0x100000000000000ULL};
  for(unsigned bit=0;bit<64;++bit)for(unsigned levels=0;levels<16;++levels)
    for(unsigned events=0;events<8;++events)for(unsigned prior=0;prior<4;++prior)
      for(unsigned nmi=0;nmi<2;++nmi) {
        fred_state_t s=fred_seed((uintptr_t)memory),want;
        event_state_t *v=&s.core.core.core;
        s.fred_config|=levels&3;s.sti_blocked=prior&1;
        v->flags=0x10002|((prior>>1)<<8);v->nmi_blocked=nmi;
        v->gpr[4]+=(bit%8);
        uint64_t w[]={targets[bit%4],v->cs.selector|((uint64_t)(levels>>2)<<16)|0x40000,
          2|(((1ULL<<bit)^(bit&1?UINT64_MAX:0))&0x3d7fd7ULL),
          0x123456789abcdef0ULL^bit,v->ss.selector|((uint64_t)events<<16)|0xdef0123400000000ULL};
        fred_frame(&s,w);want=s;memcpy(saved,memory,sizeof(memory));
        unsigned good=canonical(w[0],BITS);
        if(good)fred_finish(&want,w,0);
        CHECK(neverd_step_v9_1000(ARG(&s))==(good?12:5));
        CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
      }
  /* Every forbidden saved-flags bit rejects, including VM and missing bit 1. */
  for(unsigned bit=0;bit<64;++bit) {
    if((0x3d7fd7ULL>>bit)&1)continue;
    fred_state_t s=fred_seed((uintptr_t)memory),want=s;
    uint64_t w[]={0x1800,8,2|(1ULL<<bit),UINT64_MAX,16};
    fred_frame(&s,w);memcpy(saved,memory,sizeof(memory));
    CHECK(neverd_step_v9_1000(ARG(&s))==5&&!memcmp(&s,&want,sizeof(s)));
    CHECK(!memcmp(memory,saved,sizeof(memory)));
  }
  return 0;
}
)C";
    run({0xf2, 0x0f, 0x01, 0xca}, {0x1000}, Harness, false, O);
  }
}

TEST_F(X64ContinuationSourceTest, FredUserStandardAndDescriptorReturns) {
  for (unsigned Bits : {48u, 57u}) {
    auto O = fredOptions();
    O.PagingAddressBits = Bits;
    std::string Harness =
        std::string(EventHarness) + FastSystemHarness + FredHarness;
    Harness += "\n#define BITS " + std::to_string(Bits) + "\n";
    Harness += R"C(
int main(void) {
  unsigned char memory[4096],saved[4096];
  const uint64_t targets[]={0x1800,UINT64_MAX,0x800000000000ULL,0x100000000000000ULL};
  /* Standard returns do not consult any descriptor table, even after selector wrap. */
  for(unsigned star=3;star<65536;star+=4)for(unsigned events=0;events<8;++events) {
    fred_state_t s=fred_seed((uintptr_t)memory),want;
    event_state_t *v=&s.core.core.core;
    v->star=(uint64_t)star<<48;v->gdt.base=v->ldt.base=UINT64_MAX;
    v->flags=2|((events&1)<<8);s.sti_blocked=(events>>1)&1;
    v->unusable=0x7e;
    uint64_t w[]={targets[(star>>2)%4],(uint16_t)(star+16),
      2|((uint64_t)events<<8),UINT64_MAX,(uint16_t)(star+8)|((uint64_t)events<<16)|0xabcdefff00000000ULL};
    fred_frame(&s,w);want=s;memcpy(saved,memory,sizeof(memory));
    unsigned good=canonical(w[0],BITS);
    if(good) {
      want.core.core.core.cs=(event_segment_t){0,UINT32_MAX,(uint16_t)w[1],0xa0fb};
      want.core.core.core.ss=(event_segment_t){0,UINT32_MAX,(uint16_t)w[4],0xc0f3};
      fred_finish(&want,w,1);
    }
    CHECK(neverd_step_v9_1000(ARG(&s))==(good?12:5));
    CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
  }
  for(unsigned local=0;local<2;++local)for(unsigned dpl=0;dpl<4;++dpl)
    for(unsigned conforming=0;conforming<2;++conforming)for(unsigned accessed=0;accessed<2;++accessed)
      for(unsigned events=0;events<8;++events) {
        fred_state_t s=fred_seed((uintptr_t)memory),want;
        event_state_t *v=&s.core.core.core;
        unsigned cs=local?7:27,ss=local?15:35;
        uint64_t table=local?v->ldt.base:v->gdt.base;
        uint64_t cd=0x00af9a000000ffffULL|((uint64_t)dpl<<45)|((uint64_t)conforming<<42)|((uint64_t)accessed<<40);
        uint64_t sd=0x00cff2000000ffffULL|((uint64_t)accessed<<40);
        event_word(table+(cs&~7),cd);event_word(table+(ss&~7),sd);
        v->star=0;v->flags=2|((events&1)<<8);v->unusable=0x32;
        uint64_t w[]={0x1800,cs,0x244ed7,0x123456789abcdef0ULL,ss|((uint64_t)events<<16)};
        fred_frame(&s,w);want=s;memcpy(saved,memory,sizeof(memory));
        unsigned good=conforming||dpl==3;
        if(good) {
          want.core.core.core.cs=(event_segment_t){0,UINT32_MAX,cs,(uint16_t)(0xa09b|(dpl<<5)|(conforming<<2))};
          want.core.core.core.ss=(event_segment_t){0,UINT32_MAX,ss,0xc0f3};
          saved[table-(uintptr_t)memory+(cs&~7)+5]|=1;
          saved[table-(uintptr_t)memory+(ss&~7)+5]|=1;
          fred_finish(&want,w,1);
        }
        CHECK(neverd_step_v9_1000(ARG(&s))==(good?12:5));
        CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
      }
  /* The CS accessed-bit write can alias the already captured augmented SS. */
  fred_state_t s=fred_seed((uintptr_t)memory),want;
  event_state_t *v=&s.core.core.core;
  v->star=0;v->gdt.base=v->gpr[4]+16;v->gdt.limit=63;
  uint64_t w[]={0x1800,27,2,UINT64_MAX,0x00a0fa0000000023ULL};
  fred_frame(&s,w);event_word(v->gdt.base+32,0x00cff2000000ffffULL);
  want=s;memcpy(saved,memory,sizeof(memory));
  want.core.core.core.cs=(event_segment_t){0,0x23fff,27,0xa0fb};
  want.core.core.core.ss=(event_segment_t){0,UINT32_MAX,35,0xc0f3};
  saved[v->gdt.base-(uintptr_t)memory+24+5]|=1;
  saved[v->gdt.base-(uintptr_t)memory+32+5]|=1;
  fred_finish(&want,w,1);
  CHECK(neverd_step_v9_1000(ARG(&s))==12);
  CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
  return 0;
}
)C";
    run({0xf3, 0x0f, 0x01, 0xca}, {0x1000}, Harness, false, O);
  }
}

TEST_F(X64ContinuationSourceTest, FredRejectsInvalidContextsWithoutEffects) {
  for (bool User : {false, true}) {
    std::string Harness = std::string(EventHarness) + FredHarness;
    Harness += "\n#define USER " + std::to_string(User) + "\n";
    Harness += R"C(
int main(void) {
  unsigned char memory[4096],saved[4096];
  for(unsigned bad=0;bad<19;++bad) {
    fred_state_t s=fred_seed((uintptr_t)memory),want;
    event_state_t *v=&s.core.core.core;v->star=3ULL<<48;
    uint64_t w[]={0x1800,USER?19:8,2,UINT64_MAX,USER?11:16};
    if(bad==0)w[2]=0;
    if(bad==1)w[2]|=1ULL<<17;
    if(bad==2)w[1]|=1ULL<<19;
    if(bad==3)w[4]|=1ULL<<19;
    if(bad==4)w[1]^=1;
    if(bad==5)w[4]^=1;
    if(bad==6)v->cs.selector|=3;
    if(bad==7)v->cs.attributes|=0x4000;
    if(bad==8)v->unusable|=1;
    if(bad==9)s.sti_blocked=2;
    if(bad==10)s.pending_debug=1;
    if(bad==11)v->nmi_blocked=2;
    if(bad==12)v->flags|=1ULL<<63;
    if(bad==13)v->flags&=~2ULL;
    if(bad==14)v->pc++;
    if(bad==15)v->cr8=16;
    if(bad==16)w[0]=0x800000000000ULL;
    if(bad==17)s.core.uif=2;
    if(bad==18)s.core.monitor_active=2;
    fred_frame(&s,w);want=s;memcpy(saved,memory,sizeof(memory));
    CHECK(neverd_step_v9_1000(ARG(&s))==5);
    CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
  }
  /* Entry rejection, including ERETU's stack-level check, precedes frame access. */
  for(unsigned bad=0;bad<9+USER;++bad) {
    fred_state_t s=fred_seed((uintptr_t)memory),want;
    event_state_t *v=&s.core.core.core;v->gpr[4]=1;
    if(bad==0)v->pc++;
    if(bad==1)v->flags&=~2ULL;
    if(bad==2)v->cs.selector|=3;
    if(bad==3)s.sti_blocked=2;
    if(bad==4)s.pending_debug=1;
    if(bad==5)v->nmi_blocked=2;
    if(bad==6)v->mxcsr=0x10000;
    if(bad==7)s.core.uif=2;
    if(bad==8)s.core.monitor_active=2;
    if(bad==9)s.fred_config|=1;
    want=s;CHECK(neverd_step_v9_1000(ARG(&s))==5&&!memcmp(&s,&want,sizeof(s)));
  }
  if(USER)for(unsigned bad=0;bad<5;++bad) {
    fred_state_t s=fred_seed((uintptr_t)memory),want;
    event_state_t *v=&s.core.core.core;v->star=3ULL<<48;
    uint64_t w[]={0x1800,19,2,UINT64_MAX,11};
    if(bad==0)s.fred_config|=1;
    if(bad==1)w[2]|=0x1000;
    if(bad==2)w[1]=3; /* standard compatibility return */
    if(bad==3){w[1]=27;w[4]=35;event_word(v->gdt.base+24,0x00cffa000000ffffULL);}
    if(bad==4){w[1]=27;w[4]=3;event_word(v->gdt.base+24,0x00affa000000ffffULL);}
    fred_frame(&s,w);want=s;memcpy(saved,memory,sizeof(memory));
    CHECK(neverd_step_v9_1000(ARG(&s))==5);
    CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
  }
  return 0;
}
)C";
    run({uint8_t(User ? 0xf3 : 0xf2), 0x0f, 0x01, 0xca}, {0x1000}, Harness,
        false, fredOptions());
  }
}

TEST_F(X64ContinuationSourceTest, FredReadsOnlyTheFiveReturnWords) {
#if !defined(__linux__) && !defined(__APPLE__)
  GTEST_SKIP() << "guard-page fixture requires mmap";
#else
  for (bool User : {false, true}) {
    std::string Harness = std::string(EventHarness) + FredHarness;
    Harness += "\n#define USER " + std::to_string(User) + "\n";
    Harness += R"C(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  long page=sysconf(_SC_PAGESIZE);CHECK(page>=4096);
  unsigned char *map=mmap(0,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
  CHECK(map!=MAP_FAILED&&mprotect(map+page,page,PROT_READ|PROT_WRITE)==0);
  for(unsigned edge=0;edge<2;++edge) {
    unsigned char memory[4096];fred_state_t s=fred_seed((uintptr_t)memory),want;
    event_state_t *v=&s.core.core.core;v->star=3ULL<<48;
    unsigned char *frame=edge?map+2*page-40:map+page;
    v->gpr[4]=(uintptr_t)frame-8;v->gdt.base=v->ldt.base=UINT64_MAX;
    uint64_t w[]={0x1800,USER?19:8,2,(uintptr_t)map,USER?11:16};
    fred_frame(&s,w);want=s;fred_finish(&want,w,USER);
    if(USER) {
      want.core.core.core.cs=(event_segment_t){0,UINT32_MAX,19,0xa0fb};
      want.core.core.core.ss=(event_segment_t){0,UINT32_MAX,11,0xc0f3};
    }
    CHECK(neverd_step_v9_1000(ARG(&s))==12&&!memcmp(&s,&want,sizeof(s)));
    CHECK(!memcmp(frame,w,sizeof(w)));
  }
  CHECK(munmap(map,page*3)==0);return 0;
}
)C";
    run({uint8_t(User ? 0xf3 : 0xf2), 0x0f, 0x01, 0xca}, {0x1000}, Harness,
        false, fredOptions());
  }
#endif
}

TEST_F(X64ContinuationSourceTest, FredProfileDoesNotInheritLegacyTransitions) {
  for (const auto Bytes : {std::vector<uint8_t>{0x90},
                           {0x48, 0xcf},
                           {0x48, 0x0f, 0x07},
                           {0x0f, 0x34},
                           {0xf3, 0x0f, 0x01, 0xec}}) {
    auto Units =
        buildX64ContinuationSource(image(Bytes), {0x1000}, fredOptions());
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
  auto User = fastSystemOptions();
  User.Profile = X64ContinuationProfile::UserInterruptNoFaultV8;
  User.UiretRestoresUIF = false;
  for (auto O : {eventOptions(), fastSystemOptions(), User}) {
    auto Units = buildX64ContinuationSource(image({0xf2, 0x0f, 0x01, 0xca}),
                                            {0x1000}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
}
