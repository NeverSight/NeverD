//===- X64FastSystemContinuationTests.inc - Intel fast-call contexts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp to share the source/KVM runner.
//===----------------------------------------------------------------------===//

namespace {
X64ContinuationSourceOptions fastSystemOptions() {
  auto O = eventOptions();
  O.Profile = X64ContinuationProfile::KernelFastSystemNoFaultV7;
  O.SysenterMSRAddressBits = 48;
  return O;
}

const char FastSystemHarness[] = R"C(
static fast_system_state_t fast_seed(uint64_t memory) {
  fast_system_state_t s={0};s.core=event_seed(memory);
  s.sysenter_cs=0x5678000b;s.sysenter_esp=0x123456789abcULL;
  s.sysenter_eip=0x1800;s.reserved=0x7654321089abcdefULL;
  return s;
}
static int canonical(uint64_t value,unsigned bits) {
  return (uint64_t)((int64_t)(value<<(64-bits))>>(64-bits))==value;
}
static void fast_expect(fast_system_state_t *s,unsigned kind) {
  event_state_t *v=&s->core;unsigned enter=kind==0,wide=kind!=1;
  uint16_t cs=(uint16_t)(s->sysenter_cs&0xfffc);
  cs=(uint16_t)(cs+(enter?0:wide?32:16))|(enter?0:3);
  v->cs=(event_segment_t){0,UINT32_MAX,cs,(enter?0xa09b:wide?0xa0fb:0xc0fb)};
  v->ss=(event_segment_t){0,UINT32_MAX,(uint16_t)(cs+8),enter?0xc093:0xc0f3};
  v->unusable&=~3;
  v->pc=enter?s->sysenter_eip:wide?v->gpr[2]:(uint32_t)v->gpr[2];
  v->gpr[4]=enter?s->sysenter_esp:wide?v->gpr[1]:(uint32_t)v->gpr[1];
  if(enter)v->flags&=~0x20200ULL;
  if(!wide)for(unsigned i=0;i<8;++i)
    v->gpr[i]=(uint32_t)v->gpr[i]|(v->arbitrary[i]<<32);
}
)C";
} // namespace

