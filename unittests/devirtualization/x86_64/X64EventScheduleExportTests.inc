//===- X64EventScheduleExportTests.inc - IRQ/NMI boundary scheduling -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Persistent shadows, admission, budgets and exactly-once acknowledgment.
//===----------------------------------------------------------------------===//

const char ScheduledFixture[] = R"C(
static uint64_t schedule_gdt[8],schedule_idt[512];
static unsigned char schedule_tss[128],schedule_stack[1024];
typedef struct {unsigned polls,acks,ready_after,kind,vector;int result;} producer_t;
static neverd_x64_state_v6 scheduled_seed(uint64_t pc) {
  neverd_x64_state_v6 s=seed();s.PC=pc;s.NMIBlocked=0;
  memset(schedule_gdt,0,sizeof(schedule_gdt));memset(schedule_idt,0,sizeof(schedule_idt));
  memset(schedule_tss,0,sizeof(schedule_tss));memset(schedule_stack,0xa5,sizeof(schedule_stack));
  schedule_gdt[3]=0x00af9b000000ffffULL;
  for(unsigned v=0;v<256;++v)schedule_idt[2*v]=0x1800|(24ULL<<16)|(0x8eULL<<40);
  s.GDTR=(neverd_x64_descriptor_table){(uintptr_t)schedule_gdt,sizeof(schedule_gdt)-1,0,0};
  s.IDTR=(neverd_x64_descriptor_table){(uintptr_t)schedule_idt,sizeof(schedule_idt)-1,0,0};
  s.TR=(neverd_x64_segment){(uintptr_t)schedule_tss,sizeof(schedule_tss)-1,48,0x8b};
  s.GPR[4]=(uintptr_t)(schedule_stack+1000);
  return s;
}
static int scheduled_peek(const neverd_x64_state_v6 *s,
    const neverd_x64_interruptibility_v1 *shadow,
    neverd_x64_pending_interrupt_v1 *pending,void *context) {
  producer_t *p=context;(void)s;(void)shadow;++p->polls;
  pending->Kind=p->kind;pending->Vector=p->vector;
  return p->polls>=p->ready_after?p->result:0;
}
static void scheduled_accept(const neverd_x64_pending_interrupt_v1 *pending,void *context) {
  producer_t *p=context;CHECK(pending->Kind==p->kind&&pending->Vector==p->vector);
  ++p->acks;p->result=0;
}
static uint64_t scheduled(neverd_x64_state_v6 *s,uint64_t *budget,
    neverd_x64_interruptibility_v1 *shadow,uint64_t *poll_budget,producer_t *p) {
  return neverd_x64_continue_events_v6(s,budget,shadow,poll_budget,
                                     scheduled_peek,scheduled_accept,p);
}
static uint64_t saved_pc(const neverd_x64_state_v6 *s) {
  uint64_t pc;memcpy(&pc,(void *)(uintptr_t)s->GPR[4],8);return pc;
}
)C";

TEST_F(X64ContinuationExportTest,
       ScheduledEventsRetainShadowsAcrossResumption) {
  load({0xfb, 0xfb, 0x90, 0xfa, 0xfb, 0xeb, 0, 0x90, 0x0f, 0x0b, 0x0f, 0xa2,
        0x90});
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      const auto [Source, Manifest] = source(
          request(
              std::string(
                  R"("event_delivery":true,"scheduled_events":true,"dispatch":")") +
              Dispatch +
              R"(","instructions":[4096,4097,4098,4099,4100,4101,4103,4104,4106,4108])"),
          LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->getAsObject()->getBoolean("scheduled_events"), true);
      execute(Source, std::string(Seed) + ScheduledFixture + R"C(
int main(void) {
  for(unsigned pattern=0;pattern<2;++pattern) {
    neverd_x64_state_v6 s=scheduled_seed(0x1000);s.RFlags=2|(pattern?0x40000:0);
    neverd_x64_interruptibility_v1 shadow={0};
    producer_t p={0,0,1,2,65,1};uint64_t polls=20,budget=1;
    CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==9);
    CHECK(!budget&&s.PC==0x1001&&shadow.STIBlocked==1&&p.polls==1&&!p.acks);
    budget=1;CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==9);
    CHECK(!budget&&s.PC==0x1002&&!shadow.STIBlocked&&p.polls==2&&!p.acks);
    budget=1;CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
    CHECK(budget==1&&s.PC==0x1800&&saved_pc(&s)==0x1002&&p.acks==1&&polls==17);
    CHECK(!shadow.STIBlocked&&!shadow.SSBlocked);
  }
  /* IF was already one: STI does not start a new inhibition window. */
  neverd_x64_state_v6 s=scheduled_seed(0x1000);
  neverd_x64_interruptibility_v1 shadow={0};producer_t p={0,0,2,2,65,1};
  uint64_t budget=8,polls=8;
  CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
  CHECK(saved_pc(&s)==0x1001&&budget==7&&p.polls==2&&p.acks==1);
  /* CLI in the inhibited slot keeps IRQ pending. A later STI/JMP consumes
     exactly one new shadow, including an original control transfer. */
  s=scheduled_seed(0x1003);shadow=(neverd_x64_interruptibility_v1){1,0,0,{0}};
  p=(producer_t){0,0,1,2,65,1};budget=8;polls=8;
  CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
  CHECK(saved_pc(&s)==0x1007&&budget==5&&p.polls==4&&p.acks==1);
  /* An exception ends inhibition but never acknowledges the deferred IRQ. */
  s=scheduled_seed(0x1008);shadow=(neverd_x64_interruptibility_v1){0,1,0,{0}};
  p=(producer_t){0,0,1,2,65,1};budget=8;polls=8;
  CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
  CHECK(saved_pc(&s)==0x1008&&budget==7&&p.polls==1&&!p.acks);
  CHECK(!shadow.STIBlocked&&!shadow.SSBlocked&&p.result==1);
  return 0;
}
)C");
      if (const char *Directory =
              std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
        fs::create_directories(Directory);
        const std::string Name = std::string("scheduled-events-") +
                                 (LLVM ? "llvm-" : "high-") + Dispatch;
        fs::copy_file(tmpFile("run.c"), fs::path(Directory) / (Name + ".c"),
                      fs::copy_options::overwrite_existing);
        std::ofstream(fs::path(Directory) / (Name + ".json")) << Manifest;
      }
    }
}

