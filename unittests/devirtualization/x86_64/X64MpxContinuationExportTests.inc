//===- X64MpxContinuationExportTests.inc - Bounds source completion -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Included by X64ContinuationExportTests.cpp inside its fixture namespace.

TEST_F(X64ContinuationExportTest, MpxOwnsAllBoundsAndMemoryForms) {
  std::vector<uint8_t> Bytes;
  std::string Request =
      R"({"profile":"intel-mpx-v11","mxcsr_mask":"0xffff","instructions":[)";
  std::string Cases = "static const struct {unsigned op,bank,prefix;uint64_t "
                      "pc,size;} cases[]={\n";
  for (unsigned Prefix = 0; Prefix != 3; ++Prefix)
    for (unsigned Bank = 0; Bank != 4; ++Bank)
      for (unsigned Op = 0; Op != 13; ++Op) {
        const auto Start = Bytes.size();
        if (Start)
          Request += ",";
        Request += std::to_string(0x1000 + Start);
        if (Prefix == 2)
          Bytes.push_back(0x64); // FS affects BNDMOV/table pointers only.
        if (Prefix)
          Bytes.push_back(0x67); // MPX still uses the full 64-bit address.
        if (Op == 4)
          Bytes.push_back(0xf0); // LOCK BNDMOV store.
        if (Op <= 8)
          Bytes.push_back(Op == 0 || Op == 5 || Op == 8 ? 0xf3
                          : Op == 6 || Op == 7          ? 0xf2
                                                        : 0x66);
        Bytes.push_back(0x0f);
        Bytes.push_back(
            Op == 0 || Op == 3 || Op == 4 || Op == 7 || Op == 10 ? 0x1b : 0x1a);
        if (Op == 1 || (Op >= 5 && Op <= 7)) {
          Bytes.push_back(0xc0 | Bank << 3 | (Op == 1 ? (Bank + 1) % 4 : 1));
        } else if (Op == 2 || Op == 3 || Op == 4) {
          Bytes.push_back(0x40 | Bank << 3);
          Bytes.push_back(16);
        } else if (Op == 11) {
          Bytes.push_back(Bank << 3 | 4);
          Bytes.insert(Bytes.end(), {0xcd, 0x78, 0x56, 0x34, 0x12});
        } else if (Op == 12) {
          Bytes.push_back(0x40 | Bank << 3 | 4);
          Bytes.insert(Bytes.end(), {0x20, 16}); // No MIB index.
        } else {
          Bytes.push_back(0x40 | Bank << 3 | 4);
          Bytes.insert(Bytes.end(), {0x88, 16}); // RAX+RCX*4+16.
        }
        Cases += "{" + std::to_string(Op) + "," + std::to_string(Bank) + "," +
                 std::to_string(Prefix) + "," + std::to_string(0x1000 + Start) +
                 "," + std::to_string(Bytes.size() - Start) + "},\n";
        // Keep each instruction independent of the next selected start.
        Bytes.resize(Start + 16, 0x90);
      }
  Request += "]}";
  Cases += "};\n";
  load(Bytes);
  const char Harness[] = R"C(
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"MPX line %u case %u seed %u enable %u\n",__LINE__,ci,seed,en); abort(); } } while(0)
static uint64_t directory[512] __attribute__((aligned(4096)));
static uint64_t table[8192] __attribute__((aligned(64)));
static uint8_t ram[64], expected_ram[64];
int main(void) {
  for(unsigned ci=0;ci<sizeof(cases)/sizeof(*cases);++ci)
  for(unsigned seed=0;seed<24;++seed)for(unsigned en=0;en<16;++en) {
    const unsigned op=cases[ci].op,b=cases[ci].bank,p=cases[ci].prefix;
    neverd_x64_state_v11 s={0};
    for(unsigned i=0;i<16;++i)s.GPR[i]=0x1234000000000000ULL+i*91;
    for(unsigned i=0;i<sizeof(s.ZMM);++i)((uint8_t*)s.ZMM)[i]=(uint8_t)(i+seed);
    for(unsigned i=0;i<4;++i) {
      s.Bounds[i][0]=seed%3==0?0:seed%3==1?UINT64_MAX-i:0x8000000012340000ULL+i;
      s.Bounds[i][1]=seed%3==0?0:seed%3==1?UINT64_MAX-i:0x7fffffffedcbffffULL-i;
    }
    s.PC=cases[ci].pc;s.RFlags=2;s.MXCSR=0x1f80;s.CR2=0x87654321;
    s.BNDSTATUS=0xabcde002;s.BNDCFGS=(uint64_t)directory|(en&1);
    s.MPXSupported=!!(en&2);s.OSXSAVE=!!(en&4);s.XCR0=(en&8)?31:(seed&1?15:23);
    const unsigned enabled=en==15;
    if(p==2)s.FSBase=op==11?0:0x300000000ULL;
    s.GPR[0]=seed%3==0?0:seed%3==1?UINT64_MAX-16:0x8000000000000010ULL;
    s.GPR[1]=seed%4==0?0:seed%4==1?UINT64_MAX:seed%4==2?0x8000000000000000ULL:seed;
    for(unsigned i=0;i<sizeof(ram);++i)ram[i]=(uint8_t)(i*13+seed);
    for(unsigned i=0;i<8192;++i)table[i]=0x1122334400000000ULL+i+seed;
    if(op>=2&&op<=4)s.GPR[0]=enabled?(uint64_t)(ram+3)-16-s.FSBase:0;
    const uint64_t pointer=op==11?0:0x2300+seed*8;
    if(op>=9) {
      s.GPR[0]=pointer-16-s.FSBase;
      directory[0]=seed%3?(uint64_t)table|1:0;
      const unsigned at=(unsigned)((pointer>>3)&0x1ffff)*4;
      table[at+2]=seed&1?(op==12?0:s.GPR[1]):~s.GPR[1];
    }
    neverd_x64_state_v11 expected=s;
    memcpy(expected_ram,ram,sizeof(ram));
    uint64_t expected_table[8192];memcpy(expected_table,table,sizeof(table));
    unsigned fault=0;
    if(enabled) {
      if(op==0) { expected.Bounds[b][0]=s.GPR[0];expected.Bounds[b][1]=~(s.GPR[0]+s.GPR[1]*4+16); }
      if(op==1)memcpy(expected.Bounds[b],s.Bounds[(b+1)%4],16);
      if(op==2)memcpy(expected.Bounds[b],ram+3,16);
      if(op==3||op==4)memcpy(expected_ram+3,s.Bounds[b],16);
      if(op==5||op==8)fault=(op==5?s.GPR[1]:s.GPR[0]+s.GPR[1]*4+16)<s.Bounds[b][0];
      if(op==6)fault=s.GPR[1]>~s.Bounds[b][1];
      if(op==7)fault=s.GPR[1]>s.Bounds[b][1];
      if(fault)expected.BNDSTATUS=1;
      if(op>=9) {
        if(!(directory[0]&1)) {fault=1;expected.BNDSTATUS=(uint64_t)directory|2;}
        else {
          unsigned at=(unsigned)(pointer/8)*4;
          if(op==10) { expected_table[at]=s.Bounds[b][0];expected_table[at+1]=s.Bounds[b][1];expected_table[at+2]=s.GPR[1]; }
          else {
            unsigned match=table[at+2]==(op==12?0:s.GPR[1]);
            expected.Bounds[b][0]=match?table[at]:0;expected.Bounds[b][1]=match?table[at+1]:0;
          }
        }
      }
    }
    if(!fault)expected.PC+=cases[ci].size;
    uint64_t budget=1;
    CHECK(neverd_x64_continue_v11(&s,&budget)==(fault?14:9));
    CHECK(budget==0&&memcmp(&s,&expected,sizeof(s))==0);
    CHECK(memcmp(ram,expected_ram,sizeof(ram))==0);
    CHECK(memcmp(table,expected_table,sizeof(table))==0);
    CHECK(directory[0]==(op>=9?(seed%3?((uint64_t)table|1):0):directory[0]));
  }
  return 0;
}
)C";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Code, Manifest] = source(Request, LLVM, 11);
    ASSERT_FALSE(Code.empty());
    EXPECT_NE(Manifest.find("\"native_requests\": 0"), std::string::npos);
    execute(Code, Cases + Harness);
    auto DirectRequest = Request;
    DirectRequest.insert(1, "\"dispatch\":\"direct\",");
    auto [Direct, DirectManifest] = source(DirectRequest, LLVM, 11);
    execute(Direct, Cases + Harness);
    if (const char *Directory =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Directory);
      const auto Prefix =
          fs::path(Directory) / (LLVM ? "mpx-llvm" : "mpx-high");
      std::ofstream(Prefix.string() + ".c") << Code;
      std::ofstream(Prefix.string() + "-harness.c") << Cases << Harness;
      std::ofstream(Prefix.string() + ".json") << Manifest;
    }
  }
}