TEST_F(X64ContinuationSourceTest, FastSystemContextAndNoEffectRefusals) {
  for (unsigned Bits : {48u, 57u})
    for (unsigned Kind = 0; Kind != 3; ++Kind) {
      SCOPED_TRACE(Bits);
      SCOPED_TRACE(Kind);
      auto O = fastSystemOptions();
      O.PagingAddressBits = Bits;
      O.SysenterMSRAddressBits = Bits;
      std::string Harness = std::string(EventHarness) + FastSystemHarness;
      Harness += "\n#define KIND " + std::to_string(Kind) + "\n#define BITS " +
                 std::to_string(Bits) + "\n";
      Harness += R"C(
int main(void) {
  const uint64_t values[]={0,1,0xffffffffULL,0x80000000ULL,0x7fffffffffffULL,
    UINT64_MAX,0x800000000000ULL,0x00ffffffffffffffULL,
    0xff00000000000000ULL,0x0100000000000000ULL};
  const uint32_t selectors[]={0,1,3,4,7,8,0xffff,0xffe8,0xfff8,0xffff0000,0xa5a5000b};
  unsigned char memory[4096],saved[4096];
  for(unsigned sel=0;sel<11;++sel)for(unsigned pc=0;pc<10;++pc)
    for(unsigned sp=0;sp<10;++sp)for(unsigned cpl=0;cpl<4;++cpl)
      for(unsigned choice=0;choice<3;++choice) {
        fast_system_state_t s=fast_seed((uintptr_t)memory),want;
        event_state_t *v=&s.core;
        s.sysenter_cs=selectors[sel];
        s.sysenter_eip=KIND==0?values[pc]:0x1800;
        s.sysenter_esp=KIND==0?values[sp]:0x10000;
        v->gpr[1]=values[sp];v->gpr[2]=values[pc];
        v->cs.selector=8|cpl;v->cs.attributes=0xa09b|(cpl<<5);
        v->ss.selector=16|cpl;v->ss.attributes=0xc093|(cpl<<5);
        v->unusable=0x72;v->nmi_blocked=pc&1;
        v->flags=choice==0?2:choice==1?0x244ed7:0x40002;
        for(unsigned r=0;r<8;++r)v->arbitrary[r]=(values[pc]^values[sp])+(r+1)*0x123456789ULL;
        /* These instructions must not consult the descriptor RAM or stack. */
        v->gdt.base=v->idt.base=v->gpr[4]=UINT64_MAX;
        memcpy(saved,memory,sizeof(memory));want=s;
        unsigned good=(selectors[sel]&0xfffc)!=0&&(KIND==0||cpl==0);
        if(KIND==0)good&=canonical(values[pc],BITS)&&canonical(values[sp],BITS);
        if(KIND==2)good&=canonical(values[pc],BITS)&&canonical(values[sp],BITS);
        if(good)fast_expect(&want,KIND);
        CHECK(neverd_step_v7_1000(ARG(&s))==(good?(KIND==0?10:6):5));
        CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
      }
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned dpl=0;dpl<4;++dpl)
    for(unsigned conforming=0;conforming<2;++conforming) {
      fast_system_state_t s=fast_seed((uintptr_t)memory),want;
      s.core.cs.selector=8|cpl;s.core.cs.attributes=0xa09b|(dpl<<5)|(conforming<<2);
      s.core.ss.selector=16|cpl;s.core.ss.attributes=0xc093|(cpl<<5);
      s.core.gpr[1]=0x10000;s.core.gpr[2]=0x1800;
      want=s;memcpy(saved,memory,sizeof(memory));
      unsigned good=KIND==0?(conforming?dpl<=cpl:dpl==cpl):(cpl==0&&dpl==0);
      if(good)fast_expect(&want,KIND);
      CHECK(neverd_step_v7_1000(ARG(&s))==(good?(KIND==0?10:6):5));
      CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(memory,saved,sizeof(memory)));
    }
  for(unsigned bad=0;bad<9;++bad) {
    fast_system_state_t s=fast_seed((uintptr_t)memory);s.core.gpr[1]=0x9000;s.core.gpr[2]=0x1800;
    if(bad==0)s.sysenter_cs|=1ULL<<32;
    if(bad==1)s.sysenter_eip=1ULL<<(BITS-1);
    if(bad==2)s.sysenter_esp=1ULL<<(BITS-1);
    if(bad==3)s.core.pc++;
    if(bad==4)s.core.cs.attributes|=0x4000;
    if(bad==5)s.core.unusable|=1;
    if(bad==6)s.core.flags|=0x10000;
    if(bad==7)s.core.cr8=16;
    if(bad==8)s.core.nmi_blocked=2;
    fast_system_state_t want=s;
    CHECK(neverd_step_v7_1000(ARG(&s))==5&&!memcmp(&s,&want,sizeof(s)));
  }
  return 0;
}
)C";
      run(Kind == 0   ? std::vector<uint8_t>{0x0f, 0x34}
          : Kind == 1 ? std::vector<uint8_t>{0x0f, 0x35}
                      : std::vector<uint8_t>{0x48, 0x0f, 0x35},
          {0x1000}, Harness, false, O);
    }
}

