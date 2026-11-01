//===- X64ContinuationExportTests.cpp - State source public contract -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute exported C and verify the real SDK and CLI retain exact boundaries.
//===----------------------------------------------------------------------===//

#include "../../lift/NeverDLiftFixture.h"
#include "../../lift/x86/X86HleTestCases.h"
#include "../../lift/x86/X86TransactionTestCases.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string take(const char *Text) {
  const std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

std::string readOutput(const fs::path &Path) {
  std::ifstream File(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(File), {}};
}

class X64ContinuationExportTest : public NeverDLiftTest {
protected:
  neverd_session_t Session = nullptr;
  void SetUp() override {
    NeverDLiftTest::SetUp();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    NeverDLiftTest::TearDown();
  }
  void load(const std::vector<uint8_t> &Code,
            const std::vector<std::pair<uint64_t, uint64_t>> &Functions = {}) {
    // A real linked ELF mapping, not a raw-image format override. The shared
    // native byte provider deliberately retains its linked-image admission.
    std::vector<uint8_t> Bytes(0x1000 + Code.size());
    const auto Put = [&](size_t Offset, uint64_t Value, unsigned Count) {
      for (unsigned I = 0; I < Count; ++I)
        Bytes[Offset + I] = uint8_t(Value >> (I * 8));
    };
    const uint8_t Identity[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::copy(std::begin(Identity), std::end(Identity), Bytes.begin());
    Put(16, 2, 2);  // ET_EXEC.
    Put(18, 62, 2); // EM_X86_64.
    Put(20, 1, 4);
    Put(24, 0x1000, 8); // Entry.
    Put(32, 64, 8);     // Program header offset.
    Put(52, 64, 2);
    Put(54, 56, 2);
    Put(56, 1, 2);
    Put(64, 1, 4);      // PT_LOAD.
    Put(68, 5, 4);      // PF_R | PF_X.
    Put(72, 0x1000, 8); // File offset.
    Put(80, 0x1000, 8); // Virtual address.
    Put(88, 0x1000, 8);
    Put(96, Code.size(), 8);
    Put(104, Code.size(), 8);
    Put(112, 0x1000, 8);
    if (!Functions.empty()) {
      // Sized ELF function symbols give native discovery real entry extents;
      // session-only analyst edits are deliberately separate from this image.
      ASSERT_LE(0x400 + (Functions.size() + 1) * 24, 0x800u);
      Put(40, 0x200, 8);
      Put(58, 64, 2);
      Put(60, 4, 2);
      Put(0x240 + 4, 1, 4); // SHT_PROGBITS.
      Put(0x240 + 8, 6, 8); // SHF_ALLOC | SHF_EXECINSTR.
      Put(0x240 + 16, 0x1000, 8);
      Put(0x240 + 24, 0x1000, 8);
      Put(0x240 + 32, Code.size(), 8);
      Put(0x240 + 48, 16, 8);
      Put(0x280 + 4, 2, 4); // SHT_SYMTAB.
      Put(0x280 + 24, 0x400, 8);
      Put(0x280 + 32, (Functions.size() + 1) * 24, 8);
      Put(0x280 + 40, 3, 4); // String-table section.
      Put(0x280 + 44, 1, 4); // First global symbol.
      Put(0x280 + 48, 8, 8);
      Put(0x280 + 56, 24, 8);
      std::string Names(1, '\0');
      for (size_t I = 0; I < Functions.size(); ++I) {
        const auto [Entry, Size] = Functions[I];
        const size_t Symbol = 0x400 + (I + 1) * 24;
        Put(Symbol, Names.size(), 4);
        Put(Symbol + 4, 0x12, 1); // STB_GLOBAL | STT_FUNC.
        Put(Symbol + 6, 1, 2);
        Put(Symbol + 8, Entry, 8);
        Put(Symbol + 16, Size, 8);
        Names += "fixture_" + std::to_string(Entry);
        Names.push_back('\0');
      }
      ASSERT_LE(0x800 + Names.size(), 0x1000u);
      Put(0x2c0 + 4, 3, 4); // SHT_STRTAB.
      Put(0x2c0 + 24, 0x800, 8);
      Put(0x2c0 + 32, Names.size(), 8);
      Put(0x2c0 + 48, 1, 8);
      std::copy(Names.begin(), Names.end(), Bytes.begin() + 0x800);
    }
    std::copy(Code.begin(), Code.end(), Bytes.begin() + 0x1000);
    std::ofstream File(tmpFile("input.elf"), std::ios::binary);
    File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    File.close();
    ASSERT_TRUE(
        neverd_session_load(Session, tmpFile("input.elf").string().c_str()))
        << take(neverd_last_error(Session));
  }
  static std::string request(llvm::StringRef Selection) {
    return ("{\"profile\":\"kernel-events-v6\",\"mxcsr_mask\":\"0xffbf\","
            "\"paging_address_bits\":48," +
            Selection + "}")
        .str();
  }
  std::pair<std::string, std::string> source(llvm::StringRef Request, bool LLVM,
                                             unsigned Version = 6) {
    const char *Report = nullptr;
    auto Source = take(neverd_x64_continuation_source_v1(
        Session, Request.data(), Request.size(), LLVM, &Report));
    auto Manifest = take(Report);
    EXPECT_FALSE(Source.empty()) << take(neverd_last_error(Session));
    EXPECT_FALSE(Manifest.empty());
    auto Parsed = llvm::json::parse(Manifest);
    EXPECT_TRUE(bool(Parsed));
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
    } else if (const auto *Object = Parsed->getAsObject()) {
      EXPECT_EQ(Object->getInteger("schema_version"), 2);
      EXPECT_EQ(Object->getInteger("state_version"), Version);
      EXPECT_EQ(Object->getString("environment_driver"),
                "neverd_x64_run_v" + std::to_string(Version));
      EXPECT_EQ(Object->getInteger("state_bytes"), Version == 13   ? 2464
                                                   : Version == 12 ? 2368
                                                   : Version == 11 ? 2416
                                                   : Version == 10 ? 2464
                                                   : Version == 9  ? 2560
                                                   : Version == 8  ? 2544
                                                   : Version == 7  ? 2528
                                                                   : 2496);
      EXPECT_EQ(Object->getInteger("state_alignment"), 16);
      EXPECT_EQ(Object->getBoolean("ordinary_source_abi"), false);
      EXPECT_EQ(Object->getString("ordinary_source_validation"), "not-run");
      EXPECT_EQ(Object->getBoolean("whole_function_coverage"), false);
      EXPECT_EQ(
          Object->getString("source_sha256"),
          llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(Source)),
                      true));
    } else {
      ADD_FAILURE() << "manifest must be an object";
    }
    return {std::move(Source), std::move(Manifest)};
  }
  void execute(llvm::StringRef Source, llvm::StringRef Harness) {
    ASSERT_FALSE(Source.empty());
    const auto File = tmpFile("run.c");
    std::ofstream(File) << Source.str() << Harness.str();
    for (const char *Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Level);
      const auto Exe = tmpFile(std::string("run") + Level +
                               neverd::test::executableSuffix());
      auto Built = exec(NEVERD_TEST_CLANG,
                        {"-std=c11", Level, "-fsanitize=address,undefined",
                         "-Werror=uninitialized", "-Werror=return-type",
                         File.string(), "-o", Exe.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      auto Ran = exec(Exe.string(), {});
      ASSERT_TRUE(Ran.ok()) << Ran.err;
    }
  }
};

const char Seed[] = R"C(
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
#if defined(__linux__)
#include <sched.h>
static void header_contract(void) {
    cpu_set_t selected;CPU_ZERO(&selected);CPU_SET(3,&selected);
    CHECK(CPU_ISSET(3,&selected));
}
#else
static void header_contract(void) {}
#endif
static neverd_x64_state_v6 seed(void) {
    neverd_x64_state_v6 s={0};
    for(unsigned i=0;i<16;++i)s.GPR[i]=0x1234567800000000ULL+i*71;
    for(unsigned i=0;i<sizeof(s.ZMM);++i)((unsigned char *)s.ZMM)[i]=i*13+7;
    s.RFlags=0x246;s.PC=0x1000;s.CR8=3;s.MXCSR=0x1f80;
    s.CS=(neverd_x64_segment){0,0xffffffff,8,0xa09b};
    s.SS=(neverd_x64_segment){0,0xffffffff,16,0xc093};
    s.DS=s.ES=s.SS;s.FS=(neverd_x64_segment_metadata){0xffffffff,16,0xc093};s.GS=s.FS;
    s.LDTR=(neverd_x64_segment){0,63,40,0x82};s.TR=(neverd_x64_segment){0,103,48,0x8b};
    s.STAR=0x0023001000000000ULL;s.NMIBlocked=1;
    return s;
}
)C";