TEST_F(X64ContinuationExportTest,
       ScheduledEventsOwnNmiPolicyAndIncomingSsShadow) {
  load({0xfb, 0x90, 0x90});
  for (bool LLVM : {false, true}) {
    const auto [Source, Manifest] = source(
        request(
            R"("event_delivery":true,"scheduled_events":true,"instructions":[4096,4097,4098])"),
        LLVM);
    execute(Source, std::string(Seed) + ScheduledFixture + R"C(
int main(void) {
  for(unsigned policy=0;policy<2;++policy) {
    neverd_x64_state_v6 s=scheduled_seed(0x1000);s.RFlags=2;
    neverd_x64_interruptibility_v1 shadow={0,0,policy,{0}};
    producer_t p={0,0,2,3,2,1};uint64_t budget=8,polls=8;
    CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
    CHECK(saved_pc(&s)==0x1001+policy&&budget==7-policy&&p.polls==2+policy&&p.acks==1);
    CHECK(s.NMIBlocked&&!shadow.STIBlocked&&!shadow.SSBlocked);
  }
  for(unsigned nmi=0;nmi<2;++nmi) {
    neverd_x64_state_v6 s=scheduled_seed(0x1001);
    neverd_x64_interruptibility_v1 shadow={0,1,0,{0}};
    producer_t p={0,0,1,nmi?3:2,nmi?2:65,1};uint64_t budget=8,polls=8;
    CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
    CHECK(saved_pc(&s)==0x1002&&budget==7&&p.polls==2&&p.acks==1);
  }
  /* An already blocked NMI remains pending; neither IF nor the end of an
     instruction clears NMI blocking. No IDT access occurs while inhibited. */
  neverd_x64_state_v6 s=scheduled_seed(0x1001);s.NMIBlocked=1;s.IDTR.Base=1;
  neverd_x64_interruptibility_v1 shadow={0};
  producer_t p={0,0,1,3,2,1};uint64_t budget=2,polls=2;
  CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==9);
  CHECK(!budget&&!polls&&p.polls==2&&!p.acks&&s.PC==0x1003&&s.NMIBlocked);
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       ScheduledEventsRejectWithoutConsumingInstruction) {
  load({0x90, 0x0f, 0xa2});
  for (bool LLVM : {false, true}) {
    const auto [Source, Manifest] = source(
        request(
            R"("event_delivery":true,"scheduled_events":true,"instructions":[4096,4097])"),
        LLVM);
    execute(Source, std::string(Seed) + ScheduledFixture + R"C(
int main(void) {
  for(unsigned test=0;test<13;++test) {
    neverd_x64_state_v6 s=scheduled_seed(0x1000);
    neverd_x64_interruptibility_v1 shadow={0};
    producer_t p={0,0,1,2,65,1};uint64_t budget=1,polls=1,status=5,seen=0;
    switch(test) {
    case 0:budget=0;status=9;break;
    case 1:polls=0;status=16;break;
    case 2:s.RFlags=0;break;
    case 3:shadow.STIBlocked=2;break;
    case 4:shadow.SSBlocked=2;break;
    case 5:shadow.NMIBlockedBySTI=2;break;
    case 6:shadow.Reserved[4]=1;break;
    case 7:shadow.STIBlocked=1;s.RFlags=2;break;
    case 8:s.PC=0x2000;status=8;break;
    case 9:p.result=2;seen=1;break;
    case 10:p.kind=0;seen=1;break;
    case 11:p.kind=3;seen=1;break;
    case 12:s.IDTR.Limit=0;seen=1;break;
    }
    neverd_x64_state_v6 old=s;neverd_x64_interruptibility_v1 old_shadow=shadow;
    const uint64_t old_budget=budget,old_polls=polls;
    CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==status);
    CHECK(!memcmp(&s,&old,sizeof(s))&&!memcmp(&shadow,&old_shadow,sizeof(shadow)));
    CHECK(budget==old_budget&&polls==old_polls-seen&&p.polls==seen&&!p.acks);
  }
  neverd_x64_state_v6 s=scheduled_seed(0x1001),old=s;
  neverd_x64_interruptibility_v1 shadow={1,1,0,{0}},old_shadow=shadow;
  producer_t p={0,0,1,2,65,0};uint64_t budget=1,polls=2;
  CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==4);
  CHECK(!memcmp(&s,&old,sizeof(s))&&!memcmp(&shadow,&old_shadow,sizeof(shadow)));
  CHECK(budget==1&&polls==1&&p.polls==1&&!p.acks);
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       ScheduledEventsObservePhysicalCallsBeforeCallee) {
  load({0xfb, 0xe8, 2, 0, 0, 0, 0x90, 0x90, 0xc3});
  for (bool LLVM : {false, true})
    for (bool Follow : {false, true}) {
      const auto [Source, Manifest] = source(
          request(
              std::string(
                  R"("event_delivery":true,"scheduled_events":true,"follow_internal_calls":)") +
              (Follow ? "true" : "false") +
              R"(,"instructions":[4096,4097,4102,4104])"),
          LLVM);
      execute(Source, std::string(Seed) + ScheduledFixture +
                          "\n#define FOLLOW " + std::to_string(Follow) + R"C(
int main(void) {
  neverd_x64_state_v6 s=scheduled_seed(0x1000);s.RFlags=2;
  neverd_x64_interruptibility_v1 shadow={0};
  producer_t p={0,0,1,2,65,1};uint64_t budget=8,polls=8,old_sp=s.GPR[4];
  CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==(FOLLOW?7:1));
  CHECK(budget==6&&!shadow.STIBlocked&&!shadow.SSBlocked);
  uint64_t return_pc;memcpy(&return_pc,(void *)(uintptr_t)(old_sp-8),8);
  CHECK(return_pc==0x1006&&p.acks==FOLLOW);
  if(!FOLLOW) {
    CHECK(s.PC==0x1008&&s.GPR[4]==old_sp-8);
    CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
  }
  CHECK(saved_pc(&s)==0x1008&&budget==6&&p.polls==3&&p.acks==1);
  return 0;
}
)C");
    }
}

TEST_F(X64ContinuationExportTest,
       ScheduledEventsRequireExplicitCompatibleBinding) {
  load({0x90});
  for (
      const char *Options :
      {R"("scheduled_events":true,"instructions":[4096])",
       R"("scheduled_events":true,"event_delivery":true,"native_rtm_regions":true,"instructions":[4096])",
       R"("scheduled_events":true,"event_delivery":true,"native_rtm_abort_regions":true,"instructions":[4096])"}) {
    const auto Request = request(Options);
    const char *Report = nullptr;
    EXPECT_TRUE(
        take(neverd_x64_continuation_source_v1(Session, Request.data(),
                                               Request.size(), false, &Report))
            .empty());
    EXPECT_TRUE(take(Report).empty());
  }
}

TEST_F(X64ContinuationExportTest, ScheduledEventsMatchNativeInterruptWindows) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "native interrupt scheduling requires Linux x64 KVM";
#else
  struct Original {
    std::vector<uint8_t> Bytes;
    std::vector<unsigned> Starts;
    unsigned Steps, Shadow, NMI;
  };
  const Original Originals[] = {
      {{0xfb, 0x90, 0x90}, {0, 1, 2}, 2, 0, 0},
      {{0xfb, 0xfb, 0x90}, {0, 1, 2}, 2, 0, 0},
      {{0xfb, 0xfa, 0xfb, 0x90, 0x90}, {0, 1, 2, 3, 4}, 4, 0, 0},
      {{0xfb, 0xeb, 0, 0x90}, {0, 1, 3}, 2, 0, 0},
      {{0xfb, 0xe8, 2, 0, 0, 0, 0x90, 0x90, 0xc3}, {0, 1, 6, 7, 8}, 2, 0, 0},
      {{0x90, 0x90}, {0, 1}, 1, 2, 0}, // Incoming KVM STI shadow.
      {{0x90, 0x90}, {0, 1}, 1, 1, 0}, // Incoming MOV-SS shadow.
      {{0x90, 0x90}, {0, 1}, 1, 1, 1}, // MOV-SS also blocks NMI.
  };
  std::vector<uint8_t> Code;
  std::string Entries, Cases = R"C(
static const struct {uint64_t pc;unsigned steps;unsigned char request[19];} originals[]={
)C";
  for (const auto &Original : Originals) {
    const uint64_t PC = 0x1000 + Code.size();
    for (auto Offset : Original.Starts) {
      if (!Entries.empty())
        Entries += ',';
      Entries += std::to_string(PC + Offset);
    }
    Cases += "{" + std::to_string(PC) + "ULL," +
             std::to_string(Original.Steps) + ",{" +
             std::to_string(Original.Bytes.size()) + "," +
             std::to_string(Original.NMI ? 3 : 2) + "," +
             std::to_string(Original.NMI ? 2 : 65) + "," +
             std::to_string(Original.Shadow);
    for (auto Byte : Original.Bytes)
      Cases += "," + std::to_string(Byte);
    Cases += "}},\n";
    Code.insert(Code.end(), Original.Bytes.begin(), Original.Bytes.end());
    Code.push_back(0x90); // Keep each original sequence's stop independent.
  }
  Cases += "};\n";
  load(Code);
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(
        request(
            R"("event_delivery":true,"scheduled_events":true,"follow_internal_calls":true,"instructions":[)" +
            Entries + "]"),
        LLVM);
    const auto File =
        tmpFile(LLVM ? "schedule-native-llvm.c" : "schedule-native-high.c");
    std::ofstream(File) << Source << Seed << ScheduledFixture << Cases << R"C(
#undef CHECK
#define CHECK(c) do {if(!(c)) { __asm__ volatile("hlt"::"a"(__LINE__),"b"(test):"memory"); __builtin_unreachable(); }} while(0)
int main(void) {
  unsigned char before[4096],after[4096];unsigned comparisons=0;
  for(unsigned test=0;test<sizeof(originals)/sizeof(*originals);++test)
  for(unsigned alignment=0;alignment<16;++alignment)for(unsigned trap=0;trap<2;++trap)
  for(unsigned flags=0;flags<4;++flags) {
    neverd_x64_state_v6 s=seed();s.PC=originals[test].pc;s.NMIBlocked=0;
    s.RFlags=2|(flags&1?0x8d5:0)|(flags&2?0x40000:0)|(originals[test].request[3]?512:0);
    s.GDTR=(neverd_x64_descriptor_table){0x8000,63,0,0};
    s.IDTR=(neverd_x64_descriptor_table){0x8200,0x7ff,0,0};
    s.TR=(neverd_x64_segment){0x8a00,103,48,0x8b};s.GPR[4]=0x8f00+alignment;
    memset((void *)0x8000,0xa5,4096);
    uint64_t descriptor=0x00af9a000000ffffULL,gate=0x1800|(24ULL<<16)|((0x8eULL|trap)<<40),zero=0;
    const unsigned vector=originals[test].request[2];
    memcpy((void *)0x8018,&descriptor,8);memcpy((void *)(uintptr_t)(0x8200+16*vector),&gate,8);
    memcpy((void *)(uintptr_t)(0x8208+16*vector),&zero,8);
    neverd_x64_state_v6 want=s;memcpy(before,(void *)0x8000,4096);
    __asm__ volatile("outb %%al,$0xe9"::"a"(20),"D"(&want),"S"(originals[test].request):"memory");
    memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
    neverd_x64_interruptibility_v1 shadow={originals[test].request[3]==2,
                                         originals[test].request[3]==1,0,{0}};
    producer_t p={0,0,1,originals[test].request[1],vector,1};
    uint64_t budget=32,polls=32;
    CHECK(scheduled(&s,&budget,&shadow,&polls,&p)==7);
    CHECK(budget==32-originals[test].steps&&p.polls==originals[test].steps+1&&p.acks==1);
    CHECK(!shadow.STIBlocked&&!shadow.SSBlocked);
    CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp((void *)0x8000,after,4096));
    ++comparisons;
  }
  return comparisons==1024?0:1;
}
)C";
    for (const char *Optimization : {"O0", "O2"}) {
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
      const std::string Prefix =
          LLVM ? "schedule-native-llvm" : "schedule-native-high";
      std::ofstream(fs::path(Directory) / (Prefix + ".json")) << Manifest;
      std::ofstream(fs::path(Directory) / (Prefix + ".source.c")) << Source;
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
