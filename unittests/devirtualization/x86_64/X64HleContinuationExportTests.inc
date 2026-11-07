//===- X64HleContinuationExportTests.inc - HLE source state --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included inside the public continuation fixture. Providers qualify state
// transport; the separate raw-CPU matrix qualifies native instruction effects.

TEST_F(X64ContinuationExportTest, HleMemoryFormsRetainCompleteStateAndBudget) {
  const auto Cases = neverd::test::x86HleTestCases();
  std::vector<uint8_t> Bytes;
  std::string Table = "static const unsigned cases[][5]={\n";
  for (const auto &C : Cases) {
    Table += "{" + std::to_string(Bytes.size()) + "," +
             std::to_string(C.Bytes.size()) + "," +
             std::to_string(C.Operation) + "," + std::to_string(C.Width) + "," +
             std::to_string(C.Acquire) + "},\n";
    Bytes.insert(Bytes.end(), C.Bytes.begin(), C.Bytes.end());
  }
  Bytes.push_back(0xc3);
  Table += "};\n";
  load(Bytes);
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static void hle(void *memory,void *gprs,void *flags,unsigned control,unsigned width) {
  uint64_t g[4],f;memcpy(g,gprs,32);memcpy(&f,flags,8);
  unsigned op=control&255;
  if(!(control&512)||control&~1023u||op>18)abort();
  uint64_t operand=g[1];
  if(op>=3&&op<=5)operand&=width*8-1;
  else if(op!=7&&(op<8||op>11)&&width<8)operand&=(1ull<<(width*8))-1;
  uint64_t value=g[0]^(operand*17)^(g[2]+71)^(g[3]>>3)^control^width;
  unsigned mask=op==7?0x40:op==8||op==9?0x8d4:op==11||op==17||op==18?0:
                op>=3&&op<=5?0x895:0x8d5;
  if(op==7){g[0]=(uint32_t)value;g[2]=(uint32_t)(value>>32);}
  else if(op==6||op==16||op==17) {
    unsigned r=op==6?0:1;
    if(width==8)g[r]=value;
    else if(width==4)g[r]=(uint32_t)value;
    else {uint64_t m=(1ull<<(width*8))-1;g[r]=(g[r]&~m)|(value&m);}
  }
  f=(f&~(uint64_t)mask)|(value&mask);
  ((unsigned char *)memory)[0]^=(unsigned char)value;
  memcpy(gprs,g,32);memcpy(flags,&f,8);++calls;
}
#define __builtin_ia32_hle_context hle
)C";
  const std::string Harness = Table + R"C(
int main(void) {
  for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
    for(unsigned seed=0;seed<16;++seed) {
      unsigned char a[64],b[64];memset(a,seed,64);memcpy(b,a,64);
      neverd_x64_state_v6 s={0};
      for(unsigned r=0;r<16;++r)s.GPR[r]=0xfedcba9876543210ull+r*971+seed;
      for(unsigned j=0;j<sizeof(s.ZMM);++j)((unsigned char *)s.ZMM)[j]=seed+j*7;
      unsigned op=cases[i][2],width=cases[i][3],control=512+op+256*cases[i][4];
      if(op>=3&&op<=5)s.GPR[1]=seed%(width*8);
      s.GPR[7]=(uintptr_t)(a+16);s.PC=4096+cases[i][0];s.RFlags=0x202|(seed*137&0x8d5);
      s.CR2=0x1234+seed;s.CR8=7;s.MXCSR=0x1f80;
      s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
      s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
      neverd_x64_state_v6 expected=s;
      hle(b+16,expected.GPR,&expected.RFlags,control,width);
      expected.PC+=cases[i][1];calls=0;uint64_t budget=1;
      if(neverd_x64_continue_v6(&s,&budget)!=9||budget||calls!=1||
         memcmp(&s,&expected,sizeof(s))||memcmp(a,b,64))abort();
      s.PC=4096+cases[i][0];expected=s;calls=0;budget=0;
      if(neverd_x64_continue_v6(&s,&budget)!=9||budget||calls||
         memcmp(&s,&expected,sizeof(s))||memcmp(a,b,64))abort();
    }
  return 0;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      auto [Code, Report] = source(
          request(std::string(R"("entries":[4096],"follow_internal_calls":true,
            "max_instructions":512,"native_hle_steps":true,"dispatch":")") +
                  Dispatch + "\""),
          LLVM);
      auto Parsed = llvm::json::parse(Report);
      if (!Parsed)
        FAIL() << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      EXPECT_NE(Report.find("hle_successors"), std::string::npos);
      EXPECT_NE(Code.find("__builtin_ia32_hle_context"), std::string::npos);
      execute(std::string(Provider) + Code, Harness);
      if (const char *Directory =
              std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
        const auto Base = std::string(Directory) + "/hle-" +
                          (LLVM ? "llvm-" : "high-") + Dispatch;
        std::ofstream(Base + ".c") << Code;
        std::ofstream(Base + ".json") << Report;
        std::ofstream(Base + "-cases.h") << Table;
      }
    }
}

TEST_F(X64ContinuationExportTest, HleRequiresAnExplicitBinding) {
  load({0xf2, 0xf0, 0x01, 0x0f});
  for (bool LLVM : {false, true}) {
    auto [Code, Report] = source(request(R"("instructions":[4096])"), LLVM);
    auto Parsed = llvm::json::parse(Report);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 1);
    EXPECT_EQ(Code.find("__builtin_ia32_hle_context"), std::string::npos);
    for (const char *Bad : {"1", "null", "\"true\""}) {
      const auto Request = request(
          std::string(R"("instructions":[4096],"native_hle_steps":)") + Bad);
      const char *Manifest = nullptr;
      EXPECT_TRUE(
          take(neverd_x64_continuation_source_v1(
                   Session, Request.data(), Request.size(), LLVM, &Manifest))
              .empty());
      EXPECT_TRUE(take(Manifest).empty());
    }
  }
}