TEST_F(X64ContinuationExportTest, SoftwareInterruptStopsBeforeSelectedHandler) {
  load({0xcd, 0xff, 0xcd, 0x2d, 0xb8, 7, 0, 0, 0});
  const auto Request = request(
      R"("follow_internal_calls":true,"dispatch":"direct","instructions":[4096,4098,4100])");
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    uint64_t gdt[4]={0,0,0,0x00af9b000000ffffULL},idt[512]={0};
    unsigned char stack[256];
    const unsigned vectors[]={255,45};
    for(unsigned i=0;i<2;++i) {
        neverd_x64_state_v6 s=seed();s.PC=0x1000+2*i;
        const uint64_t old_sp=(uintptr_t)(stack+231),old_ax=s.GPR[0];
        s.GPR[4]=old_sp;s.GDTR.Base=(uintptr_t)gdt;s.GDTR.Limit=sizeof(gdt)-1;
        s.IDTR.Base=(uintptr_t)idt;s.IDTR.Limit=16*vectors[i]+15;
        idt[2*vectors[i]]=0x1004|(24ULL<<16)|(0x8eULL<<40);
        uint64_t budget=0;neverd_x64_state_v6 saved=s;
        CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !memcmp(&s,&saved,sizeof(s)));
        --s.IDTR.Limit;saved=s;budget=2;
        memset(stack,0xa5,sizeof(stack));
        CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==2);
        CHECK(!memcmp(&s,&saved,sizeof(s)));
        for(unsigned j=0;j<sizeof(stack);++j)CHECK(stack[j]==0xa5);
        ++s.IDTR.Limit;
        CHECK(neverd_x64_continue_v6(&s,&budget)==7 && budget==1);
        CHECK(s.PC==0x1004 && s.GPR[0]==old_ax && s.NMIBlocked==1);
        uint64_t frame[5];memcpy(frame,(void *)(uintptr_t)s.GPR[4],sizeof(frame));
        CHECK(frame[0]==0x1002+2*i && frame[1]==8 && frame[2]==0x246);
        CHECK(frame[3]==old_sp && frame[4]==16);
        budget=2;
        CHECK(neverd_x64_continue_v6(&s,&budget)==8 && budget==1);
        CHECK(s.PC==0x1009 && s.GPR[0]==7);
    }
    return 0;
}

)C");
  }
}

TEST_F(X64ContinuationExportTest,
       CompatibilityFastSystemRoundTripStopsAtEveryTransition) {
  load({0x0f, 0x34, 0x0f, 0x35, 0x90});
  const std::string Request =
      R"({"profile":"intel-fast-system-v7","mxcsr_mask":"0xffbf","paging_address_bits":48,"sysenter_msr_address_bits":48,"follow_internal_calls":true,"instructions":[4096,4098,4100]})";
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM, 7);
    execute(Source, R"C(
#include <stdlib.h>
#define CHECK(c) do {if(!(c))abort();}while(0)
int main(void) {
  for(unsigned mode=0;mode<2;++mode) {
    neverd_x64_state_v7 s={0},want;
    s.PC=0x1000;s.RFlags=0x3d7ed7;s.MXCSR=0x1f80;
    s.CS=(neverd_x64_segment){0,UINT32_MAX,27,0x80fb|(mode<<14)};
    s.SS=(neverd_x64_segment){0,UINT32_MAX,35,0xc0f3};
    s.SysenterCS=8;s.SysenterEIP=0x1002;s.SysenterESP=0x123456789abcULL;
    for(unsigned i=0;i<16;++i)s.GPR[i]=0x123456789abcdef0ULL+i;
    for(unsigned i=0;i<8;++i)s.Undefined[i]=0xdeadbeef00000000ULL+i;
    s.GPR[1]=0x7654321000009000ULL;s.GPR[2]=0x1234567800001000ULL;
    memset(s.ZMM,0xa5,sizeof(s.ZMM));
    uint64_t budget=3;
    want=s;want.PC=0x1002;want.GPR[4]=s.SysenterESP;want.RFlags&=~0x30200ULL;
    want.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
    want.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
    CHECK(neverd_x64_continue_v7(&s,&budget)==10&&budget==2&&!memcmp(&s,&want,sizeof(s)));
    want=s;want.PC=0x1000;want.GPR[4]=0x9000;
    for(unsigned i=0;i<8;++i)want.GPR[i]=(uint32_t)want.GPR[i]|(s.Undefined[i]<<32);
    want.CS=(neverd_x64_segment){0,UINT32_MAX,27,0xc0fb};
    want.SS=(neverd_x64_segment){0,UINT32_MAX,35,0xc0f3};
    CHECK(neverd_x64_continue_v7(&s,&budget)==6&&budget==1&&!memcmp(&s,&want,sizeof(s)));
    want=s;want.PC=0x1002;want.GPR[4]=s.SysenterESP;
    want.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
    want.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
    CHECK(neverd_x64_continue_v7(&s,&budget)==10&&budget==0&&!memcmp(&s,&want,sizeof(s)));
    CHECK(neverd_x64_continue_v7(&s,&budget)==9&&!memcmp(&s,&want,sizeof(s)));
  }
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       FastSystemV7RetainsLiveMsrsAndStopsAtBothBoundaries) {
  load(
      {0x0f, 0x30, 0x0f, 0x32, 0x0f, 0x34, 0x0f, 0x35, 0x48, 0x0f, 0x35, 0x90});
  const std::string Request =
      R"({"profile":"intel-fast-system-v7","mxcsr_mask":"0xffbf","paging_address_bits":48,"sysenter_msr_address_bits":48,"follow_internal_calls":true,"instructions":[4096,4098,4100,4102,4104,4107]})";
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM, 7);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    const auto *Object = Parsed->getAsObject();
    EXPECT_EQ(Object->getString("profile"), "intel-fast-system-v7");
    EXPECT_EQ(Object->getInteger("native_requests"), 0);
    const auto *Fields = Object->getArray("state_fields");
    ASSERT_NE(Fields, nullptr);
    for (const auto &[Name, Offset] :
         std::vector<std::pair<std::string, unsigned>>{{"SysenterCS", 2496},
                                                       {"SysenterESP", 2504},
                                                       {"SysenterEIP", 2512},
                                                       {"ReservedV7", 2520}}) {
      const auto It =
          std::find_if(Fields->begin(), Fields->end(), [&](const auto &F) {
            return F.getAsObject()->getString("name") == Name;
          });
      ASSERT_NE(It, Fields->end());
      EXPECT_EQ(It->getAsObject()->getInteger("offset"), Offset);
      EXPECT_EQ(It->getAsObject()->getInteger("bytes"), 8);
    }
    execute(Source, R"C(
#include <stdlib.h>
#define CHECK(c) do {if(!(c))abort();}while(0)
int main(void) {
  neverd_x64_state_v7 s={0},want;
  s.PC=0x1000;s.RFlags=0x246;s.MXCSR=0x1f80;s.CR8=3;
  s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
  memset(s.ZMM,0xa5,sizeof(s.ZMM));
  s.SysenterEIP=0x1006;s.SysenterESP=0x123456789abcULL;
  s.ReservedV7=0xfedcba9876543210ULL;
  s.GPR[1]=0x174;s.GPR[0]=0xa5a5fffb;s.GPR[2]=0xffffffff;
  want=s;want.SysenterCS=0xa5a5fffb;want.PC=0x1002;
  uint64_t budget=1;
  CHECK(neverd_x64_continue_v7(&s,&budget)==9&&budget==0&&!memcmp(&s,&want,sizeof(s)));
  want=s;want.GPR[2]=0;want.PC=0x1004;budget=1;
  CHECK(neverd_x64_continue_v7(&s,&budget)==9&&budget==0&&!memcmp(&s,&want,sizeof(s)));
  s.CS=(neverd_x64_segment){0,UINT32_MAX,11,0xa09f};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,19,0xc0f3};
  want=s;want.PC=0x1006;want.GPR[4]=s.SysenterESP;want.RFlags&=~0x200ULL;
  want.CS=(neverd_x64_segment){0,UINT32_MAX,0xfff8,0xa09b};
  want.SS=(neverd_x64_segment){0,UINT32_MAX,0,0xc093};budget=4;
  CHECK(neverd_x64_continue_v7(&s,&budget)==10&&budget==3&&!memcmp(&s,&want,sizeof(s)));
  s.GPR[1]=0xabcdef1200009000ULL;s.GPR[2]=0x123456780000100bULL;
  for(unsigned i=0;i<8;++i)s.Undefined[i]=0x76543210+i;
  want=s;want.PC=0x100b;want.GPR[4]=0x9000;
  for(unsigned i=0;i<8;++i)want.GPR[i]=(uint32_t)want.GPR[i]|(s.Undefined[i]<<32);
  want.CS=(neverd_x64_segment){0,UINT32_MAX,0xb,0xc0fb};
  want.SS=(neverd_x64_segment){0,UINT32_MAX,0x13,0xc0f3};
  CHECK(neverd_x64_continue_v7(&s,&budget)==6&&budget==2&&!memcmp(&s,&want,sizeof(s)));
  CHECK(neverd_x64_continue_v7(&s,&budget)==5&&budget==2&&!memcmp(&s,&want,sizeof(s)));
  s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};s.PC=0x1008;
  s.GPR[1]=0xffff800012345678ULL;s.GPR[2]=0x100b;want=s;
  want.GPR[4]=s.GPR[1];want.PC=0x100b;
  want.CS=(neverd_x64_segment){0,UINT32_MAX,0x1b,0xa0fb};
  want.SS=(neverd_x64_segment){0,UINT32_MAX,0x23,0xc0f3};
  CHECK(neverd_x64_continue_v7(&s,&budget)==6&&budget==1&&!memcmp(&s,&want,sizeof(s)));
  s.PC=0x7777;want=s;
  CHECK(neverd_x64_continue_v7(&s,&budget)==8&&budget==1&&!memcmp(&s,&want,sizeof(s)));
  return 0;
}
)C");
    const auto Input = tmpFile("fast-request.json"), Output = tmpFile("fast.c"),
               Report = tmpFile("fast.json");
    std::ofstream(Input) << Request;
    std::vector<std::string> Args{"decompile",
                                  tmpFile("input.elf").string(),
                                  "--x64-continuation=" + Input.string(),
                                  "--continuation-report=" + Report.string(),
                                  "-o",
                                  Output.string()};
    if (LLVM)
      Args.push_back("--llvm");
    auto Ran = exec(ndBin(), Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    EXPECT_EQ(readOutput(Output), Source);
    EXPECT_EQ(readOutput(Report), Manifest);
  }
  for (
      const std::string &Bad :
      {R"({"profile":"intel-fast-system-v7","mxcsr_mask":65471,"paging_address_bits":48,"instructions":[4100]})",
       R"({"profile":"kernel-events-v6","mxcsr_mask":65471,"paging_address_bits":48,"sysenter_msr_address_bits":48,"instructions":[4100]})",
       R"({"profile":"intel-fast-system-v7","mxcsr_mask":65471,"paging_address_bits":48,"sysenter_msr_address_bits":48,"segment_base_msr_address_bits":57,"instructions":[4100]})"}) {
    const char *Report = "stale";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(), Bad.size(),
                                                0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
  }
}

