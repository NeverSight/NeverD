//===- X64ContinuationExportTests.cpp - State source public contract -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute exported C and verify the real SDK and CLI retain exact boundaries.
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
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
  void load(const std::vector<uint8_t> &Code) {
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
  std::pair<std::string, std::string> source(llvm::StringRef Request,
                                             bool LLVM) {
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
      EXPECT_EQ(Object->getInteger("state_bytes"), 2496);
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
  const auto Request = request(R"("instructions":[4096,4098,4100])");
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

TEST_F(X64ContinuationExportTest, FarReturnStopsBeforeTargetAndKeepsCleanup) {
  load({0x48, 0xca, 7, 0, 0xb8, 7, 0, 0, 0});
  const auto Request = request(R"("instructions":[4096,4100])");
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

TEST_F(X64ContinuationExportTest, DriverRetainsPhysicalCallsPivotsAndReentry) {
  load({0x48, 0x89, 0x4c, 0x24, 0x08,         // mov [rsp+8],rcx
        0xe8, 0x06, 0x00, 0x00, 0x00,         // call 1010
        0x48, 0x8b, 0x44, 0x24, 0x08,         // mov rax,[rsp+8]
        0xc3, 0x48, 0x8b, 0x22, 0xc3,         // ret; mov rsp,[rdx]; ret
        0xff, 0xe0, 0x0f, 0x32, 0xeb, 0xfe}); // jmp rax; rdmsr; jmp self
  const auto Request = request(
      R"("instructions":[4096,4101,4106,4111,4112,4115,4116,4118,4120])");
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    auto [Source, Manifest] = source(Request, LLVM);
    auto Parsed = llvm::json::parse(Manifest);
    ASSERT_TRUE(bool(Parsed));
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

TEST_F(X64ContinuationExportTest, PrivilegeExitConsumesExactlyOneInstruction) {
  load({0x48, 0xcf});
  for (bool LLVM : {false, true}) {
    auto [Source, Manifest] = source(request(R"("instructions":[4096])"), LLVM);
    execute(Source, std::string(Seed) + R"C(
int main(void) {
    uint64_t table[8]={0,0x00af9b000000ffffULL,0x00cf93000000ffffULL};
    uint64_t frame[]={0x1800,8,0x246,0x123400,16},budget=7;
    neverd_x64_state_v6 s=seed();s.GDTR=(neverd_x64_descriptor_table){(uintptr_t)table,63,0,0};
    s.GPR[4]=(uintptr_t)frame;
    CHECK(neverd_x64_continue_v6(&s,&budget)==6 && budget==6);
    CHECK(s.PC==0x1800 && s.GPR[4]==0x123400 && !s.NMIBlocked);
    return 0;
}
)C");
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
  for (const auto &Bad : std::vector<std::string>{
           "{}", "[]", Good + "!", request(R"("instructions":[])"),
           request(R"("instructions":[4096,4096])"),
           request(R"("instructions":[-1])"),
           request(R"("instructions":[4096.25])"),
           request(R"("instructions":[4096],"entries":[4096])"),
           request(R"("instructions":[4096],"native_timestamp_reads":1)"),
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
} // namespace
