//===- X64KvmContinuationExportTests.inc - Bound hypercall source -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.

TEST_F(X64ContinuationExportTest, KvmHypercallsRequireAnExplicitAbi) {
  load({0x0f, 0x01, 0xc1, 0x0f, 0x01, 0xd9, 0xc3});
  const std::string Request = R"JSON({"profile":"kernel-events-v6",
    "mxcsr_mask":"0xffff","paging_address_bits":48,
    "instructions":[4096,4099]})JSON";
  for (bool LLVM : {false, true}) {
    auto [Code, Report] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 2);
    EXPECT_EQ(Code.find("__builtin_ia32_vmcall_kvm"), std::string::npos);
    execute(Code, R"C(
#include <stdlib.h>
int main(void) {
  for(unsigned i=0;i<2;++i) {
    neverd_x64_state_v6 s={0};s.RFlags=2;s.PC=0x1000+3*i;s.MXCSR=0x1f80;
    s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
    s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
    neverd_x64_state_v6 old=s;uint64_t budget=4;
    if(neverd_x64_continue_v6(&s,&budget)!=4 || budget!=4 ||
       memcmp(&s,&old,sizeof(s)))abort();
  }
  return 0;
})C");
  }
}

TEST_F(X64ContinuationExportTest, KvmHypercallsTransportAllFiveInputs) {
  load({0x0f, 0x01, 0xc1, 0x48, 0x89, 0xc7, // vmcall; mov rdi,rax
        0x0f, 0x01, 0xd9, 0xc3});           // vmmcall; ret
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x))abort();} while(0)
static uint64_t observed[5],memory_effect;
static unsigned calls;
static uint64_t kvm_complete(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
  observed[0]=nr;observed[1]=a;observed[2]=b;observed[3]=c;observed[4]=d;
  ++calls;memory_effect=nr^a^b^c^d;
  return nr+3*a+5*b+7*c+11*d;
}
#define __builtin_ia32_vmcall_kvm kvm_complete
#define __builtin_ia32_vmmcall_kvm kvm_complete
)C";
  const char Harness[] = R"C(
int main(void) {
  const unsigned map[]={0,3,1,2,6};
  for(unsigned form=0;form<2;++form)for(unsigned seed=0;seed<128;++seed) {
    neverd_x64_state_v6 s={0};
    for(unsigned i=0;i<16;++i)s.GPR[i]=0x1234567800000000ULL+seed*71+i*13;
    for(unsigned i=0;i<sizeof(s.ZMM);++i)((uint8_t *)s.ZMM)[i]=(uint8_t)(seed+i*7);
    s.RFlags=0x246;s.MXCSR=0x1f80;s.PC=0x1000+form*6;s.CR2=seed;
    s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
    s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
    neverd_x64_state_v6 expected=s;
    uint64_t inputs[5];for(unsigned i=0;i<5;++i)inputs[i]=s.GPR[map[i]];
    expected.GPR[0]=inputs[0]+3*inputs[1]+5*inputs[2]+7*inputs[3]+11*inputs[4];
    expected.PC+=3;calls=0;memory_effect=0;uint64_t budget=1;
    CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0 && calls==1);
    CHECK(!memcmp(&s,&expected,sizeof(s)) && !memcmp(inputs,observed,sizeof(inputs)));
    CHECK(memory_effect==(inputs[0]^inputs[1]^inputs[2]^inputs[3]^inputs[4]));
  }
  neverd_x64_state_v6 s={0};s.PC=0x1000;s.RFlags=2;s.MXCSR=0x1f80;
  s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
  s.GPR[0]=1;s.GPR[3]=2;s.GPR[1]=3;s.GPR[2]=4;s.GPR[6]=5;
  uint64_t stack[]={0x7000};s.GPR[4]=(uint64_t)stack;
  uint64_t budget=4;calls=0;
  CHECK(neverd_x64_continue_v6(&s,&budget)==3 && budget==0 && calls==2);
  CHECK(s.GPR[7]==105 && s.GPR[0]==209 && s.PC==0x7000);
  return 0;
})C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      const std::string Request =
          std::string(R"JSON({"profile":"kernel-events-v6",
        "mxcsr_mask":"0xffff","paging_address_bits":48,"native_kvm_hypercalls":true,
        "entries":[4096],"follow_internal_calls":true,"dispatch":")JSON") +
          Dispatch + "\"}";
      auto [Code, Report] = source(Request, LLVM);
      EXPECT_NE(Code.find("__builtin_ia32_vmcall_kvm"), std::string::npos);
      EXPECT_NE(Code.find("__builtin_ia32_vmmcall_kvm"), std::string::npos);
      auto Parsed = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      EXPECT_NE(Report.find("hypercall_successors"), std::string::npos);
      execute(std::string(Provider) + Code, Harness);
      if (std::string(Dispatch) == "switch")
        if (const auto *Directory =
                std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
          const std::string Base =
              std::string(Directory) + (LLVM ? "/kvm-llvm" : "/kvm-high");
          std::ofstream(Base + ".c") << Code;
          std::ofstream(Base + ".json") << Report;
        }
    }
}