TEST_F(X64ContinuationExportTest, UserInterruptV8StopsBeforeTheReturnedTarget) {
  load({0xf3, 0x0f, 0x01, 0xee, 0xf3, 0x0f, 0x01, 0xef, 0xf3, 0x0f, 0x01, 0xed,
        0xf3, 0x0f, 0x01, 0xec, 0x90});
  const std::string Request =
      R"({"profile":"intel-user-interrupt-v8","mxcsr_mask":"0xffbf","paging_address_bits":48,"sysenter_msr_address_bits":48,"uiret_restores_uif":true,"follow_internal_calls":true,"instructions":[4096,4100,4104,4108,4112]})";
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM, 8);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    const auto *Object = Parsed->getAsObject();
    EXPECT_EQ(Object->getString("profile"), "intel-user-interrupt-v8");
    EXPECT_EQ(Object->getInteger("native_requests"), 0);
    EXPECT_EQ(Object->getArray("units")->size(), 5u);
    for (const auto &[Name, Offset] :
         std::vector<std::pair<std::string, unsigned>>{
             {"UIF", 2528}, {"MonitorActive", 2529}}) {
      const auto *Fields = Object->getArray("state_fields");
      const auto It =
          std::find_if(Fields->begin(), Fields->end(), [&](const auto &F) {
            return F.getAsObject()->getString("name") == Name;
          });
      ASSERT_NE(It, Fields->end());
      EXPECT_EQ(It->getAsObject()->getInteger("offset"), Offset);
      EXPECT_EQ(It->getAsObject()->getInteger("bytes"), 1);
    }
    execute(Source, R"C(
#include <stdlib.h>
#define CHECK(c) do {if(!(c))abort();}while(0)
int main(void) {
  neverd_x64_state_v8 s={0},want;
  uint64_t frame[]={0x1010,0x200,UINT64_MAX},before[3],budget=10;
  s.PC=0x1000;s.RFlags=0x202;s.MXCSR=0x1f80;
  s.CS=(neverd_x64_segment){0,UINT32_MAX,11,0xa0fb};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,19,0xc0f3};
  s.GPR[4]=(uintptr_t)frame;s.UIF=1;s.MonitorActive=1;s.NMIBlocked=1;
  memset(s.ZMM,0xa5,sizeof(s.ZMM));memcpy(before,frame,sizeof(frame));
  want=s;want.PC=0x1010;want.GPR[4]=UINT64_MAX;
  want.UIF=0;want.MonitorActive=0;
  CHECK(neverd_x64_continue_v8(&s,&budget)==11&&budget==6);
  CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(frame,before,sizeof(frame)));
  /* A listed target is still not executed at the UIRET boundary. */
  CHECK(neverd_x64_continue_v8(&s,&budget)==5&&budget==6);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  s.PC=0x100c;s.GPR[4]=(uintptr_t)frame;s.UIF=2;want=s;
  CHECK(neverd_x64_continue_v8(&s,&budget)==5&&budget==6&&!memcmp(&s,&want,sizeof(s)));
  return 0;
}
)C");
    const auto Input = tmpFile("user-request.json"), Output = tmpFile("user.c"),
               Report = tmpFile("user.json");
    std::ofstream(Input) << Request;
    std::vector<std::string> Args{"decompile",
                                  tmpFile("input.elf").string(),
                                  "--x64-continuation=" + Input.string(),
                                  "--continuation-report=" + Report.string(),
                                  "-o",
                                  Output.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Ran = exec(ndBin(), Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    EXPECT_EQ(readOutput(Output), Source);
    EXPECT_EQ(readOutput(Report), Manifest);
  }
  for (const auto &Binding :
       {std::string(), std::string(",\"uiret_restores_uif\":0")}) {
    const auto Bad =
        std::string(
            R"({"profile":"intel-user-interrupt-v8","mxcsr_mask":65471,"paging_address_bits":48,"sysenter_msr_address_bits":48,"instructions":[4108])") +
        Binding + "}";
    const char *Report = "stale";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(), Bad.size(),
                                                0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
  }
}

TEST_F(X64ContinuationExportTest, FredV9StopsBeforeTargetAndPendingEvents) {
  load({0xf2, 0x0f, 0x01, 0xca, 0xf3, 0x0f, 0x01, 0xca, 0x90});
  const std::string Request =
      R"({"profile":"intel-fred-return-v9","mxcsr_mask":"0xffbf","paging_address_bits":48,"follow_internal_calls":true,"instructions":[4096,4100,4104]})";
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM, 9);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    const auto *Object = Parsed->getAsObject();
    EXPECT_EQ(Object->getString("profile"), "intel-fred-return-v9");
    EXPECT_EQ(Object->getInteger("native_requests"), 1);
    EXPECT_EQ(Object->getInteger("modeled"), 2);
    EXPECT_EQ(Object->getArray("units")->size(), 3u);
    const auto *Choices =
        (*Object->getArray("units"))[1].getAsObject()->getArray(
            "arbitrary_effects");
    ASSERT_EQ(Choices->size(), 7u);
    for (size_t I = 0; I != Choices->size(); ++I) {
      const auto *Choice = (*Choices)[I].getAsObject();
      EXPECT_EQ(Choice->getInteger("slot"), I < 4 ? I : I + 1);
      EXPECT_EQ(Choice->getString("after_profile_transition"),
                "successful_compatibility_return");
      EXPECT_EQ(Choice->getBoolean("conditional"), true);
      EXPECT_FALSE(Choice->get("after_operation"));
    }
    execute(Source, R"C(
#include <stdlib.h>
#define CHECK(c) do {if(!(c))abort();}while(0)
int main(void) {
  neverd_x64_state_v9 s={0},want;
  uint64_t frame[]={0xdeadbeef,0x1004,8,0x302,UINT64_MAX,0x30010},before[6],budget=10;
  s.PC=0x1000;s.RFlags=0x202;s.MXCSR=0x1f80;
  s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
  s.GPR[4]=(uintptr_t)frame;s.UIF=1;s.MonitorActive=1;s.NMIBlocked=1;
  s.FredConfig=0xffff800012340003ULL;
  memset(s.ZMM,0xa5,sizeof(s.ZMM));memcpy(before,frame,sizeof(frame));
  want=s;want.PC=0x1004;want.GPR[4]=UINT64_MAX;want.RFlags=0x302;
  want.FredConfig&=~3ULL;want.STIBlocked=1;want.PendingDebug=1;
  CHECK(neverd_x64_continue_v9(&s,&budget)==12&&budget==9);
  CHECK(!memcmp(&s,&want,sizeof(s))&&!memcmp(frame,before,sizeof(frame)));
  CHECK(neverd_x64_continue_v9(&s,&budget)==5&&budget==9&&!memcmp(&s,&want,sizeof(s)));
  s.PendingDebug=0;s.RFlags=0x202;s.GPR[4]=(uintptr_t)frame;s.STAR=3ULL<<48;
  s.GSBase=0x1234;s.KernelGSBase=0x5678;
  frame[1]=0x1008;frame[2]=19;frame[3]=2;frame[4]=UINT64_MAX;frame[5]=0x4000b;
  want=s;want.PC=0x1008;want.GPR[4]=UINT64_MAX;want.RFlags=2;
  want.STIBlocked=0;want.NMIBlocked=0;want.GSBase=0x5678;want.KernelGSBase=0x1234;
  want.MonitorActive=0;
  want.CS=(neverd_x64_segment){0,UINT32_MAX,19,0xa0fb};
  want.SS=(neverd_x64_segment){0,UINT32_MAX,11,0xc0f3};
  CHECK(neverd_x64_continue_v9(&s,&budget)==12&&budget==8&&!memcmp(&s,&want,sizeof(s)));
  s.PC=0x1004;s.GPR[4]=(uintptr_t)frame;s.MonitorActive=1;
  s.CS=(neverd_x64_segment){0,UINT32_MAX,8,0xa09b};
  s.SS=(neverd_x64_segment){0,UINT32_MAX,16,0xc093};
  for(unsigned i=0;i<8;++i)s.Undefined[i]=0xabcdef0054321000ULL+i;
  frame[1]=0xdeadbeef00001008ULL;frame[2]=3;frame[4]=0xabcdef0087654321ULL;
  frame[5]=11;memcpy(before,frame,sizeof(frame));
  want=s;want.PC=0x1008;want.GPR[4]=0x87654321;want.MonitorActive=0;
  want.GSBase=0x1234;want.KernelGSBase=0x5678;
  want.CS=(neverd_x64_segment){0,UINT32_MAX,3,0xc0fb};
  want.SS=(neverd_x64_segment){0,UINT32_MAX,11,0xc0f3};
  for(unsigned i=0;i<8;++i)if(i!=4)
    want.GPR[i]=(uint32_t)want.GPR[i]|((uint64_t)(uint32_t)want.Undefined[i]<<32);
  CHECK(neverd_x64_continue_v9(&s,&budget)==12&&budget==7&&!memcmp(&s,&want,sizeof(s)));
  CHECK(!memcmp(frame,before,sizeof(frame)));
  CHECK(neverd_x64_continue_v9(&s,&budget)==5&&budget==7&&!memcmp(&s,&want,sizeof(s)));
  return 0;
}
)C");
    const auto Input = tmpFile("fred-request.json"), Output = tmpFile("fred.c"),
               Report = tmpFile("fred.json");
    std::ofstream(Input) << Request;
    std::vector<std::string> Args{"decompile",
                                  tmpFile("input.elf").string(),
                                  "--x64-continuation=" + Input.string(),
                                  "--continuation-report=" + Report.string(),
                                  "-o",
                                  Output.string()};
    if (LLVM)
      Args.push_back("--llvm");
    const auto Ran = exec(ndBin(), Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    EXPECT_EQ(readOutput(Output), Source);
    EXPECT_EQ(readOutput(Report), Manifest);
  }
}