TEST_F(X64ContinuationExportTest, HleOriginalRegistersAndAddressWrapping) {
  const std::vector<std::vector<uint8_t>> Encodings = {
      {0xf2, 0xf0, 0x0f, 0xc0, 0x27},             // xadd ah,[rdi]
      {0xf3, 0x47, 0x87, 0x4c, 0xac, 0xf0},       // xchg r9d,[r12+r13*4-16]
      {0xf2, 0xf0, 0x66, 0x45, 0x0f, 0xc1, 0x37}, // xadd r14w,[r15]
      {0xf3, 0x48, 0xc7, 0x05, 0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff},
      {0xf3, 0xf0, 0x65, 0x67, 0x4f, 0x0f, 0xab, 0x4c, 0xac, 0xf0},
      {0xf2, 0x40, 0x86, 0x3e}, // implicit lock, xchg dil,[rsi]
      {0xf3, 0x64, 0x67, 0x66, 0xc7, 0x06, 0xef, 0xbe},
      {0xf2, 0xf0, 0x66, 0x0f, 0xba, 0x2f, 0xff},  // bts word,[rdi],255
      {0x66, 0xf3, 0xf0, 0x0f, 0xba, 0x37, 0xf0},  // btr word,[rdi],240
      {0xf3, 0xf0, 0x66, 0x0f, 0xba, 0x3f, 0x80}}; // btc word,[rdi],128
  std::vector<uint8_t> Bytes;
  std::string Table = "static const unsigned offsets[]={";
  for (const auto &Encoding : Encodings) {
    Table += std::to_string(Bytes.size()) + ",";
    Bytes.insert(Bytes.end(), Encoding.begin(), Encoding.end());
  }
  Table += std::to_string(Bytes.size()) + "};\n";
  Bytes.push_back(0xc3);
  load(Bytes);
  const char Provider[] = R"C(
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint64_t address,source_value;
static unsigned native_control,operand_bytes,calls;
static void probe(void *m,void *g,void *f,unsigned c,unsigned w){
  uint64_t bank[4];memcpy(bank,g,32);
  if((uintptr_t)m!=address||bank[1]!=source_value||c!=native_control||w!=operand_bytes)abort();
  uint64_t result=0x12345678fedcba98ull;
  if((c&255)==16||(c&255)==17){
    if(w==4)result=(uint32_t)result;
    memcpy((char*)g+8,&result,w==4?8:w);
  }
  ++calls;
}
#define __builtin_ia32_hle_context probe
)C";
  const std::string Harness = Table + R"C(
int main(void){
  const unsigned ops[]={16,17,16,18,5,17,18,5,4,3}, widths[]={1,4,2,8,8,1,2,2,2,2};
  for(unsigned i=0;i<sizeof(ops)/sizeof(ops[0]);++i)for(unsigned seed=0;seed<64;++seed){
    neverd_x64_state_v6 s={0};
    for(unsigned r=0;r<16;++r)s.GPR[r]=0xdeadbeef12345678ull+r*71+seed;
    s.GPR[9]=(seed&1)?(uint64_t)(int64_t)-129:129;
    s.GPR[12]=0xffffffc0;s.GPR[13]=32;
    s.PC=4096+offsets[i];s.RFlags=0x246;s.MXCSR=0x1f80;
    s.FSBase=0x10000;s.GSBase=0x20000;
    s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
    s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
    neverd_x64_state_v6 expected=s;
    operand_bytes=widths[i];native_control=512+ops[i]+((i==0||i==2||i==5||i==7)?256:0);
    unsigned destination=0;
    switch(i){
    case 0:address=s.GPR[7];source_value=(s.GPR[0]>>8)&255;destination=1;break;
    case 1:address=s.GPR[12]+s.GPR[13]*4-16;source_value=(uint32_t)s.GPR[9];destination=72;break;
    case 2:address=s.GPR[15];source_value=s.GPR[14]&65535;destination=112;break;
    case 3:address=4096+offsets[i+1]+0x12345678;source_value=UINT64_MAX;break;
    case 4:address=(uint32_t)(s.GPR[12]+s.GPR[13]*4-16+(((int64_t)s.GPR[9]>>6)*8))+s.GSBase;
           source_value=s.GPR[9]&63;break;
    case 5:address=s.GPR[6];source_value=s.GPR[7]&255;destination=56;break;
    case 6:address=(uint32_t)s.GPR[6]+s.FSBase;source_value=0xbeef;break;
    case 7:address=s.GPR[7];source_value=15;break;
    case 8:case 9:address=s.GPR[7];source_value=0;break;
    }
    if(ops[i]==16||ops[i]==17){uint64_t result=0x12345678fedcba98ull;
      if(widths[i]==4)result=(uint32_t)result;
      memcpy((char*)expected.GPR+destination,&result,widths[i]==4?8:widths[i]);}
    expected.PC=4096+offsets[i+1];calls=0;uint64_t budget=1;
    if(neverd_x64_continue_v6(&s,&budget)!=9||budget||calls!=1||memcmp(&s,&expected,sizeof(s)))abort();
  }
  return 0;
}
)C";
  for (bool LLVM : {false, true})
    for (const char *Dispatch : {"switch", "direct"}) {
      auto [Code, Report] = source(
          request(std::string(R"("entries":[4096],"follow_internal_calls":true,
            "native_hle_steps":true,"dispatch":")") +
                  Dispatch + "\""),
          LLVM);
      auto Parsed = llvm::json::parse(Report);
      if (!Parsed)
        FAIL() << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      execute(std::string(Provider) + Code, Harness);
    }
}
