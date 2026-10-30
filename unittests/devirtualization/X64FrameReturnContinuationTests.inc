//===- X64FrameReturnContinuationTests.inc - Legacy mode returns
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp after its shared event harness.
/// Original KVM instructions qualify defined state; independent choices cover
/// every legacy GPR high half after a compatibility transition.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest,
       CompatibilityFrameReturnsMatchNativeStateAndStackWidths) {
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    const unsigned Discard = Kind == 2 ? 7 : Kind == 3 ? 65535 : 0;
    std::vector<uint8_t> Bytes{0x48, uint8_t(Kind == 0   ? 0xcf
                                             : Kind == 1 ? 0xcb
                                                         : 0xca)};
    if (Kind >= 2) {
      Bytes.push_back(uint8_t(Discard));
      Bytes.push_back(uint8_t(Discard >> 8));
    }
    auto Units =
        buildX64ContinuationSource(image(Bytes), {0x1000}, eventOptions());
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    ASSERT_EQ(Units->front().CompatibilityChoices.size(), 8u);
    EXPECT_TRUE(Units->front().UndefinedEffects.empty());
    for (unsigned I = 0; I != 8; ++I) {
      EXPECT_EQ(Units->front().CompatibilityChoices[I].Slot, I);
      EXPECT_EQ(Units->front().CompatibilityChoices[I].Output,
                NdVar::reg(x86reg::generalReg(I), 8));
    }
    std::string Harness = "#define KIND " + std::to_string(Kind) +
                          "\n#define DISCARD " + std::to_string(Discard) +
                          "\n" + EventHarness;
    Harness += R"C(
static void compare_frame(event_state_t s) {
  unsigned char before[4096],after[4096],frame_before[131136],frame_after[131136];
  event_state_t native=s;
  memcpy(before,(void *)0x8000,sizeof(before));
  memcpy(frame_before,(void *)0x210000,sizeof(frame_before));
  __asm__ volatile("outb %%al,$0xe9"::"a"(KIND==0?2:KIND==1?5:6),
                   "D"(&native),"S"(DISCARD):"memory");
  memcpy(after,(void *)0x8000,sizeof(after));
  memcpy(frame_after,(void *)0x210000,sizeof(frame_after));
  for(unsigned choice=0;choice<4;++choice) {
    event_state_t actual=s,want=native;
    memcpy((void *)0x8000,before,sizeof(before));
    memcpy((void *)0x210000,frame_before,sizeof(frame_before));
    for(unsigned i=0;i<8;++i) {
      uint64_t value=choice==0?native.gpr[i]>>32:choice==1?0:
                     choice==2?UINT64_MAX:0xdeadbeef12345678ULL+i;
      actual.arbitrary[i]=want.arbitrary[i]=value;
      want.gpr[i]=(uint32_t)want.gpr[i]|(value<<32);
    }
    CHECK(neverd_step_v6_1000(ARG(&actual))==6);
    CHECK(!memcmp(&actual,&want,sizeof(actual)));
    CHECK(!memcmp((void *)0x8000,after,sizeof(after)));
    CHECK(!memcmp((void *)0x210000,frame_after,sizeof(frame_after)));
  }
}
int main(void) {
  const uint64_t ips[]={0x1800,0x100001800ULL,0xffff800000001800ULL};
  const uint64_t stacks[]={0x12345678abcdfff8ULL,0xfffffffffffffff8ULL,0x12345678};
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned db=0;db<2;++db)
    for(unsigned local=0;local<2;++local)for(unsigned stackb=0;stackb<2;++stackb)
      for(unsigned skew=0;skew<2;++skew)for(unsigned ip=0;ip<3;++ip)
        for(unsigned stack=0;stack<3;++stack) {
          event_state_t s=event_seed(0x8000);
          s.gpr[4]=skew?0x21fff0:0x210000;
          s.ss.attributes=0x8093|(stackb<<14);s.nmi_blocked=skew;
          memset((void *)0x210000,0xa5,131136);
          uint64_t table=local?s.ldt.base:s.gdt.base;
          unsigned cs=local?4:24,ss=local?12:32;
          event_word(table+(cs&~7),0x008f9a000000ffffULL|
                     ((uint64_t)cpl<<45)|((uint64_t)db<<54)|((uint64_t)skew<<40));
          event_word(table+(ss&~7),0x008f92000000ffffULL|
                     ((uint64_t)cpl<<45)|((uint64_t)stackb<<54));
          event_word(s.gpr[4],ips[ip]);event_word(s.gpr[4]+8,cs|cpl);
          if(KIND==0) {
            event_word(s.gpr[4]+16,0x246);event_word(s.gpr[4]+24,stacks[stack]);
            event_word(s.gpr[4]+32,ss|cpl);
          } else if(cpl) {
            event_word(s.gpr[4]+16+DISCARD,stacks[stack]);
            event_word(s.gpr[4]+24+DISCARD,ss|cpl);
          }
          compare_frame(s);
        }
  /* A pending CS accessed bit is visible to the later saved-SP read. */
  for(unsigned cpl=1;cpl<4;++cpl)for(unsigned stackb=0;stackb<2;++stackb) {
    event_state_t s=event_seed(0x8000);s.gpr[4]=0x210000;
    memset((void *)0x210000,0xa5,131136);
    event_word(s.gpr[4],0x1800);event_word(s.gpr[4]+8,4|cpl);
    unsigned offset=KIND==0?24:16+DISCARD;
    if(KIND==0)event_word(s.gpr[4]+16,0x246);
    event_word(s.gpr[4]+offset,0x12345678);
    event_word(s.gpr[4]+offset+8,32|cpl);
    s.ldt.base=s.gpr[4]+offset-5;s.ldt.limit=7;
    event_word(s.ldt.base,0x00cf9a000000ffffULL|((uint64_t)cpl<<45));
    event_word(s.gdt.base+32,0x008f92000000ffffULL|
               ((uint64_t)cpl<<45)|((uint64_t)stackb<<54));
    compare_frame(s);
  }
  return 0;
}
)C";
    run(Bytes, {0x1000}, Harness, false, eventOptions(), {}, true);
  }
}