TEST_F(X64ContinuationExportTest, FarReturnStopsBeforeTargetAndKeepsCleanup) {
  load({0x48, 0xca, 7, 0, 0xb8, 7, 0, 0, 0});
  const auto Request =
      request(R"("follow_internal_calls":true,"instructions":[4096,4100])");
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    uint64_t gdt[5]={0,0,0,0x00af9a000000ffffULL,0x00cf92000000ffffULL};
    unsigned char stack[128];
    for(unsigned cpl=0;cpl<4;++cpl) {
        neverd_x64_state_v6 s=seed();uint64_t budget=0,sp=(uintptr_t)stack;
        s.GPR[4]=sp;s.GDTR.Base=(uintptr_t)gdt;s.GDTR.Limit=sizeof(gdt)-1;
        gdt[3]=0x00af9a000000ffffULL|((uint64_t)cpl<<45);
        gdt[4]=0x00cf92000000ffffULL|((uint64_t)cpl<<45);
        uint64_t pc=0x1004,cs=24|cpl,new_sp=UINT64_MAX-3,ss=32|cpl;
        memcpy(stack,&pc,8);memcpy(stack+8,&cs,8);
        memcpy(stack+23,&new_sp,8);memcpy(stack+31,&ss,8);
        neverd_x64_state_v6 saved=s;
        CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !memcmp(&s,&saved,sizeof(s)));
        --s.GDTR.Limit;budget=2;
        if(cpl) {
            saved=s;CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==2);
            CHECK(!memcmp(&s,&saved,sizeof(s)) && !(gdt[3]&(1ULL<<40)));
        }
        ++s.GDTR.Limit;
        CHECK(neverd_x64_continue_v6(&s,&budget)==6 && budget==1);
        CHECK(s.PC==0x1004 && s.GPR[0]==saved.GPR[0]);
        CHECK(s.GPR[4]==(cpl?3:sp+23) && s.RFlags==0x246 && s.NMIBlocked==1);
        CHECK(s.CS.Selector==(24|cpl) && (gdt[3]&(1ULL<<40)));
        if(!cpl) {
            CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0);
            CHECK(s.PC==0x1009 && s.GPR[0]==7);
            budget=1;saved=s;
            CHECK(neverd_x64_continue_v6(&s,&budget)==8 && budget==1);
            CHECK(!memcmp(&s,&saved,sizeof(s)));
        } else {
            saved=s;CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==1);
            CHECK(!memcmp(&s,&saved,sizeof(s)));
        }
    }
    return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       FarPointerCommitsBothDestinationsAndResumesAtFallthrough) {
  load({0x0f, 0xb4, 3, 0x48, 0x0f, 0xb5, 0x0e}); // lfs eax,[rbx]; lgs rcx,[rsi]
  const auto Request = request(R"("instructions":[4096,4099])");
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    uint64_t gdt[5]={0,0,0,0,0x89c192efabcd2345ULL};
    unsigned char a[6],b[10];uint32_t offset=0x89abcdef;
    uint64_t wide=0xfedcba9876543210ULL;uint16_t selector=32;
    memcpy(a,&offset,4);memcpy(a+4,&selector,2);
    memcpy(b,&wide,8);memcpy(b+8,&selector,2);
    neverd_x64_state_v6 s=seed();s.GPR[3]=(uintptr_t)a;s.GPR[6]=(uintptr_t)b;
    s.GDTR.Base=(uintptr_t)gdt;s.GDTR.Limit=sizeof(gdt)-1;
    s.Unusable=48;uint64_t budget=0;neverd_x64_state_v6 saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !memcmp(&s,&saved,sizeof(s)));
    budget=1;CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0);
    CHECK(s.PC==0x1003 && s.GPR[0]==offset && s.GPR[1]==saved.GPR[1]);
    CHECK(s.FSBase==0x89efabcd && s.FS.Selector==32 && s.FS.Attributes==0xc093);
    CHECK(s.FS.Limit==0x12345fff && s.Unusable==32 && (gdt[4]&(1ULL<<40)));
    selector=0;memcpy(b+8,&selector,2);saved=s;budget=2;
    CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==2);
    CHECK(!memcmp(&s,&saved,sizeof(s)));
    selector=32;memcpy(b+8,&selector,2);
    CHECK(neverd_x64_continue_v6(&s,&budget)==8 && budget==1);
    CHECK(s.PC==0x1007 && s.GPR[1]==wide && s.GSBase==s.FSBase);
    CHECK(!memcmp(&s.GS,&s.FS,sizeof(s.FS)) && s.Unusable==0);
    CHECK(s.RFlags==0x246 && s.NMIBlocked==1);
    return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest, FarPointerNullPolicyAndStackSwitchResume) {
  // lfs eax,[rbx]; lgs rcx,[rsi]; lss rsp,[rdx]; mov rax,[rsp]; ret
  load({0x0f, 0xb4, 3, 0x48, 0x0f, 0xb5, 0x0e, 0x48, 0x0f, 0xb2, 0x22, 0x48,
        0x8b, 4, 0x24, 0xc3});
  for (llvm::StringRef Policy : {"clear", "preserve"})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Policy.str());
      SCOPED_TRACE(LLVM);
      auto Request = request(
          (R"("instructions":[4096,4099,4103,4107,4111],"null_segment_base_policy":")" +
           Policy + "\"")
              .str());
      auto [Source, Manifest] = source(Request, LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
      EXPECT_NE(Manifest.find(Policy.str()), std::string::npos);
      std::string Harness = Seed;
      Harness += std::string("static const unsigned clear_base=") +
                 (Policy == "clear" ? "1;\n" : "0;\n");
      Harness += R"C(
int main(void) {
    uint64_t stack[2]={0x98765432,0x6789};
    unsigned char a[6],b[10],c[10];uint32_t offset=0x89abcdef;
    uint64_t wide=0xfedcba9876543210ULL,sp=(uintptr_t)stack;
    uint16_t fs=1,gs=3,ss=0;
    memcpy(a,&offset,4);memcpy(a+4,&fs,2);
    memcpy(b,&wide,8);memcpy(b+8,&gs,2);
    memcpy(c,&sp,8);memcpy(c+8,&ss,2);
    neverd_x64_state_v6 s=seed(),initial=s;
    s.GPR[3]=(uintptr_t)a;s.GPR[6]=(uintptr_t)b;s.GPR[2]=(uintptr_t)c;
    s.GDTR.Base=1ULL<<47;s.GDTR.Limit=0;
    uint64_t budget=2;CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0);
    CHECK(s.PC==0x1007 && s.GPR[0]==offset && s.GPR[1]==wide);
    CHECK(s.FS.Selector==1 && s.GS.Selector==3 && (s.Unusable&48)==48);
    CHECK(s.FSBase==(clear_base?0:initial.FSBase) && s.GSBase==(clear_base?0:initial.GSBase));
    budget=1;CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0);
    CHECK(s.PC==0x100b && s.GPR[4]==sp && s.SS.Selector==0 && (s.Unusable&2));
    CHECK(s.SS.Base==initial.SS.Base && s.SS.Limit==initial.SS.Limit && s.SS.Attributes==initial.SS.Attributes);
    budget=2;CHECK(neverd_x64_continue_v6(&s,&budget)==3 && budget==0);
    CHECK(s.GPR[0]==stack[0] && s.PC==stack[0] && s.GPR[4]==sp+8);
    CHECK(s.RFlags==initial.RFlags && s.NMIBlocked==initial.NMIBlocked);
    return 0;
}
)C";
      execute(Source, Harness);
    }
}

