//===- X64EventDeliveryExportTests.inc - Explicit event delivery --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Public event classification, privilege stacks and interruptibility.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest,
       EventDeliveryOwnsFramesErrorsAndPrivilegeStacks) {
  load({0x90});
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] =
        source(request(R"("event_delivery":true,"instructions":[4096])"), LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getBoolean("event_delivery"), true);
    EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 1);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  uint64_t gdt[8],idt[512];unsigned char tss[128],stack[512],inner[512];
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned dpl=0;dpl<=cpl;++dpl)
  for(unsigned conform=0;conform<2;++conform)for(unsigned ist=0;ist<8;++ist)
  for(unsigned mode=0;mode<3;++mode)for(unsigned gate=14;gate<=15;++gate)
  for(unsigned kind=0;kind<=6;++kind) {
    const unsigned vectors[]={14,1,14,2,14,1,8};
    neverd_x64_event_v1 e={0};e.Kind=kind;e.Vector=vectors[kind];
    e.SavedPC=0x1234;
    if(kind==0){e.ErrorCode=0x17;e.FaultAddress=0xabc000;}
    neverd_x64_state_v6 s=seed();s.NMIBlocked=0;s.RFlags=0x3d7fd7;
    s.CS=(neverd_x64_segment){mode?0x23000:0,UINT32_MAX,8|cpl,
          0x809b|(cpl<<5)|(mode==0?0x2000:mode==1?0x4000:0)};
    s.SS.Selector=16|cpl;s.SS.Attributes=0xc093|(cpl<<5);
    s.GDTR=(neverd_x64_descriptor_table){(uintptr_t)gdt,sizeof(gdt)-1,0,0};
    s.IDTR=(neverd_x64_descriptor_table){(uintptr_t)idt,sizeof(idt)-1,0,0};
    s.TR=(neverd_x64_segment){(uintptr_t)tss,sizeof(tss)-1,48,0x8b};
    s.GPR[4]=(uintptr_t)(stack+479);
    memset(gdt,0,sizeof(gdt));memset(idt,0,sizeof(idt));memset(tss,0,sizeof(tss));
    memset(stack,0xa5,sizeof(stack));memset(inner,0xa5,sizeof(inner));
    gdt[3]=0x00af9a000000ffffULL|((uint64_t)dpl<<45)|((uint64_t)conform<<42);
    idt[2*e.Vector]=0x1800|(24ULL<<16)|((uint64_t)ist<<32)|((uint64_t)(0xe0|gate)<<40);
    uint64_t inner_sp=(uintptr_t)(inner+479);
    memcpy(tss+(ist?28+8*ist:4+8*dpl),&inner_sp,8);
    const unsigned new_cpl=conform?cpl:dpl, switched=ist||new_cpl<cpl;
    const unsigned error=kind==0||kind==6;
    const uint64_t top=(switched?inner_sp:s.GPR[4])&~15ULL;
    const uint64_t sp=top-40-8*error,old_sp=s.GPR[4],old_flags=s.RFlags;
    neverd_x64_state_v6 old=s;neverd_x64_event_v1 old_e=e;
    CHECK(neverd_x64_deliver_event_v6(&s,&e)==7);
    CHECK(!memcmp(&e,&old_e,sizeof(e)));
    CHECK(s.PC==0x1800 && s.GPR[4]==sp);
    CHECK(s.CS.Selector==(24|new_cpl));
    CHECK(s.CS.Attributes==(0xa09b|(dpl<<5)|(conform<<2)));
    CHECK(s.SS.Selector==(new_cpl<cpl?new_cpl:old.SS.Selector));
    CHECK(s.Unusable==(old.Unusable|(new_cpl<cpl?2:0)));
    CHECK(s.NMIBlocked==(kind==3));
    CHECK(s.CR2==(kind==0?e.FaultAddress:old.CR2));
    CHECK(s.RFlags==(old_flags&~(gate==14?0x34300ULL:0x34100ULL)));
    uint64_t frame[6];memcpy(frame,(void *)(uintptr_t)sp,40+8*error);
    if(error)CHECK(frame[0]==e.ErrorCode);
    CHECK(frame[error]==e.SavedPC && frame[error+1]==old.CS.Selector);
    CHECK(frame[error+2]==(kind==4?(old_flags&~0x10000ULL):old_flags));
    CHECK(frame[error+3]==old_sp && frame[error+4]==old.SS.Selector);
    CHECK((gdt[3]&(1ULL<<40))!=0);
    old.PC=s.PC;old.GPR[4]=s.GPR[4];old.CS=s.CS;old.SS.Selector=s.SS.Selector;
    old.Unusable=s.Unusable;old.NMIBlocked=s.NMIBlocked;old.CR2=s.CR2;old.RFlags=s.RFlags;
    CHECK(!memcmp(&s,&old,sizeof(s)));
    for(unsigned n=0;n<sizeof(stack);++n) {
      uintptr_t a=(uintptr_t)(stack+n);
      if(a<sp||a>=top)CHECK(stack[n]==0xa5);
      a=(uintptr_t)(inner+n);if(a<sp||a>=top)CHECK(inner[n]==0xa5);
    }
  }
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest, EventDeliveryDefersBeforeTouchingGuestRam) {
  load({0x90});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] =
        source(request(R"("event_delivery":true,"instructions":[4096])"), LLVM);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  for(unsigned kind=0;kind<3;++kind)for(unsigned flags=0;flags<64;++flags) {
    neverd_x64_state_v6 s=seed();s.RFlags=2|((flags&1)?512:0);
    s.NMIBlocked=(flags>>1)&1;
    s.GDTR.Base=s.IDTR.Base=s.TR.Base=1; /* must never be dereferenced */
    neverd_x64_event_v1 e={0};e.SavedPC=0x1000;
    e.Kind=kind==0?2:kind==1?3:5;e.Vector=kind==0?32:kind==1?2:1;
    e.STIBlocked=(flags>>2)&1;e.SSBlocked=(flags>>3)&1;
    e.NMIBlockedBySTI=(flags>>4)&1;
    int blocked=kind==0?(!(flags&1)||e.STIBlocked||e.SSBlocked):
        kind==1?(s.NMIBlocked||e.SSBlocked||(e.STIBlocked&&e.NMIBlockedBySTI)):e.SSBlocked;
    if(!blocked)continue;
    neverd_x64_state_v6 old=s;neverd_x64_event_v1 old_e=e;
    CHECK(neverd_x64_deliver_event_v6(&s,&e)==15);
    CHECK(!memcmp(&s,&old,sizeof(s))&&!memcmp(&e,&old_e,sizeof(e)));
  }
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       EventDeliveryRejectsInvalidCauseBeforeEffects) {
  load({0x90});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] =
        source(request(R"("event_delivery":true,"instructions":[4096])"), LLVM);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  for(unsigned i=0;i<12;++i) {
    neverd_x64_state_v6 s=seed(),old=s;
    neverd_x64_event_v1 e={0};e.SavedPC=0x1000;e.Vector=6;
    switch(i) {
    case 0:e.Kind=7;break;case 1:e.Kind=3;break;case 2:e.Kind=1;break;
    case 3:e.Kind=6;break;case 4:e.ErrorCode=1;break;case 5:e.FaultAddress=1;break;
    case 6:e.RepeatedString=1;break;case 7:e.STIBlocked=2;break;
    case 8:e.SSBlocked=2;break;case 9:e.Reserved[1]=1;break;
    case 10:e.Vector=17;e.ErrorCode=1;break;
    case 11:e.Vector=14;e.ErrorCode=0x100000000ULL;break;
    }
    CHECK(neverd_x64_deliver_event_v6(&s,&e)==5);
    CHECK(!memcmp(&s,&old,sizeof(s)));
  }
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest, EventDeliveryMatchesNativeFaultsNmiAndIrq) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "native event qualification requires Linux x64 KVM";
#else
  load({0x90});
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] =
        source(request(R"("event_delivery":true,"instructions":[4096])"), LLVM);
    const auto File = tmpFile(LLVM ? "event-llvm.c" : "event-high.c");
    std::ofstream(File) << Source << Seed << R"C(