TEST_F(X64ContinuationSourceTest,
       CompatibilityFrameReturnsCheckLimitsBeforePublishingState) {
  for (unsigned Bits : {48u, 57u}) {
    auto O = eventOptions();
    O.PagingAddressBits = Bits;
    run({0x48, 0xcf, 0x48, 0xcb, 0x48, 0xca, 0xff, 0xff},
        {0x1000, 0x1002, 0x1004}, std::string(EventHarness) + R"C(
static uint64_t frame_step(unsigned kind,event_state_t *s) {
  return kind==0?neverd_step_v6_1000(ARG(s)):
         kind==1?neverd_step_v6_1002(ARG(s)):neverd_step_v6_1004(ARG(s));
}
int main(void) {
  unsigned char memory[4096],before[4096],frame[65600],frame_before[65600];
  for(unsigned kind=0;kind<3;++kind)for(unsigned cpl=0;cpl<4;++cpl)
    for(unsigned db=0;db<2;++db)for(unsigned stackb=0;stackb<2;++stackb)
      for(unsigned local=0;local<2;++local)for(unsigned gran=0;gran<2;++gran)
        for(unsigned base=0;base<2;++base)for(unsigned edge=0;edge<4;++edge) {
          event_state_t s=event_seed((uintptr_t)memory);s.pc=0x1000+kind*2;
          memset(frame,0xa5,sizeof(frame));s.gpr[4]=(uintptr_t)frame;
          s.ss.attributes=0x8093|(stackb<<14);
          uint32_t limit=gran?0x1234fff:0x1234;
          uint64_t table=local?s.ldt.base:s.gdt.base;
          unsigned cs=local?4:24,ss=local?12:32,discard=kind==2?65535:0;
          uint32_t ip=edge==0?0:edge==1?limit:edge==2?limit+1:UINT32_MAX;
          uint32_t codebase=base?0x12345678:0;
          uint64_t code=0x1234|((uint64_t)(0x9a|(cpl<<5))<<40)|
            ((uint64_t)db<<54)|((uint64_t)gran<<55)|
            ((uint64_t)(codebase&0xffffff)<<16)|((uint64_t)(codebase>>24)<<56);
          event_word(table+(cs&~7),code);
          event_word(table+(ss&~7),0x008f92000000ffffULL|
                     ((uint64_t)cpl<<45)|((uint64_t)stackb<<54));
          event_word(s.gpr[4],0xffff800000000000ULL|ip);
          event_word(s.gpr[4]+8,cs|cpl);
          const uint64_t new_sp=0xdeadbeefabcdfff8ULL;
          if(kind==0) {
            event_word(s.gpr[4]+16,0x246);event_word(s.gpr[4]+24,new_sp);
            event_word(s.gpr[4]+32,ss|cpl);
          } else if(cpl) {
            event_word(s.gpr[4]+16+discard,new_sp);
            event_word(s.gpr[4]+24+discard,ss|cpl);
          }
          for(unsigned i=0;i<8;++i)s.arbitrary[i]=0xaabbccdd01234567ULL+i;
          event_state_t want=s;memcpy(before,memory,sizeof(memory));
          memcpy(frame_before,frame,sizeof(frame));
          if(ip>limit) {
            CHECK(frame_step(kind,&s)==5 && !memcmp(&s,&want,sizeof(s)));
            CHECK(!memcmp(memory,before,sizeof(memory)));
          } else {
            before[table-(uintptr_t)memory+(cs&~7)+5]|=1;
            want.cs=(event_segment_t){codebase,limit,cs|cpl,
                                      0x9b|(cpl<<5)|(db<<14)|(gran<<15)};
            want.unusable&=~1;
            if(kind==0 || cpl) {
              before[table-(uintptr_t)memory+(ss&~7)+5]|=1;
              want.ss=(event_segment_t){0,UINT32_MAX,ss|cpl,
                                        0x8093|(cpl<<5)|(stackb<<14)};
              want.unusable&=~2;
            }
            if(cpl) {
              want.ds.selector=want.es.selector=want.fs.selector=want.gs.selector=0;
              want.unusable|=60;
            }
            uint64_t raw=(kind && !cpl?s.gpr[4]+16:new_sp)+discard;
            uint32_t sp=stackb?(uint32_t)raw:
              ((uint32_t)s.gpr[4]&0xffff0000)|(uint16_t)raw;
            want.gpr[4]=sp;want.pc=ip;if(kind==0)want.nmi_blocked=0;
            for(unsigned i=0;i<8;++i)
              want.gpr[i]=(uint32_t)want.gpr[i]|(want.arbitrary[i]<<32);
            CHECK(frame_step(kind,&s)==6 && !memcmp(&s,&want,sizeof(s)));
            CHECK(!memcmp(memory,before,sizeof(memory)));
          }
          CHECK(!memcmp(frame,frame_before,sizeof(frame)));
        }
  return 0;
}
)C",
        false, O);
  }
}