TEST_F(X64ContinuationSourceTest,
       FastSystemMsrsAreLiveAcrossReadsAndTransfers) {
  for (unsigned Bits : {48u, 57u}) {
    auto O = fastSystemOptions();
    O.SysenterMSRAddressBits = Bits;
    O.SegmentBaseMSRAddressBits = Bits;
    std::string Harness = std::string(EventHarness) + FastSystemHarness;
    Harness += "\n#define BITS " + std::to_string(Bits) + "\n";
    Harness += R"C(
static uint64_t read_msr(fast_system_state_t *s,unsigned selector) {
  s->core.gpr[1]=selector;s->core.pc=0x1002;
  CHECK(neverd_step_v7_1002(ARG(s))==0);
  return s->core.gpr[0]|(s->core.gpr[2]<<32);
}
static void write_msr(fast_system_state_t *s,unsigned selector,uint64_t value) {
  s->core.gpr[1]=selector;s->core.gpr[0]=(uint32_t)value;
  s->core.gpr[2]=value>>32;s->core.pc=0x1000;
  fast_system_state_t want=*s;
  if(selector!=0x174&&!canonical(value,BITS)) {
    CHECK(neverd_step_v7_1000(ARG(s))==5&&!memcmp(s,&want,sizeof(*s)));return;
  }
  if(selector==0x174)want.sysenter_cs=(uint32_t)value;
  else if(selector==0x175)want.sysenter_esp=value;
  else if(selector==0x176)want.sysenter_eip=value;
  else if(selector==0xc0000100)want.core.fs_base=value;
  else if(selector==0xc0000101)want.core.gs_base=value;
  else want.core.kernel_gs=value;
  want.core.pc=0x1002;
  CHECK(neverd_step_v7_1000(ARG(s))==0&&!memcmp(s,&want,sizeof(*s)));
  CHECK(read_msr(s,selector)==(selector==0x174?(uint32_t)value:value));
}
int main(void) {
  unsigned char memory[4096];fast_system_state_t s=fast_seed((uintptr_t)memory);
  const unsigned selectors[]={0x174,0x175,0x176,0xc0000100,0xc0000101,0xc0000102};
  const uint64_t values[]={0,1,UINT64_MAX,0xa5a5ffff00112233ULL,0xffffffffULL,
    0x0000800000000000ULL,0x00ffffffffffffffULL,0xff00000000000000ULL};
  for(unsigned i=0;i<6;++i)for(unsigned j=0;j<8;++j)write_msr(&s,selectors[i],values[j]);
  write_msr(&s,0x174,0xabcdef12a5a5000bULL);
  write_msr(&s,0x175,0x123456789abcULL);write_msr(&s,0x176,0x1800);
  s.core.pc=0x1004;fast_system_state_t want=s;fast_expect(&want,0);
  CHECK(neverd_step_v7_1004(ARG(&s))==10&&!memcmp(&s,&want,sizeof(s)));
  s.core.pc=0x1006;s.core.gpr[1]=0x20000;s.core.gpr[2]=0x1800;want=s;fast_expect(&want,2);
  CHECK(neverd_step_v7_1006(ARG(&s))==6&&!memcmp(&s,&want,sizeof(s)));
  for(unsigned i=0;i<2;++i) {
    s=fast_seed((uintptr_t)memory);s.core.gpr[1]=0x177;s.core.pc=i?0x1002:0x1000;want=s;
    CHECK((i?neverd_step_v7_1002(ARG(&s)):neverd_step_v7_1000(ARG(&s)))==5&&!memcmp(&s,&want,sizeof(s)));
  }
  /* The processor's MSR width is independent of the active 48-bit paging width. */
  s=fast_seed((uintptr_t)memory);write_msr(&s,0x175,0x0000800000000000ULL);
  s.core.pc=0x1004;want=s;fast_expect(&want,0);
  CHECK(neverd_step_v7_1004(ARG(&s))==10&&!memcmp(&s,&want,sizeof(s)));
  s=fast_seed((uintptr_t)memory);write_msr(&s,0x176,0x0000800000000000ULL);
  s.core.pc=0x1004;want=s;
  if(BITS==48)fast_expect(&want,0);
  CHECK(neverd_step_v7_1004(ARG(&s))==(BITS==48?10:5)&&!memcmp(&s,&want,sizeof(s)));
  return 0;
}
)C";
    run({0x0f, 0x30, 0x0f, 0x32, 0x0f, 0x34, 0x48, 0x0f, 0x35},
        {0x1000, 0x1002, 0x1004, 0x1006}, Harness, false, O);
  }
}

TEST_F(X64ContinuationSourceTest, FastSystemProfileBindingsAreExplicit) {
  auto O = fastSystemOptions();
  for (unsigned I = 0; I != 5; ++I) {
    auto Bad = O;
    if (I == 0)
      Bad.SysenterMSRAddressBits.reset();
    if (I == 1)
      Bad.SysenterMSRAddressBits = 49;
    if (I == 2)
      Bad.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
    if (I == 3)
      Bad.SegmentBaseMSRAddressBits = 57;
    if (I == 4)
      Bad.PagingAddressBits = 57;
    auto Result =
        buildX64ContinuationSource(image({0x0f, 0x34}), {0x1000}, Bad);
    EXPECT_FALSE(bool(Result));
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
  for (auto Profile : {X64ContinuationProfile::KernelReturnNoFaultV5,
                       X64ContinuationProfile::KernelEventsNoFaultV6}) {
    auto Old = eventOptions();
    Old.Profile = Profile;
    for (const auto &Bytes :
         {std::vector<uint8_t>{0x0f, 0x34}, std::vector<uint8_t>{0x0f, 0x35},
          std::vector<uint8_t>{0x48, 0x0f, 0x35}}) {
      auto Units = buildX64ContinuationSource(image(Bytes), {0x1000}, Old);
      ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
      EXPECT_TRUE(Units->front().RequiresNativeExecution);
    }
  }
}

TEST_F(X64ContinuationSourceTest, FastSystemMsrsMatchNativeReadback) {
  const std::string Harness =
      std::string(EventHarness) + FastSystemHarness + R"C(
int main(void) {
  unsigned a,b,c,d;
  __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
  if(b!=0x756e6547||d!=0x49656e69||c!=0x6c65746e)return 77;
  __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
  if(!(d&(1u<<11)))return 77;
  const uint64_t values[]={0,1,UINT64_MAX,0xffffffffULL,0x123456789abcULL,
                          0xffff800012345678ULL};
  unsigned char *memory=(unsigned char *)0x8000;
  for(unsigned selector=0x174;selector<=0x176;++selector)
    for(unsigned value=0;value<6;++value) {
      fast_system_state_t s=fast_seed((uintptr_t)memory);
      uint32_t lo=values[value],hi=values[value]>>32;
      __asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(selector):"memory");
      __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(selector):"memory");
      uint64_t hardware=lo|((uint64_t)hi<<32);
      CHECK(hardware==(selector==0x174?(uint32_t)values[value]:values[value]));
      s.core.gpr[1]=selector;s.core.gpr[0]=(uint32_t)values[value];
      s.core.gpr[2]=values[value]>>32;
      CHECK(neverd_step_v7_1000(ARG(&s))==0);
      CHECK(neverd_step_v7_1002(ARG(&s))==0);
      CHECK(s.core.gpr[0]==lo&&s.core.gpr[2]==hi);
      CHECK((selector==0x174?s.sysenter_cs:selector==0x175?s.sysenter_esp:s.sysenter_eip)==hardware);
    }
  return 0;
}
)C";
  run({0x0f, 0x30, 0x0f, 0x32}, {0x1000, 0x1002}, Harness, false,
      fastSystemOptions(), {}, true);
}