TEST_F(X64ContinuationExportTest, SysretCompatibilityStopsBeforeUserCode) {
  load({0x0f, 0x07, 0x90});
  const auto Request = request(R"("instructions":[4096,4098])");
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
    const auto *Units = Parsed->getAsObject()->getArray("units");
    ASSERT_NE(Units, nullptr);
    ASSERT_EQ(Units->size(), 2u);
    const auto *Effects =
        Units->front().getAsObject()->getArray("arbitrary_effects");
    ASSERT_NE(Effects, nullptr);
    ASSERT_EQ(Effects->size(), 8u);
    for (unsigned I = 0; I != Effects->size(); ++I) {
      const auto *E = (*Effects)[I].getAsObject();
      ASSERT_NE(E, nullptr);
      EXPECT_EQ(E->getInteger("slot"), I);
      EXPECT_EQ(E->getInteger("after_operation"), 1);
      EXPECT_EQ(E->getString("output_offset"),
                "0x" + llvm::utohexstr(I * 8, true));
      EXPECT_EQ(E->getInteger("output_bytes"), 8);
      EXPECT_EQ(E->getInteger("bit_offset"), 32);
      EXPECT_EQ(E->getInteger("bit_count"), 32);
      EXPECT_EQ(E->getBoolean("conditional"), false);
    }
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  neverd_x64_state_v6 s=seed();s.GPR[1]=0x800000001002ULL;s.GPR[11]=0x246;
  s.GPR[4]=0x876543210abcdef0ULL;s.STAR=0xffff123456789abcULL;
  s.SS.Selector=0;s.FS.Selector=1;s.GS.Selector=3;s.LDTR.Selector=0;
  s.Unusable=0x72;s.GDTR.Base=UINT64_MAX;
  for(unsigned i=0;i<8;++i)s.Undefined[i]=0xfedcba9876543210ULL+i;
  neverd_x64_state_v6 want=s;want.PC=0x1002;want.RFlags=0x246;
  for(unsigned i=0;i<8;++i)want.GPR[i]=(uint32_t)s.GPR[i]|(s.Undefined[i]<<32);
  want.CS=(neverd_x64_segment){0,0xffffffff,0xffff,0xc0fb};
  want.SS=(neverd_x64_segment){0,0xffffffff,7,0xc0f3};want.Unusable&=~3;
  uint64_t budget=4;CHECK(neverd_x64_continue_v6(&s,&budget)==6 && budget==3);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==3 && !memcmp(&s,&want,sizeof(s)));
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest, DriverRetainsPhysicalCallsPivotsAndReentry) {
  load({0x48, 0x89, 0x4c, 0x24, 0x08,         // mov [rsp+8],rcx
        0xe8, 0x06, 0x00, 0x00, 0x00,         // call 1010
        0x48, 0x8b, 0x44, 0x24, 0x08,         // mov rax,[rsp+8]
        0xc3, 0x48, 0x8b, 0x22, 0xc3,         // ret; mov rsp,[rdx]; ret
        0xff, 0xe0, 0x0f, 0x33, 0xeb, 0xfe}); // jmp rax; rdpmc; jmp self
  const auto Request = request(
      R"("instructions":[4096,4101,4106,4111,4112,4115,4116,4118,4120])");
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getString("call_return_policy"), "stop");
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 1);
    EXPECT_EQ(Parsed->getAsObject()->getArray("units")->size(), 9u);
    EXPECT_EQ(Source.find("unknown register"), std::string::npos);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    header_contract();
    uint64_t a[16]={0},b[16]={0},new_sp=(uintptr_t)&b[4],budget=8;
    neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&a[4];s.GPR[2]=(uintptr_t)&new_sp;
    uint64_t input=s.GPR[1];
    CHECK(neverd_x64_continue_v6(&s,&budget)==1 && budget==6 && s.PC==0x1010);
    CHECK(a[3]==0x100a && a[5]==input && s.GPR[4]==(uintptr_t)&a[3]);
    b[4]=0x100a;b[5]=0x7777;b[6]=0xfedcba9876543210ULL;
    CHECK(neverd_x64_continue_v6(&s,&budget)==3 && budget==4 && s.PC==0x100a);
    CHECK(s.GPR[4]==(uintptr_t)&b[5]);
    CHECK(neverd_x64_continue_v6(&s,&budget)==3 && budget==2 && s.PC==0x7777);
    CHECK(s.GPR[0]==b[6] && s.GPR[4]==(uintptr_t)&b[6] && a[5]==input);
    neverd_x64_state_v6 saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==8 && budget==2 && !memcmp(&s,&saved,sizeof(s)));
    s=seed();s.PC=0x1014;s.GPR[0]=0x1018;budget=3;
    CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0 && s.PC==0x1018);
    saved=s;CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !memcmp(&s,&saved,sizeof(s)));
    s.PC=0x1014;s.GPR[0]=0x9999;budget=2;
    CHECK(neverd_x64_continue_v6(&s,&budget)==2 && s.PC==0x9999 && budget==1);
    s=seed();s.PC=0x1016;budget=5;saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==4 && budget==5 && !memcmp(&s,&saved,sizeof(s)));
    s=seed();s.RFlags|=1u<<17;s.GPR[4]=(uintptr_t)&a[4];saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==5 && !memcmp(&s,&saved,sizeof(s)));
    return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       InternalCallsAndReturnsFollowChangedPhysicalFrames) {
  load({0x48, 0x89, 0x4c, 0x24, 0x08,       // mov [rsp+8],rcx
        0xe8, 0x06, 0x00, 0x00, 0x00,       // call 1010
        0x48, 0x8b, 0x44, 0x24, 0x08,       // mov rax,[rsp+8]
        0xc3, 0x48, 0x8b, 0x22, 0xc3,       // ret; mov rsp,[rdx]; ret
        0xff, 0xe0, 0x0f, 0x33, 0xeb, 0xfe, // jmp rax; rdpmc; jmp self
        0xff, 0xd0, 0xc3});                 // call rax; ret
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto Request = request(
        R"("follow_internal_calls":true,"instructions":[4096,4101,4106,4111,4112,4115,4116,4118,4120,4122,4124])");
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getString("call_return_policy"),
              "follow-selected-targets");
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  for(unsigned i=0;i<64;++i) {
    uint64_t a[64]={0},b[16]={0},new_sp=(uintptr_t)&b[4];
    neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&a[40];s.GPR[2]=(uintptr_t)&new_sp;
    s.GPR[1]^=(uint64_t)i<<37;
    uint64_t input=s.GPR[1],result=0xfedcba9876543210ULL+i*71;
    b[4]=0x100a;b[5]=0x7777;b[6]=result;
    // Six original instructions cross CALL, stack replacement and both RETs.
    uint64_t budget=6;
    CHECK(neverd_x64_continue_v6(&s,&budget)==3 && budget==0);
    CHECK(s.PC==0x7777 && s.GPR[0]==result && s.GPR[4]==(uintptr_t)&b[6]);
    CHECK(a[39]==0x100a && a[41]==input);

    // The last available step may enter a selected callee, but cannot run it.
    s=seed();s.GPR[4]=(uintptr_t)&a[40];s.GPR[2]=(uintptr_t)&new_sp;budget=2;
    CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !budget && s.PC==0x1010);
    CHECK(s.GPR[4]==(uintptr_t)&a[39] && a[39]==0x100a);
    neverd_x64_state_v6 saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !memcmp(&s,&saved,sizeof(s)));
    // The environment replaces the return PC while stopped. Do not synthesize
    // the call's lexical continuation or restore its previous stack.
    b[4]=0x1014;s.GPR[0]=0x9999;budget=4;
    CHECK(neverd_x64_continue_v6(&s,&budget)==2 && budget==1 && s.PC==0x9999);
    CHECK(s.GPR[4]==(uintptr_t)&b[5] && s.GPR[0]==0x9999);

    // Indirect recursion consumes only the guest stack and instruction budget.
    s=seed();s.PC=0x101a;s.GPR[0]=0x101a;s.GPR[4]=(uintptr_t)&a[40];a[40]=0x7777;
    budget=32;
    CHECK(neverd_x64_continue_v6(&s,&budget)==9 && !budget && s.PC==0x101a);
    CHECK(s.GPR[4]==(uintptr_t)&a[8]);
    for(unsigned j=8;j<40;++j)CHECK(a[j]==0x101c);
    s.PC=0x101c;budget=33;
    CHECK(neverd_x64_continue_v6(&s,&budget)==3 && !budget && s.PC==0x7777);
    CHECK(s.GPR[4]==(uintptr_t)&a[41]);

    // An external indirect call reports CALL even on the last budget unit.
    s=seed();s.PC=0x101a;s.GPR[0]=0x8888;s.GPR[4]=(uintptr_t)&a[40];budget=1;
    CHECK(neverd_x64_continue_v6(&s,&budget)==1 && !budget && s.PC==0x8888);
    CHECK(s.GPR[4]==(uintptr_t)&a[39] && a[39]==0x101c);
    s=seed();s.PC=0x1016;budget=5;saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==4 && budget==5 && !memcmp(&s,&saved,sizeof(s)));
    s=seed();s.RFlags|=1u<<17;s.GPR[4]=(uintptr_t)&a[40];saved=s;
    CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==5 && !memcmp(&s,&saved,sizeof(s)));
  }
  return 0;
}
)C");

    // Omitting the callee retains the environment boundary. It may change the
    // return PC, replacement stack and every other register before reentry.
    auto [Partial, PartialManifest] = source(
        request(
            R"("follow_internal_calls":true,"instructions":[4096,4101,4106,4111])"),
        LLVM);
    execute(Partial, std::string(Seed) + R"C(
int main(void) {
  uint64_t a[16]={0},b[16]={0},budget=2;
  neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&a[4];
  CHECK(neverd_x64_continue_v6(&s,&budget)==1 && !budget && s.PC==0x1010);
  CHECK(a[3]==0x100a && s.GPR[4]==(uintptr_t)&a[3]);
  b[4]=0x7777;b[5]=0x8877665544332211ULL;s.GPR[4]=(uintptr_t)&b[4];s.PC=a[3];
  s.GPR[9]=0xfedcba9876543210ULL;budget=2;
  CHECK(neverd_x64_continue_v6(&s,&budget)==3 && !budget && s.PC==0x7777);
  CHECK(s.GPR[4]==(uintptr_t)&b[5] && s.GPR[0]==b[5] && s.GPR[9]==0xfedcba9876543210ULL);
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest, StarMsrDriverRetainsStateAndRefusalBudget) {
  load({0x0f, 0x30, 0x0f, 0x32, 0x48, 0x0f, 0x07});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] =
        source(request(R"("instructions":[4096,4098,4100])"), LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("native_requests"), 0);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
  neverd_x64_state_v6 s=seed(),want;
  s.GPR[1]=0x12345678c0000081ULL;s.GPR[0]=0xaabbccdd00000000ULL;
  s.GPR[2]=0x8765432100330020ULL;
  want=s;want.STAR=0x0033002000000000ULL;want.PC=0x1002;
  uint64_t budget=1;
  CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  want=s;want.GPR[0]=0;want.GPR[2]=0x00330020;want.PC=0x1004;budget=1;
  CHECK(neverd_x64_continue_v6(&s,&budget)==9 && budget==0);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  s.GPR[1]=0x12345678;s.GPR[11]=0x246;
  want=s;want.PC=0x12345678;want.RFlags=0x246;
  want.CS=(neverd_x64_segment){0,0xffffffff,0x43,0xa0fb};
  want.SS=(neverd_x64_segment){0,0xffffffff,0x3b,0xc0f3};budget=3;
  CHECK(neverd_x64_continue_v6(&s,&budget)==6 && budget==2);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  s=seed();s.GPR[1]=0xc0000081;s.GPR[0]=1;want=s;budget=4;
  CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==4);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  s.PC=0x1002;s.GPR[1]=0xc0000082;want=s;
  CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==4);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest, PrivilegeExitConsumesExactlyOneInstruction) {
  load({0x48, 0xcf, 0x90});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(
        request(
            R"("instructions":[4096,4098],"dispatch":"direct","follow_internal_calls":true)"),
        LLVM);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    uint64_t table[8]={0,0x00af9b000000ffffULL,0x00cf93000000ffffULL};
    uint64_t frame[]={0x1002,8,0x246,0x123400,16},budget=7;
    neverd_x64_state_v6 s=seed();s.GDTR=(neverd_x64_descriptor_table){(uintptr_t)table,63,0,0};
    s.GPR[4]=(uintptr_t)frame;
    CHECK(neverd_x64_continue_v6(&s,&budget)==6 && budget==6);
    CHECK(s.PC==0x1002 && s.GPR[4]==0x123400 && !s.NMIBlocked);
    return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       CompatibilityFrameExitPublishesConditionalChoicesAndStops) {
  for (unsigned Width : {2u, 4u, 8u}) {
    const uint8_t Prefix = Width == 2 ? 0x66 : 0x48;
    load(Width == 4 ? std::vector<uint8_t>{0xcf, 0x90, 0xcb, 0x90, 0x90}
                    : std::vector<uint8_t>{Prefix, 0xcf, Prefix, 0xcb, 0x90});
    for (bool LLVM : {false, true}) {
      auto [Source, Manifest] =
          source(request(R"("instructions":[4096,4098,4100])"), LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Units = Parsed->getAsObject()->getArray("units");
      ASSERT_NE(Units, nullptr);
      ASSERT_EQ(Units->size(), 3u);
      for (unsigned Kind = 0; Kind != 2; ++Kind) {
        const auto *Choices =
            (*Units)[Kind].getAsObject()->getArray("arbitrary_effects");
        ASSERT_NE(Choices, nullptr);
        ASSERT_EQ(Choices->size(), 8u);
        for (size_t I = 0; I != Choices->size(); ++I) {
          const auto *Choice = (*Choices)[I].getAsObject();
          EXPECT_EQ(Choice->getInteger("slot"), I);
          EXPECT_EQ(Choice->getInteger("bit_offset"), 32);
          EXPECT_EQ(Choice->getInteger("bit_count"), 32);
          EXPECT_EQ(Choice->getString("after_profile_transition"),
                    "successful_compatibility_return");
          EXPECT_EQ(Choice->getBoolean("conditional"), true);
          EXPECT_FALSE(Choice->get("after_operation"));
        }
      }
      EXPECT_TRUE(
          (*Units)[2].getAsObject()->getArray("arbitrary_effects")->empty());
      execute(Source, "#define FIELD_BYTES " + std::to_string(Width) + "\n" +
                          Seed + R"C(
int main(void) {
  for(unsigned kind=0;kind<2;++kind) {
    uint64_t table[8]={0,0x00af9b000000ffffULL,0x00cf93000000ffffULL,
                        0x00cf9b000000ffffULL,0x00cf93000000ffffULL};
    const uint64_t words[]={0xffff800000001004ULL,24,0x246,0x12345678abcdfff8ULL,32};
    unsigned char frame[40],before[40];uint64_t budget=7;
    memset(frame,0xa5,sizeof(frame));
    for(unsigned i=0;i<5;++i)memcpy(frame+i*FIELD_BYTES,&words[i],FIELD_BYTES);
    neverd_x64_state_v6 s=seed();s.PC=kind?0x1002:0x1000;
    s.GDTR=(neverd_x64_descriptor_table){(uintptr_t)table,63,0,0};
    s.GPR[4]=(uintptr_t)frame;
    for(unsigned i=0;i<8;++i)s.Undefined[i]=0xdeadbeef01234567ULL+i;
    neverd_x64_state_v6 want=s;
    want.PC=0x1004;want.CS=(neverd_x64_segment){0,UINT32_MAX,24,0xc09b};
    want.GPR[4]=kind?(uint32_t)(s.GPR[4]+2*FIELD_BYTES):
                    words[3]&(FIELD_BYTES==2?0xffff:0xffffffff);
    if(!kind) {
      want.SS=(neverd_x64_segment){0,UINT32_MAX,32,0xc093};want.NMIBlocked=0;
    }
    for(unsigned i=0;i<8;++i)
      want.GPR[i]=(uint32_t)want.GPR[i]|(want.Undefined[i]<<32);
    memcpy(before,frame,sizeof(frame));
    CHECK(neverd_x64_continue_v6(&s,&budget)==6 && budget==6);
    CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp(frame,before,sizeof(frame)));
    /* The selected NOP must not run in the returned compatibility context. */
    CHECK(neverd_x64_continue_v6(&s,&budget)==5 && budget==6);
    CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp(frame,before,sizeof(frame)));
  }
  return 0;
}
)C");
    }
  }
}

