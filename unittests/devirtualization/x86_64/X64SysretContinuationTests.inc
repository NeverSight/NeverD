//===- X64SysretContinuationTests.inc - Compatibility return oracles -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp to share the source/KVM runner.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest, SysretCompatibilityPublishesCompleteContext) {
  for (bool V6 : {false, true})
    for (unsigned Bits : {48u, 57u}) {
      SCOPED_TRACE(V6);
      SCOPED_TRACE(Bits);
      auto O = eventOptions();
      if (!V6)
        O.Profile = X64ContinuationProfile::KernelReturnNoFaultV5;
      O.PagingAddressBits = Bits;
      std::string Harness = EventHarness;
      Harness += V6 ? R"C(
typedef event_state_t return_state_t;
#define GPR(s) ((s).gpr)
#define FLAGS(s) ((s).flags)
#define PC(s) ((s).pc)
#define ARBITRARY(s) ((s).arbitrary)
#define RETURN_V6 1
#define STEP(s) neverd_step_v6_1000(ARG(s))
)C"
                    : R"C(
typedef state_v5_t return_state_t;
#define GPR(s) ((s).core.core.core.gpr)
#define FLAGS(s) ((s).core.core.core.flags)
#define PC(s) ((s).core.core.core.pc)
#define ARBITRARY(s) ((s).core.core.core.arbitrary)
#define RETURN_V6 0
#define STEP(s) neverd_step_v5_1000(ARG(s))
)C";
      Harness += R"C(
int main(void) {
  const uint16_t selectors[]={0,1,3,4,0x23,0xffe8,0xfff0,0xfff8,0xffff};
  const uint64_t pcs[]={0,1,0xffffffffULL,0x80000000ULL,0x7fffffffffffULL,
    UINT64_MAX,0x800000000000ULL,0x00ffffffffffffffULL,
    0xff00000000000000ULL,0x0100000000000000ULL};
  const uint64_t witnesses[]={0,UINT64_MAX,1,0x80000000,0x12345678abcdef01ULL};
  unsigned char memory[4096];
  for(unsigned i=0;i<9;++i)for(unsigned bit=0;bit<65;++bit)
    for(unsigned pc=0;pc<10;++pc)for(unsigned choice=0;choice<5;++choice) {
      event_state_t initial=event_seed((uintptr_t)memory);
      initial.unusable=0x72;initial.nmi_blocked=pc&1;
      initial.ss.selector=0;initial.fs.selector=1;initial.gs.selector=3;initial.ldt.selector=0;
      initial.gdt.base=UINT64_MAX;
      return_state_t s;memcpy(&s,&initial,sizeof(s));
      GPR(s)[1]=pcs[pc];GPR(s)[11]=bit==64?UINT64_MAX:1ULL<<bit;
      FLAGS(s)|=0x40000;GPR(s)[4]=0x876543210abcdef0ULL;
      for(unsigned reg=0;reg<8;++reg)
        ARBITRARY(s)[reg]=witnesses[choice]^(reg*0x102030405060708ULL);
      s.star=((uint64_t)selectors[i]<<48)|0xabcde0123456ULL;
      return_state_t want=s;
      PC(want)=(uint32_t)pcs[pc];FLAGS(want)=(GPR(s)[11]&0x3c7fd7)|2;
      for(unsigned reg=0;reg<8;++reg)
        GPR(want)[reg]=(uint32_t)GPR(s)[reg]|(ARBITRARY(s)[reg]<<32);
      want.cs=(__typeof__(want.cs)){0,0xffffffff,selectors[i]|3,0xc0fb};
      want.ss=(__typeof__(want.ss)){0,0xffffffff,(uint16_t)(selectors[i]+8)|3,0xc0f3};
#if RETURN_V6
      want.unusable&=~3;
#endif
      CHECK(STEP(&s)==6 && !memcmp(&s,&want,sizeof(s)));
      PC(s)=0x1000;want=s;
      CHECK(STEP(&s)==5 && !memcmp(&s,&want,sizeof(s)));
    }
  return 0;
}
)C";
      run({0x0f, 0x07}, {0x1000}, Harness, false, O);
    }
}

TEST_F(X64ContinuationSourceTest, SysretCompatibilityMatchesNativeTransition) {
  auto O = eventOptions();
  O.Profile = X64ContinuationProfile::KernelReturnNoFaultV5;
  run({0x0f, 0x07}, {0x1000}, R"C(
typedef struct {
  uint64_t gpr[16],flags,pc,star;
  segment_t cs,ss;
} return_query_t;
int main(void) {
  const uint16_t selectors[]={0,1,3,4,0x23,0xffe8,0xfff0,0xfff8,0xffff};
  const uint64_t high[]={0,0xffffffff00000000ULL,0x800000000000ULL,
    0xff00000000000000ULL,0x1234567800000000ULL,0x8000000000000000ULL};
  for(unsigned i=0;i<9;++i)for(unsigned bit=0;bit<65;++bit)
    for(unsigned upper=0;upper<6;++upper) {
      state_v5_t s=seed_v5(0x1000),want;s.star=(uint64_t)selectors[i]<<48;
      s.core.core.core.gpr[1]=high[upper]|0x1800;
      s.core.core.core.gpr[4]=0x876543210abcdef0ULL;
      s.core.core.core.gpr[11]=bit==64?UINT64_MAX:1ULL<<bit;
      return_query_t q={0};memcpy(q.gpr,s.core.core.core.gpr,sizeof(q.gpr));
      q.flags=s.core.core.core.flags;q.star=s.star;
      __asm__ volatile("outb %%al,$0xe9"::"a"(8),"D"(&q):"memory");
      // Bind each permitted arbitrary high half to this native execution's
      // witness. Check the defined low halves and preserved R8-R15 separately;
      // no output bit is masked out of the subsequent full-state comparison.
      for(unsigned reg=0;reg<16;++reg)
        CHECK(reg<8 ? (uint32_t)q.gpr[reg]==(uint32_t)s.core.core.core.gpr[reg]
                    : q.gpr[reg]==s.core.core.core.gpr[reg]);
      for(unsigned reg=0;reg<8;++reg)
        s.core.core.core.arbitrary[reg]=q.gpr[reg]>>32;
      want=s;memcpy(want.core.core.core.gpr,q.gpr,sizeof(q.gpr));
      want.core.core.core.flags=q.flags;want.core.core.core.pc=q.pc;
      want.cs=q.cs;want.ss=q.ss;
      CHECK(neverd_step_v5_1000(ARG(&s))==6 && !memcmp(&s,&want,sizeof(s)));
    }
  return 0;
}
)C",
      false, O, {}, true);
}