TEST_F(X64ContinuationSourceTest, FastSystemMatchesNativePrivilegeContexts) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Kind);
    std::string Harness = std::string(EventHarness) + FastSystemHarness;
    Harness += "\n#define KIND " + std::to_string(Kind) + "\n";
    Harness += R"C(
int main(void) {
  const uint32_t selectors[]={4,7,8,0xffd8,0xfff8,0xffff,0xa5a5000b};
  const uint64_t flags[]={2,0x246,0x646,0x244ed7,0x40002};
  const uint64_t stacks[]={0,1,0xffffffffULL,0x123456789abcULL,0xffff800012345678ULL};
  unsigned char *memory=(unsigned char *)0x8000;
  for(unsigned sel=0;sel<7;++sel)for(unsigned flag=0;flag<5;++flag)
    for(unsigned stack=0;stack<5;++stack)for(unsigned nmi=0;nmi<2;++nmi)
      for(unsigned context=0;context<(KIND==0?32:1);++context) {
        unsigned cpl=context&3,dpl=(context>>2)&3,conforming=context>>4;
        if(conforming?dpl>cpl:dpl!=cpl)continue;
        fast_system_state_t s=fast_seed((uintptr_t)memory),q,want;
        s.sysenter_cs=selectors[sel];s.sysenter_esp=stacks[stack];
        s.core.flags=flags[flag];s.core.nmi_blocked=nmi;
        s.core.gpr[1]=stacks[stack];s.core.gpr[2]=KIND==1?0x1234567800001800ULL:0x1800;
        s.core.cs.selector=8|cpl;s.core.cs.attributes=0xa09b|(dpl<<5)|(conforming<<2);
        s.core.ss.selector=16|cpl;s.core.ss.attributes=0xc093|(cpl<<5);
        s.core.ds=s.core.es=s.core.ss;
        s.core.fs=(event_metadata_t){UINT32_MAX,(uint16_t)(16|cpl),(uint16_t)(0xc093|(cpl<<5))};
        s.core.gs=s.core.fs;
        q=s;
        __asm__ volatile("outb %%al,$0xe9"::"a"(9+KIND),"D"(&q):"memory");
        // Compare the independently specified defined state too. Only the
        // eight genuinely arbitrary high halves may bind to a native witness.
        if(KIND==1)for(unsigned r=0;r<8;++r)
          s.core.arbitrary[r]=q.core.gpr[r]>>32;
        want=s;fast_expect(&want,KIND);
        memcpy(q.core.arbitrary,s.core.arbitrary,sizeof(q.core.arbitrary));
        CHECK(!memcmp(&q,&want,sizeof(q)));
        CHECK(neverd_step_v7_1000(ARG(&s))==(KIND==0?10:6));
        CHECK(!memcmp(&s,&q,sizeof(s)));
      }
  return 0;
}
)C";
    run(Kind == 0   ? std::vector<uint8_t>{0x0f, 0x34}
        : Kind == 1 ? std::vector<uint8_t>{0x0f, 0x35}
                    : std::vector<uint8_t>{0x48, 0x0f, 0x35},
        {0x1000}, Harness, false, fastSystemOptions(), {}, true);
  }
}