TEST_F(X64ContinuationExportTest,
       SelectedCFGDoesNotRequireAnOrdinaryReturnFrame) {
  load({0x48, 0x8b, 0x22, 0xc3});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(request(R"("entries":[4096])"), LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
    EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 2);
    const auto *Regions = Parsed->getAsObject()->getArray("regions");
    ASSERT_NE(Regions, nullptr);
    ASSERT_EQ(Regions->size(), 1u);
    const auto *Region = Regions->front().getAsObject();
    ASSERT_NE(Region, nullptr);
    EXPECT_TRUE(Region->getString("lowir_disposition").has_value());
    EXPECT_TRUE(Region->getInteger("lowir_unproved_returns").has_value());
    EXPECT_TRUE(
        Region->getInteger("lowir_unsupported_instructions").has_value());
    EXPECT_EQ(Region->get("ordinary_disposition"), nullptr);
    EXPECT_EQ(Region->get("unproved_ordinary_returns"), nullptr);
    EXPECT_EQ(Region->get("ordinary_unsupported_instructions"), nullptr);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    uint64_t stack[]={0x7777},sp=(uintptr_t)stack,budget=4;
    neverd_x64_state_v6 s=seed();s.GPR[2]=(uintptr_t)&sp;
    CHECK(neverd_x64_continue_v6(&s,&budget)==3 && budget==2 && s.PC==0x7777);
    CHECK(s.GPR[4]==sp+8);return 0;
}
)C");
  }
}

