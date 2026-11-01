//===- X64RtmAbortExportTests.inc - Bounded native RTM providers ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact pair selection, unsupported-host refusal and complete resumption.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationExportTest, RtmAbortRegionsRequireExactAdjacentBytes) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0xc7, 0xf8, 3, 0, 0, 0, 0xc6, 0xf8, 0xff, 0xc3},
      {0xc7, 0xf8, 4, 0, 0, 0, 0x90, 0xc6, 0xf8, 0xff, 0xc3},
      {0xc7, 0xf8, 3, 0, 0, 0, 0x0f, 0x01, 0xd5, 0xc3},
      {0xc7, 0xf8, 3, 0, 0, 0, 0xc6, 0xf8},
      {0x66, 0xc7, 0xf8, 3, 0, 0xc6, 0xf8, 0xff, 0xc3}};
  for (unsigned I = 0; I < Cases.size(); ++I) {
    load(Cases[I]);
    for (bool LLVM : {false, true}) {
      auto [Source, Manifest] = source(
          request(R"("instructions":[4096],"native_rtm_abort_regions":true)"),
          LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getInteger("native_requests"), 1);
      const auto *Binding = Object->getObject("native_rtm_abort_environment");
      ASSERT_NE(Binding, nullptr);
      const auto *Regions = Binding->getArray("regions");
      ASSERT_NE(Regions, nullptr);
      EXPECT_EQ(Regions->size(), I == 0 ? 1u : 0u);
      if (I == 0) {
        const auto *Region = (*Regions)[0].getAsObject();
        ASSERT_NE(Region, nullptr);
        EXPECT_EQ(Region->getString("bytes"), "C7F803000000C6F8FF");
        EXPECT_EQ(Region->getString("fallback"), "0x1009");
        EXPECT_EQ(Region->getInteger("abort_code"), 255);
      }
      execute(Source, std::string(Seed) + R"C(
int main(void) {
  neverd_x64_state_v6 s=seed(),before=s;
  CHECK(!neverd_x64_rtm_abort_environment_v6(&s,3,0));
  CHECK(!memcmp(&s,&before,sizeof(s)));
  s.PC=0x12345678;before=s;
  CHECK(!neverd_x64_rtm_abort_environment_v6(&s,4,0));
  CHECK(!memcmp(&s,&before,sizeof(s)));
  CHECK(!neverd_x64_rtm_abort_environment_v6(0,4,0));
  return 0;
})C");
      auto [Plain, Ignored] = source(request(R"("instructions":[4096])"), LLVM);
      EXPECT_EQ(Plain.find("neverd_x64_rtm_abort_environment_v6"),
                std::string::npos);
    }
  }
}

TEST_F(X64ContinuationExportTest, RtmAbortRegionsResumeWithIndependentBudgets) {
  // The pair owns its transaction; selected XABORT remains a separate native
  // boundary when entered independently. The callback resumes at the real RET.
  load({0xc7, 0xf8, 3, 0, 0, 0, 0xc6, 0xf8, 0xa7, 0xc3}, {{0x1000, 10}});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(
        request(
            R"("entries":[4096],"native_rtm_abort_regions":true,"follow_internal_calls":true,"dispatch":"direct")"),
        LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 3);
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 2);
    execute(Source, std::string(Seed) + R"C(
#include <cpuid.h>
int main(void) {
  unsigned a,b,c,d;
  const int supported=__get_cpuid_count(7,0,&a,&b,&c,&d) && (b&(1u<<11));
  const unsigned flag_bits[]={0,2,4,6,7,11};
  for(unsigned pattern=0;pattern<64;++pattern) {
    neverd_x64_state_v6 s=seed();uint64_t stack[9],saved_stack[9];
    for(unsigned i=0;i<9;++i)stack[i]=0x123456789abcdef0ULL+i;
    stack[4]=0x2000;s.GPR[4]=(uintptr_t)&stack[4];
    s.RFlags=0x202|((pattern&1)<<10);
    for(unsigned bit=0;bit<6;++bit)s.RFlags|=(uint64_t)((pattern>>bit)&1)<<flag_bits[bit];
    neverd_x64_state_v6 before=s,want=s;memcpy(saved_stack,stack,sizeof(stack));
    uint64_t budget=0,environment=1;
    CHECK(neverd_x64_run_v6(&s,&budget,&environment,neverd_x64_rtm_abort_environment_v6,0)==9);
    CHECK(environment==1 && !memcmp(&s,&before,sizeof(s)));
    budget=2;environment=0;
    CHECK(neverd_x64_run_v6(&s,&budget,&environment,neverd_x64_rtm_abort_environment_v6,0)==4);
    CHECK(budget==2 && !memcmp(&s,&before,sizeof(s)));
    environment=1;
    uint64_t result=neverd_x64_run_v6(&s,&budget,&environment,neverd_x64_rtm_abort_environment_v6,0);
    CHECK(environment==0);
    if(supported) {
      CHECK(result==3 && budget==1 && !(s.GPR[0]>>32));
      uint32_t status=s.GPR[0];CHECK(!(status&0x00ffffc0U));
      if(status&1)CHECK((status>>24)==0xa7 && !(status&2));
      else CHECK(!(status>>24));
      want.GPR[0]=status;want.GPR[4]+=8;want.PC=stack[4];
      CHECK(!memcmp(&s,&want,sizeof(s)));
    } else CHECK(result==4 && budget==2 && !memcmp(&s,&before,sizeof(s)));
    CHECK(!memcmp(stack,saved_stack,sizeof(stack)));
  }
  return 0;
})C");
  }
}

TEST_F(X64ContinuationExportTest, RtmAbortLoopConsumesEveryProviderAttempt) {
  load({0xc7, 0xf8, 0xfa, 0xff, 0xff, 0xff, 0xc6, 0xf8, 0, 0xc3});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(
        request(R"("instructions":[4096],"native_rtm_abort_regions":true)"),
        LLVM);
    execute(Source, std::string(Seed) + R"C(
#include <cpuid.h>
int main(void) {
  unsigned a,b,c,d;
  const int supported=__get_cpuid_count(7,0,&a,&b,&c,&d) && (b&(1u<<11));
  neverd_x64_state_v6 s=seed(),before=s;uint64_t budget=7,environment=3;
  CHECK(neverd_x64_run_v6(&s,&budget,&environment,neverd_x64_rtm_abort_environment_v6,0)==4);
  CHECK(budget==7 && s.PC==before.PC);
  if(supported) {
    CHECK(!environment && !(s.GPR[0]&~0x3fULL));
    before.GPR[0]=s.GPR[0];
  } else CHECK(environment==2);
  CHECK(!memcmp(&s,&before,sizeof(s)));
  return 0;
})C");
  }
}