TEST_F(X64ContinuationExportTest, MpxRejectsInvalidContextBeforeEffects) {
  load({0xf3, 0x0f, 0x1b, 0x40, 0x10, 0xc3});
  const char Request[] =
      R"({"profile":"intel-mpx-v11","mxcsr_mask":"0xffff","instructions":[4096]})";
  const char Harness[] = R"C(
#include <stdlib.h>
int main(void) {
  for(unsigned i=0;i<5;++i) {
    neverd_x64_state_v11 s={0};s.PC=4096;s.RFlags=2;s.MXCSR=0x1f80;
    if(i==0)s.MPXSupported=2;if(i==1)s.OSXSAVE=2;
    if(i==2)s.InTransaction=1;if(i==3)s.BNDCFGS=4;if(i==4)s.RFlags|=0x100;
    neverd_x64_state_v11 before=s;uint64_t budget=3;
    if(neverd_x64_continue_v11(&s,&budget)!=5||budget!=3||memcmp(&s,&before,sizeof(s)))abort();
  }
  return 0;
}

)C";
  for (bool LLVM : {false, true}) {
    auto [Code, Manifest] = source(Request, LLVM, 11);
    execute(Code, Harness);
  }
}

TEST_F(X64ContinuationExportTest, MpxOriginalBranchesOwnInitialization) {
  const std::vector<std::vector<uint8_t>> Forms = {
      {0xe8, 0x00, 0x10, 0x00, 0x00},
      {0xe9, 0x00, 0x10, 0x00, 0x00},
      {0xeb, 0x03},
      {0x74, 0x03},
      {0x0f, 0x84, 0x03, 0, 0, 0},
      {0xc3},
      {0xc2, 0x08, 0},
      {0xff, 0xd0},
      {0xff, 0xe0}};
  std::vector<uint8_t> Bytes;
  std::string Request =
      R"({"profile":"intel-mpx-v11","mxcsr_mask":"0xffff","instructions":[)";
  std::string Cases =
      "static const struct {unsigned op,bnd;uint64_t pc,size;} cases[]={\n";
  for (unsigned Bnd = 0; Bnd < 2; ++Bnd)
    for (unsigned Op = 0; Op < Forms.size(); ++Op) {
      auto Start = Bytes.size();
      if (Start)
        Request += ",";
      Request += std::to_string(4096 + Start);
      if (Bnd)
        Bytes.push_back(0xf2);
      Bytes.insert(Bytes.end(), Forms[Op].begin(), Forms[Op].end());
      Cases += "{" + std::to_string(Op) + "," + std::to_string(Bnd) + "," +
               std::to_string(4096 + Start) + "," +
               std::to_string(Bytes.size() - Start) + "},\n";
      Bytes.resize(Start + 16, 0x90);
    }
  Request += "]}";
  Cases += "};\n";
  load(Bytes);
  const char Harness[] = R"C(
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"branch %u/%u line %u\n",ci,mode,__LINE__);abort();}}while(0)
int main(void) {
  for(unsigned ci=0;ci<sizeof(cases)/sizeof(*cases);++ci)for(unsigned mode=0;mode<8;++mode) {
    uint64_t stack[4]={0x8877,0x9000,0x1234,0x5678};
    neverd_x64_state_v11 s={0};
    s.PC=cases[ci].pc;s.RFlags=2|((mode&1)?0x40:0);s.MXCSR=0x1f80;
    s.MPXSupported=1;s.OSXSAVE=1;s.XCR0=31;s.BNDCFGS=(mode&2)?3:1;
    if(mode&4)s.BNDCFGS&=~1ULL;
    s.GPR[0]=0x9000;s.GPR[4]=(uint64_t)(stack+1);s.BNDSTATUS=0x1234;
    for(unsigned i=0;i<4;++i){s.Bounds[i][0]=0x1234567800000000ULL+i;s.Bounds[i][1]=~s.Bounds[i][0];}
    neverd_x64_state_v11 expected=s;
    unsigned op=cases[ci].op;
    if(!(mode&6)&&!cases[ci].bnd&&op!=2)memset(expected.Bounds,0,sizeof(expected.Bounds));
    uint64_t next=s.PC+cases[ci].size, status=2;
    if(op==0||op==1)expected.PC=next+0x1000;
    if(op==2)expected.PC=next+3;
    if(op==3||op==4)expected.PC=next+((mode&1)?3:0);
    if(op==5||op==6){expected.PC=stack[1];expected.GPR[4]+=op==5?8:16;status=3;}
    if(op==7||op==8)expected.PC=s.GPR[0];
    if(op==0||op==7){expected.GPR[4]-=8;status=1;}
    uint64_t budget=3;
    CHECK(neverd_x64_continue_v11(&s,&budget)==status);
    CHECK(budget==2&&memcmp(&s,&expected,sizeof(s))==0);
    CHECK(stack[0]==(op==0||op==7?next:0x8877));
    CHECK(stack[1]==0x9000&&stack[2]==0x1234&&stack[3]==0x5678);
  }
  return 0;
}
)C";
  for (bool LLVM : {false, true}) {
    auto [Code, Manifest] = source(Request, LLVM, 11);
    EXPECT_NE(Manifest.find("\"native_requests\": 0"), std::string::npos);
    execute(Code, Cases + Harness);
    auto Direct = Request;
    Direct.insert(1, "\"dispatch\":\"direct\",");
    auto [Linked, LinkedManifest] = source(Direct, LLVM, 11);
    execute(Linked, Cases + Harness);
  }
}