TEST_F(X64ContinuationExportTest,
       StrictRequestsAndBoundariesNeverPublishPartialSource) {
  const char *Report = "old";
  EXPECT_EQ(neverd_x64_continuation_source_v1(nullptr, "{}", 2, 0, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
  EXPECT_EQ(neverd_x64_continuation_source_v1(Session, "{}", 2, 0, &Report),
            nullptr);
  load({0x90, 0xc3, 0x0f});
  const auto Good = request(R"("instructions":[4096])");
  for (
      const auto &Bad : std::vector<std::string>{
          "{}",
          "[]",
          Good + "!",
          request(R"("instructions":[])"),
          request(R"("instructions":[4096,4096])"),
          request(R"("instructions":[-1])"),
          request(R"("instructions":[4096.25])"),
          request(R"("instructions":[4096],"entries":[4096])"),
          request(R"("instructions":[4096],"native_timestamp_reads":1)"),
          request(R"("instructions":[4096],"native_debug_register_access":1)"),
          request(R"("instructions":[4096],"native_page_global_access":null)"),
          request(R"("instructions":[4096],"native_rtm_abort_regions":1)"),
          request(R"("instructions":[4096],"native_rtm_abort_regions":null)"),
          R"({"profile":"intel-fast-system-v7","mxcsr_mask":"0xffbf","paging_address_bits":48,"sysenter_msr_address_bits":48,"instructions":[4096],"native_rtm_abort_regions":true})",
          request(R"("instructions":[4096],"follow_internal_calls":1)"),
          request(R"("instructions":[4096],"follow_internal_calls":null)"),
          request(R"("entries":[4096],"follow_direct_jumps":1)"),
          request(R"("entries":[4096],"follow_direct_jumps":null)"),
          request(R"("instructions":[4096],"follow_direct_jumps":true)"),
          request(R"("instructions":[4096],"follow_direct_jumps":false)"),
          request(R"("instructions":[4096],"dispatch":true)"),
          request(R"("instructions":[4096],"dispatch":null)"),
          request(R"("instructions":[4096],"dispatch":"unchecked")"),
          request(
              R"("instructions":[4096],"callee_depth":1,"follow_internal_calls":true)"),
          request(R"("entries":[4096],"callee_depth":1)"),
          request(
              R"("entries":[4096],"callee_depth":9,"follow_internal_calls":true)"),
          request(
              R"("entries":[4096],"callee_depth":true,"follow_internal_calls":true)"),
          request(R"("instructions":[4096],"null_segment_base_policy":true)"),
          request(R"("instructions":[4096],"null_segment_base_policy":null)"),
          request(
              R"("instructions":[4096],"null_segment_base_policy":"intel")"),
          request(R"("instructions":[4096],"unknown_option":true)"),
          request(R"("instructions":[4096],"max_instructions":0)"),
          request(R"("instructions":[4096],"max_operations":1)"),
          request(R"("instructions":[4098])"),
          request(R"("instructions":[8192])")}) {
    SCOPED_TRACE(Bad);
    Report = "old";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Bad.data(), Bad.size(),
                                                0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
    EXPECT_FALSE(take(neverd_last_error(Session)).empty());
  }
  Report = "old";
  EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Good.data(), Good.size(),
                                              2, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
  auto Input = Good + "!";
  auto [Source, Manifest] =
      source(llvm::StringRef(Input.data(), Good.size()), false);
  EXPECT_FALSE(Source.empty());
  EXPECT_EQ(take(neverd_x64_continuation_source_v1(Session, Good.data(),
                                                   Good.size(), 0, nullptr)),
            Source);
  EXPECT_TRUE(take(neverd_last_error(Session)).empty());
  Report = "old";
  EXPECT_EQ(
      neverd_x64_continuation_source_v1(
          Session, Good.data(), std::numeric_limits<size_t>::max(), 0, &Report),
      nullptr);
  EXPECT_EQ(Report, nullptr);
}

TEST_F(X64ContinuationExportTest,
       SpecialCallRegionsKeepUninitializedArgumentSlotsAndTheirAliases) {
  // An independently authored caller leaves its sixth argument untouched.
  // The first callee ignores it; the mutation reads that exact physical slot.
  // Neither case permits the exporter to invent zero or infer a source ABI.
  const std::vector<uint8_t> Caller = {
      0x48, 0x83, 0xec, 0x38,                    // sub rsp,56
      0x48, 0xc7, 0x44, 0x24, 0x20, 11, 0, 0, 0, // arg5 = 11
      0x48, 0xc7, 0x44, 0x24, 0x30, 7,  0, 0, 0, // arg7 = 7
      0xe8, 0x15, 0,    0,    0,                 // call 1030
      0x48, 0x83, 0xc4, 0x38, 0xc3};             // add rsp,56; ret
  for (bool ReadsGap : {false, true}) {
    auto Code = Caller;
    Code.resize(0x30, 0x90);
    Code.insert(Code.end(),
                {0x48, 0x8b, 0x44, 0x24, uint8_t(ReadsGap ? 0x30 : 0x38), 0x48,
                 0x03, 0x44, 0x24, 0x28, 0xc3});
    std::string Native = "static const unsigned char original_bytes[]={";
    for (uint8_t Byte : Code)
      Native += std::to_string(Byte) + ",";
    Native += R"C(};
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
static uint64_t run_original(void *code, uint64_t *sp) {
  uint64_t result;
  /* This bounded fixture never writes R11. Preserve the real host stack
     there while running the original bytes on the caller-owned test stack. */
  __asm__ volatile("mov %%rsp,%%r11\n\tmov %[sp],%%rsp\n\t"
                   "call *%[code]\n\tmov %%r11,%%rsp"
                   : "=&a"(result) : [code]"r"(code), [sp]"r"(sp)
                   : "r11", "memory", "cc");
  return result;
}
#endif
)C";
    load(Code);
    for (bool LLVM : {false, true}) {
      auto [Source, Manifest] = source(
          request(
              R"("entries":[4096],"callee_depth":1,"follow_internal_calls":true,"dispatch":"direct")"),
          LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      const auto *Regions = Parsed->getAsObject()->getArray("regions");
      ASSERT_NE(Regions, nullptr);
      ASSERT_EQ(Regions->size(), 2u);
      EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 9);
      EXPECT_EQ((*Regions)[0].getAsObject()->getInteger("callee_depth"), 0);
      EXPECT_EQ((*Regions)[1].getAsObject()->getInteger("callee_depth"), 1);
      EXPECT_EQ((*Regions)[1].getAsObject()->getString("entry"), "0x1030");
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos);
      execute(Source, std::string("#define READS_GAP ") +
                          (ReadsGap ? "1\n" : "0\n") + Seed + Native + R"C(
int main(void) {
#if defined(__linux__) && defined(__x86_64__)
  void *code=mmap(0,sizeof(original_bytes),PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(code!=MAP_FAILED);memcpy(code,original_bytes,sizeof(original_bytes));
  CHECK(!mprotect(code,sizeof(original_bytes),PROT_READ|PROT_EXEC));
#endif
  for(unsigned i=0;i<64;++i) {
    uint64_t stack[64],want[64];
    for(unsigned j=0;j<64;++j)stack[j]=0x1234567800000000ULL+j*71+i;
    stack[40]=0x7777;memcpy(want,stack,sizeof(stack));
    want[32]=0x101b;want[37]=11;want[39]=7;
    neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&stack[40];
    uint64_t budget=9;
    CHECK(neverd_x64_continue_v6(&s,&budget)==3 && !budget && s.PC==0x7777);
    CHECK(s.GPR[0]==(READS_GAP?stack[38]:7)+11);
    CHECK(s.GPR[4]==(uintptr_t)&stack[41] && !memcmp(stack,want,sizeof(stack)));
#if defined(__linux__) && defined(__x86_64__)
    uint64_t native[64];memcpy(native,stack,sizeof(native));
    CHECK(run_original(code,&native[41])==s.GPR[0]);
    /* Native CALLs retain host PCs, while the generated steps retain image PCs. */
    CHECK(native[32]==(uintptr_t)code+0x1b);
    native[32]=stack[32];native[40]=stack[40];
    CHECK(!memcmp(native,stack,sizeof(native)));
#endif
  }
#if defined(__linux__) && defined(__x86_64__)
  CHECK(!munmap(code,sizeof(original_bytes)));
#endif
  return 0;
}
)C");
    }
    const auto TooSmall = request(
        R"("entries":[4096],"callee_depth":1,"follow_internal_calls":true,"max_instructions":8)");
    const char *Report = "old";
    EXPECT_EQ(neverd_x64_continuation_source_v1(Session, TooSmall.data(),
                                                TooSmall.size(), 0, &Report),
              nullptr);
    EXPECT_EQ(Report, nullptr);
  }
  // Recursive edges do not start new discovery waves or duplicate a region.
  load({0xe8, 0xfb, 0xff, 0xff, 0xff, 0xc3});
  auto [Source, Manifest] = source(
      request(
          R"("entries":[4096],"callee_depth":8,"follow_internal_calls":true,"dispatch":"direct")"),
      false);
  auto Parsed = llvm::json::parse(Manifest);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_EQ(Parsed->getAsObject()->getArray("regions")->size(), 1u);
  EXPECT_EQ(Parsed->getAsObject()->getInteger("total"), 2);
}

TEST_F(X64ContinuationExportTest,
       DirectSpecialDispatchMatchesEveryPhysicalStopAndBudget) {
  std::vector<uint8_t> Code = {
      0x48, 0x89, 0x4c, 0x24, 0x08,       // mov [rsp+8],rcx
      0xe8, 0x06, 0x00, 0x00, 0x00,       // call 1010
      0x48, 0x8b, 0x44, 0x24, 0x08,       // mov rax,[rsp+8]
      0xc3, 0x48, 0x8b, 0x22, 0xc3,       // ret; mov rsp,[rdx]; ret
      0xff, 0xe0, 0x0f, 0x33, 0xeb, 0xfe, // jmp rax; rdpmc; jmp self
      0xff, 0xd0, 0xc3};                  // call rax; ret
  // Cross multiple independently compiled regions, including CALL/RET on the
  // last budget unit and a fallthrough at the region partition boundary.
  Code.insert(Code.end(), 128, 0x90);
  Code.push_back(0xc3);
  load(Code);
  std::string Addresses =
      "4096,4101,4106,4111,4112,4115,4116,4118,4120,4122,4124";
  for (uint64_t PC = 0x101d; PC <= 0x109d; ++PC)
    Addresses += "," + std::to_string(PC);
  for (bool LLVM : {false, true})
    for (bool Follow : {false, true}) {
      const std::string Selection = "\"instructions\":[" + Addresses +
                                    R"(],"follow_internal_calls":)" +
                                    (Follow ? "true" : "false");
      auto [Switch, SwitchManifest] = source(request(Selection), LLVM);
      auto [Direct, DirectManifest] =
          source(request(Selection + R"(,"dispatch":"direct")"), LLVM);
      const std::string Driver = "uint64_t neverd_x64_continue_v6(";
      const auto Begin = Direct.find(
          "static __attribute__((noinline)) uint64_t neverd_x64_region_");
      ASSERT_NE(Begin, std::string::npos);
      const auto End = Direct.find("typedef int (*neverd_x64_environment_v6)");
      ASSERT_NE(End, std::string::npos);
      ASSERT_LT(Begin, End);
      // Both routes share the same exact steps; link both independently
      // generated drivers to those steps for complete state/memory comparison.
      // The separate environment companion is tested with each driver above.
      Direct = Direct.substr(Begin, End - Begin);
      Direct.replace(Direct.find(Driver), Driver.size(),
                     "uint64_t direct_driver(");
      execute(Switch + Direct, std::string(Seed) + R"C(
int main(void) {
  const uint64_t pcs[]={0x1000,0x1005,0x100a,0x100f,0x1010,0x1013,
                        0x1014,0x1016,0x1018,0x101a,0x101c,0x1050,
                        0x1090,0x109d,0x9999};
  for(unsigned p=0;p<sizeof(pcs)/sizeof(*pcs);++p)
    for(unsigned n=0;n<35;++n)for(unsigned invalid=0;invalid<2;++invalid) {
      uint64_t a[64],b[16],asaved[64],bsaved[16],awant[64],bwant[16];
      for(unsigned j=0;j<64;++j)a[j]=0x7777;
      for(unsigned j=0;j<16;++j)b[j]=0x7777;
      uint64_t new_sp=(uintptr_t)&b[4];b[4]=0x100a;b[6]=0x8888;
      memcpy(asaved,a,sizeof(a));memcpy(bsaved,b,sizeof(b));
      neverd_x64_state_v6 s=seed();s.PC=pcs[p];s.GPR[0]=(n&1)?0x1090:0x101a;
      s.GPR[4]=(uintptr_t)&a[40];s.GPR[2]=(uintptr_t)&new_sp;
      if(invalid)s.RFlags|=1u<<17;
      neverd_x64_state_v6 want=s;uint64_t budget=n,expected_budget=n;
      uint64_t expected=neverd_x64_continue_v6(&want,&expected_budget);
      memcpy(awant,a,sizeof(a));memcpy(bwant,b,sizeof(b));
      memcpy(a,asaved,sizeof(a));memcpy(b,bsaved,sizeof(b));
      CHECK(direct_driver(&s,&budget)==expected && budget==expected_budget);
      CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp(a,awant,sizeof(a)) &&
            !memcmp(b,bwant,sizeof(b)));
    }
  return 0;
}
)C");
    }
}

