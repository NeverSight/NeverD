//===- X64HyperVContinuationExportTests.inc - Hyper-V page source -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.

TEST_F(X64ContinuationExportTest,
       HyperVPageRetainsRepFastOutputsAndOtherState) {
  load({0x0f, 0x01, 0xc1, 0x0f, 0x01, 0xd9, 0xc3});
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static uint64_t observed[15],ram;
static void hyperv(void *entry,void *gpr,void *vectors,unsigned stride) {
  if((uintptr_t)entry!=0x62000000 || stride!=64)abort();
  uint64_t *g=gpr;unsigned char *v=vectors;
  observed[0]=g[1];observed[1]=g[2];observed[2]=g[8];
  for(unsigned i=0;i<6;++i)memcpy(observed+3+i*2,v+i*stride,16);
  ram=0;for(unsigned i=0;i<15;++i)ram^=observed[i];++calls;
  g[0]=ram;
  if(g[1]&(0xfffull<<32))g[1]+=1ull<<48;
  if(g[1]&(1u<<16)) {
    g[2]^=0xaabbccdd12345678ull;g[8]+=0x123456789abcdef0ull;
    for(unsigned i=0;i<6;++i)for(unsigned j=0;j<16;++j)v[i*stride+j]^=i*16+j;
  }
}
#define __builtin_ia32_hyperv_call hyperv
)C";
  const char Harness[] = R"C(
int main(void) {
  for(unsigned form=0;form<2;++form)for(unsigned mode=0;mode<4;++mode)
    for(unsigned seed=0;seed<128;++seed) {
      neverd_x64_state_v6 s={0};
      for(unsigned i=0;i<16;++i)s.GPR[i]=0x123456789abcdef0ull+seed*71+i*13;
      for(unsigned i=0;i<sizeof(s.ZMM);++i)((uint8_t*)s.ZMM)[i]=(uint8_t)(seed+i*7);
      s.GPR[1]=0x50|((uint64_t)(mode&1)<<16)|((uint64_t)(mode>>1)<<32);
      s.RFlags=0x246;s.PC=0x1000+3*form;s.MXCSR=0x1f80;s.CR2=seed;
      s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
      s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
      neverd_x64_state_v6 expected=s;uint64_t inputs[15];
      inputs[0]=s.GPR[1];inputs[1]=s.GPR[2];inputs[2]=s.GPR[8];
      uint64_t checksum=inputs[0]^inputs[1]^inputs[2];
      for(unsigned i=0;i<6;++i){
        memcpy(inputs+3+i*2,s.ZMM[i],16);checksum^=inputs[3+i*2]^inputs[4+i*2];
      }
      expected.GPR[0]=checksum;expected.PC+=3;
      if(mode&2)expected.GPR[1]+=1ull<<48;
      if(mode&1){
        expected.GPR[2]^=0xaabbccdd12345678ull;
        expected.GPR[8]+=0x123456789abcdef0ull;
        for(unsigned i=0;i<6;++i)for(unsigned j=0;j<16;++j)expected.ZMM[i][j]^=i*16+j;
      }
      uint64_t budget=1;calls=0;ram=0;
      if(neverd_x64_continue_v6(&s,&budget)!=9 || budget || calls!=1 ||
         memcmp(&s,&expected,sizeof(s)) || memcmp(inputs,observed,sizeof(inputs)) ||
         ram!=checksum)abort();
    }
  neverd_x64_state_v6 s={0};s.PC=0x1000;s.RFlags=2;s.MXCSR=0x1f80;
  s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};s.GPR[1]=0x51;
  neverd_x64_state_v6 old=s;uint64_t budget=3;calls=0;
  if(neverd_x64_continue_v6(&s,&budget)!=5 || budget!=3 || calls || memcmp(&s,&old,sizeof(s)))abort();
  budget=0;s.GPR[1]=0x50;old=s;
  if(neverd_x64_continue_v6(&s,&budget)!=9 || budget || calls || memcmp(&s,&old,sizeof(s)))abort();
  return 0;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      auto [Code, Report] = source(
          request(std::string(R"("entries":[4096],"follow_internal_calls":true,
            "native_hyperv_hypercall_entry":"0x62000000","dispatch":")") +
                  Dispatch + "\""),
          LLVM);
      EXPECT_NE(Code.find("__builtin_ia32_hyperv_call"), std::string::npos);
      auto Parsed = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      EXPECT_NE(Report.find("hypercall_successors"), std::string::npos);
      execute(std::string(Provider) + Code, Harness);
      if (std::string(Dispatch) == "switch")
        if (const auto *Directory =
                std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
          const std::string Base =
              std::string(Directory) + (LLVM ? "/hyperv-llvm" : "/hyperv-high");
          std::ofstream(Base + ".c") << Code;
          std::ofstream(Base + ".json") << Report;
        }
    }
}

TEST_F(X64ContinuationExportTest, HyperVBindingRejectsNullAndCompetingAbis) {
  load({0x0f, 0x01, 0xc1});
  for (
      const char *Binding :
      {R"("native_hyperv_hypercall_entry":0)",
       R"("native_hyperv_hypercall_entry":true)",
       R"("native_hyperv_hypercall_entry":4096,"native_kvm_hypercalls":true)"}) {
    const auto Request =
        request(std::string("\"instructions\":[4096],") + Binding);
    for (bool LLVM : {false, true}) {
      const char *Report = nullptr;
      EXPECT_TRUE(
          take(neverd_x64_continuation_source_v1(Session, Request.data(),
                                                 Request.size(), LLVM, &Report))
              .empty());
      EXPECT_TRUE(take(Report).empty());
      EXPECT_FALSE(take(neverd_last_error(Session)).empty());
    }
  }
}
