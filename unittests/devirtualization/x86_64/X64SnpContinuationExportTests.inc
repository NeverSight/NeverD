//===- X64SnpContinuationExportTests.inc - RMP/SEV state transport --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.
// Providers check the native transport contract, not an emulated RMP algorithm.

TEST_F(X64ContinuationExportTest, SnpAndSevRetainCompleteStateAndMemory) {
  load({0xf3, 0x0f, 0x01, 0xff, 0xf2, 0x0f, 0x01, 0xff, 0xf3, 0x0f, 0x01, 0xfe,
        0xf2, 0x0f, 0x01, 0xfe, 0xf3, 0x0f, 0x01, 0xfd, 0xf2, 0x0f, 0x01, 0xfd,
        0xf2, 0x0f, 0x01, 0xfc, 0xf3, 0x0f, 0x01, 0xfc, 0x67, 0xf2, 0x0f, 0x01,
        0xff, 0xf2, 0x0f, 0x01, 0xd9, 0xf3, 0x0f, 0x01, 0xd9, 0xc3});
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static uint64_t ram;
static void snp(unsigned op,void *gpr,void *flags,unsigned control) {
  uint64_t g[3],f;memcpy(g,gpr,24);memcpy(&f,flags,8);
  uint64_t a=g[0],b=g[1],c=g[2];
  if(op==1){if(control&1)a=(uint32_t)a;b=(uint32_t)b;c=(uint32_t)c;}
  else if(control)abort();
  uint64_t h=a;
  if(op!=0 && op!=4)h^=b;
  if(op==1 || op==2 || op==4)h^=c;
  h^=0x13579bdf2468ace0ull+op;ram=h;++calls;
  if(op==4){g[0]=(uint32_t)h;g[1]=h^0x1122334455667788ull;g[2]=h+71;}
  else if(op==7){g[0]=a+4096;g[1]=b-1;}
  else if(op!=6)g[0]=(uint32_t)h;
  unsigned mask=op==6 ? 1 : (op==1 || op==7 ? 0x8d5 : 0x8d4);
  f=(f&~(uint64_t)mask)|(h&mask);
  memcpy(gpr,g,24);memcpy(flags,&f,8);
}
#define __builtin_ia32_psmash_context(g,f,c) snp(0,g,f,c)
#define __builtin_ia32_pvalidate_context(g,f,c) snp(1,g,f,c)
#define __builtin_ia32_rmpadjust_context(g,f,c) snp(2,g,f,c)
#define __builtin_ia32_rmpupdate_context(g,f,c) snp(3,g,f,c)
#define __builtin_ia32_rmpquery_context(g,f,c) snp(4,g,f,c)
#define __builtin_ia32_rmpread_context(g,f,c) snp(5,g,f,c)
#define __builtin_ia32_rmpopt_context(g,f,c) snp(6,g,f,c)
#define __builtin_ia32_rmpchkd_context(g,f,c) snp(7,g,f,c)
static void sev(uint64_t parameter){ram=parameter^0x3c3c3c3c3c3c3c3cull;++calls;}
#define __builtin_ia32_vmgexit_sev sev
)C";
  const char Harness[] = R"C(