TEST_F(X64ContinuationExportTest,
       SpecialRegionDepthFrontierRetainsTheExternalCall) {
  std::vector<uint8_t> Code(0x26, 0x90);
  const uint8_t Relay[] = {0xe8, 0x0b, 0, 0, 0, 0xc3};
  std::copy(std::begin(Relay), std::end(Relay), Code.begin());
  std::copy(std::begin(Relay), std::end(Relay), Code.begin() + 0x10);
  const uint8_t Leaf[] = {0xb8, 7, 0, 0, 0, 0xc3};
  std::copy(std::begin(Leaf), std::end(Leaf), Code.begin() + 0x20);
  load(Code);
  for (unsigned Depth : {1u, 2u})
    for (bool LLVM : {false, true}) {
      const auto Request = request(
          R"("entries":[4096],"follow_internal_calls":true,"dispatch":"direct","callee_depth":)" +
          std::to_string(Depth));
      auto [Source, Manifest] = source(Request, LLVM);
      auto Parsed = llvm::json::parse(Manifest);
      ASSERT_TRUE(bool(Parsed));
      EXPECT_EQ(Parsed->getAsObject()->getArray("regions")->size(), Depth + 1);
      execute(Source, "#define DEPTH " + std::to_string(Depth) + "\n" + Seed +
                          R"C(
int main(void) {
  uint64_t stack[16]={0},budget=6;stack[8]=0x7777;
  neverd_x64_state_v6 s=seed();s.GPR[4]=(uintptr_t)&stack[8];
  uint64_t result=neverd_x64_continue_v6(&s,&budget);
  CHECK(stack[7]==0x1005 && stack[6]==0x1015);
  if(DEPTH==1) {
    CHECK(result==1 && budget==4 && s.PC==0x1020);
    CHECK(s.GPR[4]==(uintptr_t)&stack[6]);
  } else {
    CHECK(result==3 && !budget && s.PC==0x7777 && s.GPR[0]==7);
    CHECK(s.GPR[4]==(uintptr_t)&stack[9]);
  }
  return 0;
}
)C");
    }
}

TEST_F(X64ContinuationExportTest, RawImagesRetainTheirExistingNativeBoundary) {
  std::ofstream(tmpFile("raw.bin"), std::ios::binary).write("\x90\xc3", 2);
  ASSERT_EQ(
      neverd_session_set_load_options(
          Session,
          R"({"loader":"binary","processor":"x86_64","base":"0x1000","offset":0,"size":0})"),
      0);
  ASSERT_TRUE(
      neverd_session_load(Session, tmpFile("raw.bin").string().c_str()));
  const auto Request = request(R"("instructions":[4096])");
  const char *Report = "old";
  EXPECT_EQ(neverd_x64_continuation_source_v1(Session, Request.data(),
                                              Request.size(), 0, &Report),
            nullptr);
  EXPECT_EQ(Report, nullptr);
  EXPECT_NE(take(neverd_last_error(Session)).find("linked x64 ELF or PE"),
            std::string::npos);
}

TEST_F(X64ContinuationExportTest,
       CLIExportsTheSameSourceAndRequiresItsManifest) {
  load({0x90, 0xc3});
  const auto Request = request(R"("instructions":[4096,4097])");
  const auto Input = tmpFile("request.json"), Output = tmpFile("output.c"),
             Report = tmpFile("manifest.json");
  std::ofstream(Input) << Request;
  const std::vector<std::string> Common{
      "decompile", tmpFile("input.elf").string(),
      "--x64-continuation=" + Input.string(), "-o", Output.string()};
  EXPECT_FALSE(exec(ndBin(), Common).ok());
  EXPECT_FALSE(fs::exists(Output));
  for (bool LLVM : {false, true}) {
    auto Args = Common;
    Args.push_back("--continuation-report=" + Report.string());
    if (LLVM)
      Args.push_back("--llvm");
    auto Ran = exec(ndBin(), Args);
    ASSERT_TRUE(Ran.ok()) << Ran.err;
    auto [Source, Manifest] = source(Request, LLVM);
    EXPECT_EQ(readOutput(Output), Source);
    EXPECT_EQ(readOutput(Report), Manifest);
    auto Invalid = Args;
    Invalid.push_back("--func=0x1000");
    EXPECT_FALSE(exec(ndBin(), Invalid).ok());
    EXPECT_EQ(readOutput(Output), Source);
    auto Aliased = Common;
    Aliased.push_back("--continuation-report=" + Output.string());
    EXPECT_FALSE(exec(ndBin(), Aliased).ok());
    EXPECT_EQ(readOutput(Output), Source);
  }
}
const char RtmOriginalOracle[] = R"C(
#include <cpuid.h>
#include <sys/mman.h>
/* Execute the original relative machine code, independent of the lifted
 * instructions. Protect the compiler's red zone and restore its ABI DF. */
static void original(neverd_x64_state_v6 *s, void *code, uint64_t *ram) {
  uint64_t a=s->GPR[0],b=s->GPR[3],c=s->GPR[1],d=s->GPR[2],f=s->RFlags;
  __asm__ volatile("lea -128(%%rsp),%%rsp; push %[flags]; popfq;"
                   "call *%[code]; pushfq; pop %[flags]; cld;"
                   "lea 128(%%rsp),%%rsp"
                   : "+a"(a),"+b"(b),"+c"(c),"+d"(d),[flags]"+r"(f)
                   : [code]"r"(code),"D"(ram)
                   : "r8","r9","r10","r11","memory","cc");
  s->GPR[0]=a;s->GPR[3]=b;s->GPR[1]=c;s->GPR[2]=d;s->RFlags=f;
}
)C";

#include "X64AlignmentContinuationTests.inc"
#include "X64ContinuationEnvironmentTests.inc"
#include "X64EventDeliveryExportTests.inc"
#include "X64HleContinuationExportTests.inc"
#include "X64HyperVContinuationExportTests.inc"
#include "X64JumpRegionContinuationTests.inc"
#include "X64KvmContinuationExportTests.inc"
#include "X64MpxContinuationExportTests.inc"
#include "X64NativeRegisterExportTests.inc"
#include "X64RtmAbortExportTests.inc"
#include "X64RtmPrefixExportTests.inc"
#include "X64RtmRegionExportTests.inc"
#include "X64SgxContinuationExportTests.inc"
#include "X64SmmContinuationExportTests.inc"
#include "X64SnpContinuationExportTests.inc"
#include "X64SparseFrameContinuationTests.inc"
#include "X64SvmContinuationExportTests.inc"

} // namespace