#undef CHECK
#define CHECK(c) do {if(!(c)) { __asm__ volatile("hlt"::"a"(__LINE__):"memory"); __builtin_unreachable(); }} while(0)
int main(void) {
  unsigned char before[4096],after[4096];
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned dpl=0;dpl<=cpl;++dpl)
  for(unsigned mode=0;mode<3;++mode)for(unsigned conform=0;conform<2;++conform)for(unsigned ist=0;ist<8;++ist)
  for(unsigned kind=13;kind<=19;++kind)for(unsigned bit=0;bit<2;++bit) {
    const unsigned vectors[]={0,6,13,14,bit?14u:65u,2,14};
    neverd_x64_event_v1 e={0};e.Vector=vectors[kind-13];e.SavedPC=0x1000;
    e.Kind=kind==17?2:kind==18?3:kind==19?4:0;
    if(kind==19)e.SavedPC+=2;
    if(kind==16){e.FaultAddress=0x400000;e.ErrorCode=cpl==3?4:0;}
    neverd_x64_state_v6 s=seed();s.NMIBlocked=0;s.RFlags=0x246|(bit?0x10000:0);
    s.CR2=0xabcdef;
    for(unsigned i=0;i<16;++i)s.GPR[i]=0x1234567800001234ULL+i*17;
    s.CS=(neverd_x64_segment){mode?0x200000:0,UINT32_MAX,8|cpl,
        0x809b|(cpl<<5)|(mode==0?0x2000:mode==1?0x4000:0)};
    s.SS.Selector=16|cpl;s.SS.Attributes=0xc093|(cpl<<5);
    s.GDTR=(neverd_x64_descriptor_table){0x8000,63,0,0};
    s.IDTR=(neverd_x64_descriptor_table){0x8200,0x7ff,0,0};
    s.TR=(neverd_x64_segment){0x8a00,103,48,0x8b};
    s.GPR[4]=0x8f27;s.GPR[1]=kind==16?0x400000:0;
    if(kind==15)s.GPR[0]=0x80000000;
    memset((void *)0x8000,0xa5,4096);
    uint64_t code=0x00af9a000000ffffULL|((uint64_t)conform<<42)|((uint64_t)dpl<<45);
    memcpy((void *)0x8018,&code,8);
    uint64_t low=0x1800|(24ULL<<16)|((uint64_t)ist<<32)|((uint64_t)(0xee|bit)<<40),high=0;
    memcpy((void *)(uintptr_t)(s.IDTR.Base+16*e.Vector),&low,8);
    memcpy((void *)(uintptr_t)(s.IDTR.Base+16*e.Vector+8),&high,8);
    uint64_t sp=0x8e27;memcpy((void *)(uintptr_t)(s.TR.Base+(ist?28+8*ist:4+8*dpl)),&sp,8);
    neverd_x64_state_v6 want=s;
    memcpy(before,(void *)0x8000,4096);
    __asm__ volatile("outb %%al,$0xe9"::"a"(kind),"D"(&want),"S"((uint64_t)e.Vector):"memory");
    memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
    CHECK(neverd_x64_deliver_event_v6(&s,&e)==7);
    CHECK(!memcmp(&s,&want,sizeof(s)));
    CHECK(!memcmp((void *)0x8000,after,4096));
  }
  return 0;
}
)C";
    for (const char *Optimization : {"O0", "O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Ran =
          exec("python3",
               {std::string(TEST_SOURCE_DIR) + "/run_x64_continuation_kvm.py",
                "--clang", NEVERD_TEST_CLANG, "--source", File.string(),
                "--optimization", Optimization});
      if (Ran.exitCode == 77)
        GTEST_SKIP() << Ran.err;
      ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
    }
    if (const char *Directory =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Directory);
      const auto Prefix = LLVM ? "event-llvm" : "event-high";
      std::ofstream(fs::path(Directory) / (std::string(Prefix) + ".json"))
          << Manifest;
      for (const auto &Entry : fs::directory_iterator(tmp()))
        if (Entry.is_regular_file() &&
            Entry.path().filename().string().starts_with(Prefix))
          fs::copy_file(Entry.path(),
                        fs::path(Directory) / Entry.path().filename(),
                        fs::copy_options::overwrite_existing);
    }
  }