TEST_F(X64ContinuationSourceTest,
       CompatibilityFrameReturnsRejectNullStacksAndInvalidModes) {
  run({0x48, 0xcf, 0x48, 0xcb}, {0x1000, 0x1002},
      std::string(EventHarness) + R"C(
int main(void) {
  unsigned char memory[4096],before[4096];
  for(unsigned kind=0;kind<2;++kind)for(unsigned cpl=0;cpl<4;++cpl)
    for(unsigned bad=0;bad<3;++bad) {
      if(kind && !cpl && bad==0)continue; /* Same-CPL RETF retains SS. */
      event_state_t s=event_seed((uintptr_t)memory);s.pc=0x1000+2*kind;
      event_word(s.gdt.base+24,(bad==1?0x00ef9a000000ffffULL:
                                        0x00cf9a000000ffffULL)|((uint64_t)cpl<<45));
      event_word(s.gdt.base+32,0x00cf92000000ffffULL|((uint64_t)cpl<<45));
      event_word(s.gpr[4],bad==2?0x0100000000001800ULL:0x1800);
      event_word(s.gpr[4]+8,24|cpl);
      if(!kind)event_word(s.gpr[4]+16,0x246);
      event_word(s.gpr[4]+(kind?16:24),0x12345678);
      event_word(s.gpr[4]+(kind?24:32),bad==0?cpl:32|cpl);
      event_state_t want=s;memcpy(before,memory,sizeof(memory));
      CHECK((kind?neverd_step_v6_1002(ARG(&s)):neverd_step_v6_1000(ARG(&s)))==5);
      CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp(memory,before,sizeof(memory)));
    }
  return 0;
}
)C",
      false, eventOptions());
}