int main(void) {
  const unsigned offsets[]={0,4,8,12,16,20,24,28,32,37,41};
  const unsigned masks[]={0x8d4,0x8d5,0x8d4,0x8d4,0x8d4,0x8d4,1,0x8d5};
  for(unsigned i=0;i<11;++i)for(unsigned seed=0;seed<128;++seed) {
    neverd_x64_state_v6 s={0};
    for(unsigned r=0;r<16;++r)s.GPR[r]=0xfedcba9876543210ull+seed*971+r*19;
    for(unsigned j=0;j<sizeof(s.ZMM);++j)((uint8_t*)s.ZMM)[j]=seed+j*17;
    s.RFlags=0x202|(seed*137&0x8d5);s.PC=0x1000+offsets[i];s.MXCSR=0x1f80;
    s.CR2=0x76543210+seed;s.CR8=7;
    s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
    s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
    neverd_x64_state_v6 expected=s;
    uint64_t memory;
    if(i<9) {
      unsigned op=i==8?1:i;
      uint64_t a=s.GPR[0],b=s.GPR[1],c=s.GPR[2];
      if(op==1){b&=UINT32_MAX;c&=UINT32_MAX;if(i==8)a&=UINT32_MAX;}
      uint64_t inputs[8]={a,a^b^c,a^b^c,a^b,a^c,a^b,a^b,a^b};
      memory=inputs[op]^(0x13579bdf2468ace0ull+op);
      switch(op) {
      case 4:expected.GPR[1]=memory^0x1122334455667788ull;expected.GPR[2]=memory+71;
      case 0:case 1:case 2:case 3:case 5:expected.GPR[0]=(uint32_t)memory;break;
      case 7:expected.GPR[0]+=4096;--expected.GPR[1];break;
      }
      expected.RFlags=(s.RFlags&~(uint64_t)masks[op])|(memory&masks[op]);
    } else memory=s.GPR[0]^0x3c3c3c3c3c3c3c3cull;
    expected.PC+=i==8?5:4;uint64_t budget=1;calls=0;ram=0;
    if(neverd_x64_continue_v6(&s,&budget)!=9 || budget || calls!=1 ||
       memcmp(&s,&expected,sizeof(s)) || ram!=memory)abort();
    s.PC=0x1000+offsets[i];expected=s;calls=0;ram=17;budget=0;
    if(neverd_x64_continue_v6(&s,&budget)!=9 || budget || calls || ram!=17 ||
       memcmp(&s,&expected,sizeof(s)))abort();
  }
  return 0;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      auto [Code, Report] = source(
          request(std::string(R"("entries":[4096],"follow_internal_calls":true,
            "native_snp_instructions":true,"native_sev_guest_exit":true,
            "dispatch":")") +
                  Dispatch + "\""),
          LLVM);
      auto Parsed = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      EXPECT_NE(Report.find("snp_sev_successors"), std::string::npos);
      execute(std::string(Provider) + Code, Harness);
      if (const char *Directory =
              std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
        const std::string Base = std::string(Directory) + "/snp-" +
                                 (LLVM ? "llvm-" : "high-") + Dispatch;
        std::ofstream(Base + ".c") << Code;
        std::ofstream(Base + ".json") << Report;
      }
    }
}

TEST_F(X64ContinuationExportTest, SnpAndSevBindingsRemainIndependent) {
  load({0xf3, 0x0f, 0x01, 0xff, 0xf3, 0x0f, 0x01, 0xd9});
  for (bool LLVM : {false, true})
    for (const char *Binding : {"", ",\"native_snp_instructions\":true",
                                ",\"native_sev_guest_exit\":true"}) {
      auto [Code, Report] = source(
          request(std::string("\"instructions\":[4096,4100]") + Binding), LLVM);
      auto Parsed = llvm::json::parse(Report);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"),
                *Binding ? 1 : 2);
      EXPECT_EQ(Code.find("__builtin_ia32_psmash_context") != std::string::npos,
                std::string(Binding).find("native_snp_instructions") !=
                    std::string::npos);
      EXPECT_EQ(Code.find("__builtin_ia32_vmgexit_sev") != std::string::npos,
                std::string(Binding).find("native_sev_guest_exit") !=
                    std::string::npos);
    }
  for (const char *Binding : {"\"native_snp_instructions\":1",
                              "\"native_sev_guest_exit\":\"true\""}) {
    const auto Request =
        request(std::string("\"instructions\":[4096],") + Binding);
    const char *Report = nullptr;
    EXPECT_TRUE(
        take(neverd_x64_continuation_source_v1(Session, Request.data(),
                                               Request.size(), false, &Report))
            .empty());
    EXPECT_TRUE(take(Report).empty());
  }
}

TEST_F(X64ContinuationExportTest, SnpUnauditedPrefixesFailClearly) {
  for (const auto &Bytes :
       {std::vector<uint8_t>{0x65, 0xf2, 0x0f, 0x01, 0xfd},
        std::vector<uint8_t>{0x67, 0xf3, 0x0f, 0x01, 0xff}}) {
    load(Bytes);
    for (bool LLVM : {false, true}) {
      const auto Request = request(R"("instructions":[4096],
        "native_snp_instructions":true,"native_sev_guest_exit":true)");
      const char *Report = nullptr;
      EXPECT_TRUE(
          take(neverd_x64_continuation_source_v1(Session, Request.data(),
                                                 Request.size(), LLVM, &Report))
              .empty());
      EXPECT_TRUE(take(Report).empty());
      EXPECT_NE(take(neverd_last_error(Session)).find("unlifted instruction"),
                std::string::npos);
    }
  }
}