#endif
}

TEST_F(X64ContinuationExportTest, EventDeliveryRoutesOriginalUd2AndDivFaults) {
  load({0x0f, 0x0b, 0x48, 0xf7, 0xf1});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(
        request(R"("event_delivery":true,"instructions":[4096,4098])"), LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  uint64_t gdt[4]={0,0,0,0x00af9b000000ffffULL},idt[14]={0};
  unsigned char stack[256];
  for(unsigned test=0;test<4;++test) {
    neverd_x64_state_v6 s=seed();s.PC=test?0x1002:0x1000;
    s.GPR[4]=(uintptr_t)(stack+239);s.GPR[0]=10;
    s.GPR[1]=test==1?0:3;s.GPR[2]=test==2?3:0;
    s.GDTR.Base=(uintptr_t)gdt;s.GDTR.Limit=sizeof(gdt)-1;
    s.IDTR.Base=(uintptr_t)idt;s.IDTR.Limit=sizeof(idt)-1;
    idt[0]=idt[12]=0x1800|(24ULL<<16)|(0x8eULL<<40);
    neverd_x64_state_v6 old=s;uint64_t budget=1;
    uint64_t status=neverd_x64_continue_v6(&s,&budget);
    CHECK(budget==0);
    if(test==3) {
      CHECK(status==9 && s.PC==0x1005 && s.GPR[0]==3 && s.GPR[2]==1);
      continue;
    }
    CHECK(status==7 && s.PC==0x1800);
    uint64_t frame[5];memcpy(frame,(void *)(uintptr_t)s.GPR[4],40);
    CHECK(frame[0]==old.PC && frame[2]==(old.RFlags|0x10000));
    CHECK(s.GPR[0]==old.GPR[0] && s.GPR[1]==old.GPR[1] && s.GPR[2]==old.GPR[2]);
    CHECK(s.NMIBlocked==old.NMIBlocked);
  }
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       EventDeliveryRequiresItsProfileAndSharedBudget) {
  load({0x90});
  for (
      const auto &Request :
      {request(R"("event_delivery":1,"instructions":[4096])"),
       std::string(
           R"({"profile":"intel-mpx-v11","mxcsr_mask":"0xffbf","event_delivery":true,"instructions":[4096]})"),
       request(
           R"("event_delivery":true,"max_operations":256,"instructions":[4096])")}) {
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), false, &Report));
    EXPECT_TRUE(Source.empty());
    EXPECT_TRUE(take(Report).empty());
    EXPECT_FALSE(take(neverd_last_error(Session)).empty());
  }
  auto [Source, Manifest] =
      source(request(R"("max_operations":256,"instructions":[4096])"), false);
  EXPECT_FALSE(Source.empty());
  auto [WithEvent, EventManifest] =
      source(request(R"("event_delivery":true,"instructions":[4096])"), false);
  auto Parsed = llvm::json::parse(EventManifest);
  ASSERT_TRUE(bool(Parsed));
  const auto Work = Parsed->getAsObject()->getInteger("lowered_operations");
  ASSERT_TRUE(Work.has_value());
  for (unsigned Minus : {0u, 1u}) {
    const auto Request = request(
        "\"event_delivery\":true,\"instructions\":[4096],\"max_operations\":" +
        std::to_string(*Work - Minus));
    const char *Report = nullptr;
    auto Result = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), false, &Report));
    EXPECT_EQ(Result.empty(), Minus == 1);
    EXPECT_EQ(take(Report).empty(), Minus == 1);
  }
}