TEST_F(X64ContinuationExportTest, MpxEntrySelectionResumesAfterOriginalBytes) {
  load({0xf3, 0x0f, 0x1b, 0x40, 0x10, // bndmk bnd0,[rax+16]
        0xf3, 0x0f, 0x1a, 0xc1,       // bndcl bnd0,rcx
        0xc3});
  const char Request[] =
      R"({"profile":"intel-mpx-v11","mxcsr_mask":"0xffff","entries":[4096]})";
  const char Harness[] = R"C(
#include <stdlib.h>
int main(void) {
  uint64_t stack=0x9000,budget=3;
  neverd_x64_state_v11 s={0};s.PC=4096;s.RFlags=2;s.MXCSR=0x1f80;
  s.MPXSupported=1;s.OSXSAVE=1;s.XCR0=31;s.BNDCFGS=3;
  s.GPR[0]=0x100000002ULL;s.GPR[1]=0x100000003ULL;s.GPR[4]=(uint64_t)&stack;
  if(neverd_x64_continue_v11(&s,&budget)!=3||budget||s.PC!=0x9000||
     s.Bounds[0][0]!=s.GPR[0]||s.Bounds[0][1]!=~(s.GPR[0]+16))abort();
  s.PC=4096;s.GPR[1]=0;s.GPR[4]=(uint64_t)&stack;budget=3;
  if(neverd_x64_continue_v11(&s,&budget)!=14||budget!=1||s.PC!=4101||
     s.GPR[4]!=(uint64_t)&stack||s.BNDSTATUS!=1)abort();
  s.GPR[1]=s.GPR[0];budget=2;
  if(neverd_x64_continue_v11(&s,&budget)!=3||budget||s.PC!=0x9000)abort();
  return 0;
}
)C";
  for (bool LLVM : {false, true}) {
    auto [Code, Manifest] = source(Request, LLVM, 11);
    EXPECT_NE(Manifest.find("mpx_successors"), std::string::npos);
    EXPECT_NE(Manifest.find("\"total\": 3"), std::string::npos);
    execute(Code, Harness);
  }
}
