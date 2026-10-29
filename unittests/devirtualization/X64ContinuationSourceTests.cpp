//===- X64ContinuationSourceTests.cpp - Physical state and reentry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute continuation source against independent state and memory oracles.
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::analysis;

BinaryImage image(std::vector<uint8_t> Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = 0x1000;
  Segment S;
  S.Name = ".text";
  S.VA = Image.Entry;
  S.Size = S.FileSz = Bytes.size();
  S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  S.Data = std::move(Bytes);
  Image.Segments.push_back(std::move(S));
  return Image;
}

X64ContinuationSourceOptions options() {
  X64ContinuationSourceOptions O;
  O.Profile = X64ContinuationProfile::KernelNoFaultV1;
  return O;
}

constexpr char StateDeclaration[] = R"C(
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef struct __attribute__((aligned(16))) {
  uint64_t gpr[16], flags, pc, fs, gs;
  unsigned char zmm[32][64];
  uint64_t arbitrary[8];
} state_t;
_Static_assert(sizeof(state_t) == 2272, "continuation v1 layout");
#define ARG(s) ((uint8_t *)(s))
#define CHECK(c) do { if (!(c)) abort(); } while (0)
static state_t seed(uint64_t pc) {
  state_t s = {0};
  for (unsigned i=0; i<16; ++i) s.gpr[i] = 0x1234567800000000ULL+i*71;
  for (unsigned i=0; i<sizeof(s.zmm); ++i) ((unsigned char *)s.zmm)[i] = i*13+7;
  s.flags=0x246; s.pc=pc;
  return s;
}
typedef struct __attribute__((aligned(16))) {
  state_t core;
  uint64_t cr8, kernel_gs;
  uint32_t mxcsr, reserved;
  uint64_t reserved_tail;
} state_v2_t;
_Static_assert(sizeof(state_v2_t) == 2304, "continuation v2 layout");
static state_v2_t seed_v2(uint64_t pc) {
  state_v2_t s={0}; s.core=seed(pc); s.cr8=3; s.mxcsr=0x1f80;
  return s;
}
typedef struct __attribute__((aligned(16))) {
  state_v2_t core;
  uint64_t cr2, reserved;
} state_v4_t;
_Static_assert(sizeof(state_v4_t) == 2320, "continuation v4 layout");
typedef struct {
  uint64_t base;
  uint32_t limit;
  uint16_t selector,attributes;
} segment_t;
typedef struct __attribute__((aligned(16))) {
  state_v4_t core;
  uint64_t star;
  segment_t cs,ss;
  uint64_t reserved;
} state_v5_t;
_Static_assert(sizeof(state_v5_t) == 2368, "continuation v5 layout");
static state_v5_t seed_v5(uint64_t pc) {
  state_v5_t s={0};s.core.core=seed_v2(pc);
  s.cs=(segment_t){0,0xffffffff,8,0xa09b};
  s.ss=(segment_t){0,0xffffffff,16,0xc093};
  s.star=0x0023001000000000ULL;
  return s;
}
)C";

class X64ContinuationSourceTest : public NeverDLiftTest {
protected:
  void run(std::vector<uint8_t> Bytes, std::vector<va_t> Entries,
           llvm::StringRef Harness, bool NativeBoundary = false,
           X64ContinuationSourceOptions Options = options(),
           llvm::StringRef RequiredSpelling = {}, bool NativeKernel = false) {
    auto Image = image(std::move(Bytes));
    auto Units = buildX64ContinuationSource(Image, Entries, Options);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    ASSERT_EQ(Units->size(), Entries.size());
    for (const auto &Unit : *Units) {
      EXPECT_EQ(Unit.RequiresNativeExecution, NativeBoundary)
          << Unit.Address << " " << Unit.Diagnostic;
      const bool V6 =
          Options.Profile == X64ContinuationProfile::KernelEventsNoFaultV6;
      const bool V2 =
          Options.Profile == X64ContinuationProfile::KernelControlsNoFaultV2 ||
          Options.Profile == X64ContinuationProfile::KernelSmapNoFaultV3 ||
          Options.Profile ==
              X64ContinuationProfile::KernelFaultAddressNoFaultV4 ||
          Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5;
      const bool V3 =
          Options.Profile == X64ContinuationProfile::KernelSmapNoFaultV3;
      const bool V4 = Options.Profile ==
                      X64ContinuationProfile::KernelFaultAddressNoFaultV4;
      const bool V5 =
          Options.Profile == X64ContinuationProfile::KernelReturnNoFaultV5;
      EXPECT_EQ(Unit.StateVersion, V6   ? 6u
                                   : V5 ? 5u
                                   : V4 ? 4u
                                   : V3 ? 3u
                                   : V2 ? 2u
                                        : 1u);
      EXPECT_EQ(Unit.StateBytes, V6   ? sizeof(X64ContinuationStateV6)
                                 : V5 ? sizeof(X64ContinuationStateV5)
                                 : V4 ? sizeof(X64ContinuationStateV4)
                                 : V2 ? sizeof(X64ContinuationStateV2)
                                      : sizeof(X64ContinuationStateV1));
    }
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      std::vector<MedFunc> Meds;
      for (const auto &Unit : *Units) {
        LowToMedConverter Convert;
        const std::map<va_t, SourceFunctionTypeHint> Hints{
            {Unit.Function.Entry, Unit.SourceABI}};
        Convert.setSourceCallHintsEnabled(true);
        Convert.setSourceEntryTypeHints(&Hints);
        auto Med = Convert.convert(Unit.Function, Arch::X64);
        Med.SourceTypeHint = Unit.SourceABI;
        inferMedTypes(Med, Arch::X64);
        ASSERT_TRUE(verifyMedFunc(Med, "continuation-test"));
        EXPECT_EQ(Med.Params.size(), 1u);
        EXPECT_EQ(Med.FrameSize, 0);
        Meds.push_back(std::move(Med));
      }
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions O;
      O.TheArch = Arch::X64;
      O.Format = BinaryFormat::ELF;
      O.UseUnalignedPointers = false;
      if (LLVM) {
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit(Meds, Context);
        ASSERT_NE(Module, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, O));
      } else {
        std::vector<HighFunc> High;
        for (const auto &Med : Meds)
          High.push_back(MedToHighConverter().convert(Med, Arch::X64));
        ASSERT_TRUE(HighCEmitter().emit(High, OS, O));
      }
      OS.flush();
      if (!RequiredSpelling.empty())
        EXPECT_NE(Source.find(RequiredSpelling.str()), std::string::npos);
      ASSERT_EQ(Source.find("unknown register"), std::string::npos) << Source;
      const auto File =
          tmpFile(LLVM ? "continuation-llvm.c" : "continuation-high.c");
      std::ofstream(File) << StateDeclaration << "\n#include \""
                          << TEST_SOURCE_DIR
                          << "/fixtures/x64_continuation_event_state.h\"\n"
                          << Source << Harness.str();
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        if (NativeKernel) {
#if !defined(__linux__) || !defined(__x86_64__)
          GTEST_SKIP() << "native supervisor fixtures require Linux x64 KVM";
#else
          const auto Ran = exec(
              "python3",
              {std::string(TEST_SOURCE_DIR) + "/run_x64_continuation_kvm.py",
               "--clang", NEVERD_TEST_CLANG, "--source", File.string(),
               "--optimization", Optimization + 1});
          if (Ran.exitCode == 77)
            GTEST_SKIP() << Ran.err;
          ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode;
#endif
          continue;
        }
        const auto Program =
            tmpFile(std::string("continuation") + Optimization +
                    neverd::test::executableSuffix());
        const auto Built =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c11", Optimization, "-Werror=uninitialized",
                  "-Werror=return-type", "-fsanitize=undefined,address",
                  File.string(), "-o", Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
        const auto Ran = exec(Program.string(), {});
        ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode << '\n'
                              << Source;
      }
    }
  }
};

X64ContinuationSourceOptions controlsOptions(uint32_t Mask = 0xffbf) {
  auto O = options();
  O.Profile = X64ContinuationProfile::KernelControlsNoFaultV2;
  O.MXCSRMask = Mask;
  return O;
}

X64ContinuationSourceOptions smapOptions() {
  auto O = controlsOptions();
  O.Profile = X64ContinuationProfile::KernelSmapNoFaultV3;
  return O;
}

namespace {
X64ContinuationSourceOptions eventOptions() {
  auto O = controlsOptions();
  O.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
  O.PagingAddressBits = 48;
  return O;
}

const char EventHarness[] = R"C(
static void event_word(uint64_t address, uint64_t value) {
  memcpy((void *)(uintptr_t)address,&value,8);
}
static event_state_t event_seed(uint64_t memory) {
  event_state_t s={0};state_v5_t core=seed_v5(0x1000);
  memcpy(&s,&core,sizeof(core));
  s.gdt=(event_table_t){memory,63,0,0};
  s.idt=(event_table_t){memory+0x200,63,0,0};
  s.ldt=(event_segment_t){memory+0x100,63,40,0x82};
  s.tr=(event_segment_t){memory+0x300,103,48,0x8b};
  s.ds=s.es=s.ss;
  s.fs=(event_metadata_t){s.ds.limit,s.ds.selector,s.ds.attributes};s.gs=s.fs;
  s.fs_base=0x123456780000ULL;s.gs_base=0x123456790000ULL;
  s.nmi_blocked=1;s.gpr[4]=memory+0x800;
  memset((void *)(uintptr_t)memory,0xa5,4096);
  event_word(memory+8,0x00af9b000000ffffULL);
  event_word(memory+16,0x00cf93000000ffffULL);
  event_word(memory+24,0x00af9a000000ffffULL);
  event_word(memory+32,0x00cf92000000ffffULL);
  return s;
}
static void event_frame(event_state_t *s,unsigned cpl,unsigned nullss,unsigned local,uint64_t flags) {
  uint64_t table=local?s->ldt.base:s->gdt.base;
  /* LDT entry zero is a valid non-null selector (TI=1). */
  unsigned cs=local?4:24,ss=local?12:32;
  event_word(table+(cs&~7),0x00af9a000000ffffULL|((uint64_t)cpl<<45));
  event_word(table+(ss&~7),0x00cf92000000ffffULL|((uint64_t)cpl<<45));
  uint64_t frame[]={0x1800,cs|cpl,flags,0x0800000000000000ULL,nullss?cpl:(ss|cpl)};
  memcpy((void *)(uintptr_t)s->gpr[4],frame,sizeof(frame));
}
static void event_vector_gate(event_state_t *s,unsigned type,unsigned ist,unsigned vector) {
  uint64_t low=0x1800|(24ULL<<16)|((uint64_t)ist<<32)|((uint64_t)(0x80|type)<<40);
  event_word(s->idt.base+16*vector,low);event_word(s->idt.base+16*vector+8,0);
  s->gpr[4]=s->gdt.base+0xf27;
  if(ist)event_word(s->tr.base+28+8*ist,s->gdt.base+0xe27);
}
static void event_gate(event_state_t *s,unsigned type,unsigned ist) {
  event_vector_gate(s,type,ist,3);
}
)C";

void stepDispatcher(llvm::raw_ostream &OS, llvm::ArrayRef<va_t> Entries) {
  // LLVMC uses void* for an opaque pointer; HighC retains uint8_t*. A direct
  // C call permits either conversion without an incompatible function pointer.
  OS << "\nstatic uint64_t step(unsigned index, void *state) {\nswitch(index) "
        "{\n";
  for (unsigned I = 0; I != Entries.size(); ++I)
    OS << "case " << I << ": return neverd_step_v6_"
       << llvm::utohexstr(Entries[I]) << "(state);\n";
  OS << "default: abort();\n}}\n";
}
} // namespace

TEST_F(X64ContinuationSourceTest, IretMatchesNativeFramesFlagsAndSegments) {
  run({0x48, 0xcf}, {0x1000}, std::string(EventHarness) + R"C(
int main(void) {
  unsigned char before[4096],after[4096];
  for(unsigned local=0;local<2;++local) for(unsigned cpl=0;cpl<4;++cpl)
    for(unsigned nullss=0;nullss<2;++nullss) for(unsigned bit=0;bit<64;++bit) {
      if(cpl==3 && nullss)continue;
      event_state_t s=event_seed(0x8000);
      event_frame(&s,cpl,nullss,local,2|(1ULL<<bit));s.nmi_blocked=bit&1;
      event_state_t want=s;
      memcpy(before,(void *)0x8000,4096);
      __asm__ volatile("outb %%al,$0xe9"::"a"(2),"D"(&want):"memory");
      memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
      CHECK(neverd_step_v6_1000(ARG(&s))==6);
      CHECK(memcmp(&s,&want,sizeof(s))==0);
      CHECK(memcmp((void *)0x8000,after,4096)==0);
    }
  for(unsigned bank=0;bank<2;++bank)for(unsigned bit=0;bit<64;++bit) {
    uint64_t descriptor=(bank?0x00cf92000000ffffULL:0x00af9a000000ffffULL)^(1ULL<<bit);
    uint64_t access=descriptor>>40;
    if(bank?((access&0xfa)!=0x92):((access&0x60f8)!=0x2098))continue;
    event_state_t s=event_seed(0x8000);event_frame(&s,0,0,0,0x246);
    event_word(s.gdt.base+(bank?32:24),descriptor);event_state_t want=s;
    memcpy(before,(void *)0x8000,4096);
    __asm__ volatile("outb %%al,$0xe9"::"a"(2),"D"(&want):"memory");
    memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
    CHECK(neverd_step_v6_1000(ARG(&s))==6);
    CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp((void *)0x8000,after,4096));
  }
  const unsigned types[]={0x91,0x93,0x95,0x97,0x9b,0x9f};
  for(unsigned cpl=1;cpl<4;++cpl)for(unsigned dpl=0;dpl<4;++dpl)
    for(unsigned type=0;type<6;++type) {
      event_state_t s=event_seed(0x8000);event_frame(&s,cpl,0,0,0x246);
      s.ds.attributes=s.es.attributes=0xc000|types[type]|(dpl<<5);
      s.fs.attributes=s.gs.attributes=s.ds.attributes;
      event_state_t want=s;memcpy(before,(void *)0x8000,4096);
      __asm__ volatile("outb %%al,$0xe9"::"a"(2),"D"(&want):"memory");
      memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
      CHECK(neverd_step_v6_1000(ARG(&s))==6);
      CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp((void *)0x8000,after,4096));
    }
  for(unsigned which=0;which<3;++which) {
    event_state_t s=event_seed(0x8000);
    uint64_t frame[]={0x1800,8,2,which?0x00cf92000000ffffULL:0x00af9a000000ffffULL,which?16:0};
    if(which==1)frame[2]=0x00af9b000000ffffULL;
    s.gdt.base=s.gpr[4]+(which?8:16);s.gdt.limit=31;
    if(which==2) {
      frame[2]=0x209a;frame[3]=0x19123;frame[4]=0;
      s.gdt.base=s.gpr[4]+3;s.gdt.limit=15;
    }
    memcpy((void *)(uintptr_t)s.gpr[4],frame,40);event_state_t want=s;
    memcpy(before,(void *)0x8000,4096);
    __asm__ volatile("outb %%al,$0xe9"::"a"(2),"D"(&want):"memory");
    memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
    CHECK(neverd_step_v6_1000(ARG(&s))==6);
    CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp((void *)0x8000,after,4096));
  }
  return 0;
}
)C",
      false, eventOptions(), {}, true);
}

class X64FarReturnTest : public X64ContinuationSourceTest,
                         public ::testing::WithParamInterface<unsigned> {};

TEST_P(X64FarReturnTest, MatchesNativeFramesCleanupFlagsAndSegments) {
  const unsigned Encoding = GetParam();
  const unsigned Discard = Encoding == 65536 ? 0 : Encoding;
  const std::vector<uint8_t> Bytes =
      Encoding == 65536 ? std::vector<uint8_t>{0x48, 0xcb}
                        : std::vector<uint8_t>{0x48, 0xca, uint8_t(Discard),
                                               uint8_t(Discard >> 8)};
  std::string Harness = EventHarness;
  llvm::raw_string_ostream OS(Harness);
  OS << "\n#define DISCARD " << Discard << "\n#define QUERY "
     << (Encoding == 65536 ? 5 : 6) << "\n";
  OS << R"C(
int main(void) {
  unsigned char before[4096],after[4096],frame_before[65584],frame_after[65584];
  const uint64_t flags[]={2,3,6,18,66,130,514,1026,2050,16386,262146,2097154,0x244ed7};
  for(unsigned local=0;local<2;++local)for(unsigned cpl=0;cpl<4;++cpl)
    for(unsigned nullss=0;nullss<2;++nullss)for(unsigned f=0;f<13;++f) {
      if(cpl==3 && nullss)continue;
      event_state_t s=event_seed(0x8000);
      s.gpr[4]=0x210000+(f&7);s.flags=flags[f];s.nmi_blocked=f&1;
      memset((void *)0x210000,0xa5,sizeof(frame_before));
      unsigned cs=local?4:24,ss=local?12:32;
      uint64_t table=local?s.ldt.base:s.gdt.base;
      event_word(table+(cs&~7),0x00af9a000000ffffULL|((uint64_t)cpl<<45));
      event_word(table+(ss&~7),0x00cf92000000ffffULL|((uint64_t)cpl<<45));
      event_word(s.gpr[4],0x1800);
      event_word(s.gpr[4]+8,0x1234567800000000ULL|cs|cpl);
      uint64_t new_sp=f&1?0x0800000000000000ULL:UINT64_MAX-DISCARD+7;
      if(cpl) {
        event_word(s.gpr[4]+16+DISCARD,new_sp);
        event_word(s.gpr[4]+24+DISCARD,0x1234567800000000ULL|(nullss?cpl:ss|cpl));
      } else if(nullss) {s.ss.selector=0;s.unusable|=2;}
      event_state_t want=s;memcpy(before,(void *)0x8000,4096);
      memcpy(frame_before,(void *)0x210000,sizeof(frame_before));
      __asm__ volatile("outb %%al,$0xe9"::"a"(QUERY),"D"(&want),"S"(DISCARD):"memory");
      memcpy(after,(void *)0x8000,4096);memcpy(frame_after,(void *)0x210000,sizeof(frame_after));
      memcpy((void *)0x8000,before,4096);memcpy((void *)0x210000,frame_before,sizeof(frame_before));
      CHECK(neverd_step_v6_1000(ARG(&s))==6);
      CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp((void *)0x8000,after,4096));
      CHECK(!memcmp((void *)0x210000,frame_after,sizeof(frame_after)));
      CHECK(s.flags==flags[f] && s.nmi_blocked==(f&1));
    }
  return 0;
}
)C";
  OS.flush();
  run(Bytes, {0x1000}, Harness, false, eventOptions(), {}, true);
}

INSTANTIATE_TEST_SUITE_P(UnsignedCleanup, X64FarReturnTest,
                         ::testing::Values(65536, 0, 1, 7, 8, 255, 32768,
                                           65535));

TEST_F(X64ContinuationSourceTest,
       FarReturnMatchesNativeDescriptorsAndAliasing) {
  run({0x48, 0xcb}, {0x1000}, std::string(EventHarness) + R"C(
static void compare_return(event_state_t s) {
  unsigned char before[4096],after[4096];event_state_t want=s;
  memcpy(before,(void *)0x8000,4096);
  __asm__ volatile("outb %%al,$0xe9"::"a"(5),"D"(&want):"memory");
  memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
  CHECK(neverd_step_v6_1000(ARG(&s))==6);
  CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp((void *)0x8000,after,4096));
}
static event_state_t return_frame(unsigned cpl) {
  event_state_t s=event_seed(0x8000);
  event_word(s.gdt.base+24,0x00af9a000000ffffULL|((uint64_t)cpl<<45));
  event_word(s.gdt.base+32,0x00cf92000000ffffULL|((uint64_t)cpl<<45));
  event_word(s.gpr[4],0x1800);event_word(s.gpr[4]+8,24|cpl);
  event_word(s.gpr[4]+16,0x220000);event_word(s.gpr[4]+24,32|cpl);
  return s;
}
int main(void) {
  for(unsigned bank=0;bank<2;++bank)for(unsigned bit=0;bit<64;++bit) {
    uint64_t descriptor=(bank?0x00cff2000000ffffULL:0x00affa000000ffffULL)^(1ULL<<bit);
    uint64_t access=descriptor>>40;
    if(bank?((access&0xfa)!=0xf2):((access&0x60f8)!=0x20f8))continue;
    event_state_t s=return_frame(3);event_word(s.gdt.base+(bank?32:24),descriptor);
    compare_return(s);
  }
  const unsigned types[]={0x91,0x93,0x95,0x97,0x9b,0x9f};
  for(unsigned cpl=1;cpl<4;++cpl)for(unsigned dpl=0;dpl<4;++dpl)
    for(unsigned type=0;type<6;++type) {
      event_state_t s=return_frame(cpl);
      s.ds.attributes=s.es.attributes=0xc000|types[type]|(dpl<<5);
      s.fs.attributes=s.gs.attributes=s.ds.attributes;compare_return(s);
    }
  for(unsigned cpl=0;cpl<4;++cpl)for(unsigned dpl=0;dpl<=cpl;++dpl) {
    event_state_t s=return_frame(cpl);
    event_word(s.gdt.base+24,0x00af9e000000ffffULL|((uint64_t)dpl<<45));
    compare_return(s);
  }
  for(unsigned cpl=1;cpl<3;++cpl)for(unsigned skew=0;skew<2;++skew) {
    event_state_t s=return_frame(cpl);
    /* The CS accessed byte overlaps the restored RSP (also when unaligned). */
    uint64_t descriptor=s.gpr[4]+16-(skew?5:0);
    event_word(descriptor,0x00209a0000000000ULL|((uint64_t)cpl<<45));
    event_word(s.gpr[4]+8,8|cpl);event_word(s.gpr[4]+24,cpl);
    s.gdt.base=descriptor-8;s.gdt.limit=15;
    compare_return(s);
  }
  return 0;
}
)C",
      false, eventOptions(), {}, true);
}

TEST_F(X64ContinuationSourceTest, FarReturnSamePrivilegeDoesNotReadOuterFrame) {
#if !defined(__linux__)
  GTEST_SKIP() << "guard-page fixture requires Linux mmap";
#else
  run({0x48, 0xcb, 0x48, 0xca, 0xff, 0xff}, {0x1000, 0x1002},
      std::string(EventHarness) + R"C(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  long page=sysconf(_SC_PAGESIZE);CHECK(page>=4096);
  unsigned char *m=mmap(0,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(m!=MAP_FAILED && !mprotect(m+page,page,PROT_READ|PROT_WRITE));
  unsigned char memory[4096];
  for(unsigned imm=0;imm<2;++imm)for(unsigned unusable=0;unusable<2;++unusable) {
    event_state_t s=event_seed((uintptr_t)memory);s.pc=0x1000+2*imm;
    s.gpr[4]=(uintptr_t)(m+2*page-16);s.unusable|=unusable*2;
    event_word(s.gpr[4],0x1800);event_word(s.gpr[4]+8,24);
    /* The current SS cache is not revalidated or replaced at the same CPL. */
    s.ss.selector=unusable?0:16;s.ss.base=0x12340000;
    event_segment_t ss=s.ss;uint64_t sp=s.gpr[4],flags=s.flags;
    CHECK((imm?neverd_step_v6_1002(ARG(&s)):neverd_step_v6_1000(ARG(&s)))==6);
    CHECK(s.pc==0x1800 && s.gpr[4]==sp+16+(imm?65535:0));
    CHECK(!memcmp(&s.ss,&ss,sizeof(ss)) && (s.unusable&2)==unusable*2);
    CHECK(s.flags==flags && s.nmi_blocked==1);
  }
  CHECK(!munmap(m,3*page));return 0;
}
)C",
      false, eventOptions());
#endif
}

TEST_F(X64ContinuationSourceTest, FarReturnRejectsInvalidStateBeforeAnyWrite) {
  for (unsigned Bits : {48u, 57u}) {
    auto O = eventOptions();
    O.PagingAddressBits = Bits;
    run({0x48, 0xca, 7, 0}, {0x1000}, std::string(EventHarness) + R"C(
int main(void) {
  unsigned char memory[4096],before[4096];
  for(unsigned bad=0;bad<22;++bad) {
    event_state_t s=event_seed((uintptr_t)memory);
    event_word(s.gdt.base+24,0x00affa000000ffffULL);
    event_word(s.gdt.base+32,0x00cff2000000ffffULL);
    event_word(s.gpr[4],0x1800);event_word(s.gpr[4]+8,27);
    event_word(s.gpr[4]+23,0x220000);event_word(s.gpr[4]+31,35);
    if(bad==0)event_word(s.gpr[4],0x0100000000000000ULL);
    if(bad==1)event_word(s.gpr[4]+8,0);
    if(bad==2)s.gdt.limit=30;
    if(bad==3)event_word(s.gdt.base+24,0x00af7a000000ffffULL);
    if(bad==4)event_word(s.gdt.base+24,0x00cffa000000ffffULL);
    if(bad==5)event_word(s.gdt.base+24,0x00af9a000000ffffULL);
    if(bad==6)event_word(s.gpr[4]+31,3);
    if(bad==7)event_word(s.gpr[4]+31,32);
    if(bad==8)event_word(s.gdt.base+32,0x00cff0000000ffffULL);
    if(bad==9)event_word(s.gdt.base+32,0x00cf72000000ffffULL);
    if(bad==10)s.nmi_blocked=2;
    if(bad==11)s.unusable|=1;
    if(bad==12){event_word(s.gpr[4]+8,7);s.unusable|=64;}
    if(bad==13)s.gdt.base=UINT64_MAX-16;
    if(bad==14)s.cs.attributes|=0x4000;
    if(bad==15)s.cs.selector|=3;
    if(bad==16)s.pc++;
    if(bad==17)s.flags|=0x10000;
    if(bad==18)s.gpr[4]=UINT64_MAX-7;
    if(bad==19)s.gpr[4]=0x0100000000000000ULL;
    if(bad==20)s.gdt.limit=38;
    if(bad==21){event_word(s.gpr[4]+31,15);s.ldt.attributes=0;}
    event_state_t want=s;memcpy(before,memory,4096);
    CHECK(neverd_step_v6_1000(ARG(&s))==5);
    CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp(memory,before,4096));
  }
  return 0;
}
)C",
        false, O);
  }
}

TEST_F(X64ContinuationSourceTest, Int3MatchesNativeGatesStacksAndFlags) {
  run({0xcc}, {0x1000}, std::string(EventHarness) + R"C(
int main(void) {
  unsigned char before[4096],after[4096];
  for(unsigned type=14;type<=15;++type)for(unsigned ist=0;ist<8;++ist)
    for(unsigned nullss=0;nullss<2;++nullss)for(unsigned bit=0;bit<64;++bit) {
      event_state_t s=event_seed(0x8000);
      s.flags=2|((1ULL<<bit)&0x3d7fd7);s.nmi_blocked=bit&1;
      if(nullss){s.ss.selector=0;s.unusable|=2;}
      event_gate(&s,type,ist);event_state_t want=s;
      memcpy(before,(void *)0x8000,4096);
      __asm__ volatile("outb %%al,$0xe9"::"a"(3),"D"(&want):"memory");
      memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
      CHECK(neverd_step_v6_1000(ARG(&s))==7);
      CHECK(memcmp(&s,&want,sizeof(s))==0);
      CHECK(memcmp((void *)0x8000,after,4096)==0);
    }
  return 0;
}
)C",
      false, eventOptions(), {}, true);
}

class X64SoftwareInterruptTest
    : public X64ContinuationSourceTest,
      public ::testing::WithParamInterface<unsigned> {};

TEST_P(X64SoftwareInterruptTest, EveryVectorMatchesNativeStateAndMemory) {
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Harness = EventHarness;
  llvm::raw_string_ostream OS(Harness);
  for (unsigned I = 0; I != 16; ++I) {
    Entries.push_back(0x1000 + Bytes.size());
    Bytes.push_back(0xcd);
    Bytes.push_back(GetParam() * 16 + I);
  }
  stepDispatcher(OS, Entries);
  OS << "\n#define FIRST_VECTOR " << GetParam() * 16 << R"C(
int main(void) {
  unsigned char before[8192],after[8192];
  for(unsigned i=0;i<16;++i)for(unsigned bit=0;bit<64;++bit) {
    const unsigned vector=FIRST_VECTOR+i,type=14+((bit>>3)&1),ist=bit&7;
    event_state_t s=event_seed(0x8000);s.pc=0x1000+2*i;
    s.flags=2|((1ULL<<bit)&0x3d7fd7);s.nmi_blocked=(bit>>5)&1;
    if(bit&16){s.ss.selector=0;s.unusable|=2;}
    s.idt.base=0x9000;s.idt.limit=16*vector+15;
    memset((void *)0x9000,0xa5,4096);
    event_vector_gate(&s,type,ist,vector);
    /* CPL0 may enter every gate DPL. Seed every other vector as invalid. */
    ((unsigned char *)0x9000)[16*vector+5]|=((bit>>4)&3)<<5;
    event_state_t want=s;
    memcpy(before,(void *)0x8000,sizeof(before));
    __asm__ volatile("outb %%al,$0xe9"::"a"(4),"D"(&want),"S"(vector):"memory");
    memcpy(after,(void *)0x8000,sizeof(after));
    memcpy((void *)0x8000,before,sizeof(before));
    CHECK(step(i,ARG(&s))==7);
    CHECK(!memcmp(&s,&want,sizeof(s)));
    CHECK(!memcmp((void *)0x8000,after,sizeof(after)));
    /* Software interrupts never push a CPU exception's error code. */
    CHECK(s.gpr[4]==(((ist?0x8e27:0x8f27)&~15ULL)-40));
    CHECK(*(uint64_t *)(uintptr_t)s.gpr[4]==0x1002+2*i);
    CHECK(s.nmi_blocked==((bit>>5)&1));
  }
  return 0;
}
)C";
  OS.flush();
  run(std::move(Bytes), std::move(Entries), Harness, false, eventOptions(), {},
      true);
}

INSTANTIATE_TEST_SUITE_P(AllVectors, X64SoftwareInterruptTest,
                         ::testing::Range(0u, 16u));

TEST_F(X64ContinuationSourceTest, InterruptVectorBoundsRejectBeforeAnyEffect) {
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Harness = EventHarness;
  llvm::raw_string_ostream OS(Harness);
  const unsigned Vectors[] = {0, 1, 2, 3, 0x29, 0x2c, 0x2d, 0x80, 0xff};
  for (unsigned Vector : Vectors) {
    Entries.push_back(0x1000 + Bytes.size());
    Bytes.push_back(0xcd);
    Bytes.push_back(Vector);
  }
  stepDispatcher(OS, Entries);
  OS << R"C(
int main(void) {
  unsigned char memory[8192],before[8192];
  const unsigned vectors[]={0,1,2,3,0x29,0x2c,0x2d,0x80,0xff};
  for(unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);++i)
    for(unsigned bad=0;bad<4;++bad) {
      const unsigned vector=vectors[i];
      event_state_t s=event_seed((uintptr_t)memory);s.pc=0x1000+2*i;
      s.idt.base=(uintptr_t)(memory+4096);s.idt.limit=16*vector+15;
      memset(memory+4096,0xa5,4096);event_vector_gate(&s,14,0,vector);
      if(bad==0)--s.idt.limit;
      if(bad==1)s.idt.base=UINT64_MAX-16*vector;
      if(bad==2)s.idt.base=0x0100000000000000ULL;
      if(bad==3)memory[4096+16*vector+5]&=0x7f;
      event_state_t want=s;memcpy(before,memory,sizeof(memory));
      CHECK(step(i,ARG(&s))==5);
      CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp(before,memory,sizeof(memory)));
    }
  return 0;
}
)C";
  OS.flush();
  run(std::move(Bytes), std::move(Entries), Harness, false, eventOptions());
}

TEST_F(X64ContinuationSourceTest,
       Int3DescriptorWritesPrecedeIstReadsAndFrameStores) {
  run({0x90, 0xcc}, {0x1001}, std::string(EventHarness) + R"C(
int main(void) {
  unsigned char before[4096],after[4096],stack_before[512],stack_after[512];
  for(unsigned ist=0;ist<2;++ist) {
    event_state_t s=event_seed(0x8000);s.pc=0x1001;event_gate(&s,14,ist);
    uint64_t descriptor=ist?s.tr.base+32:((s.gpr[4]&~15ULL)-40)-5;
    event_word(descriptor,ist?0x00209a0000000000ULL:0x00af9a000000ffffULL);
    s.gdt.base=descriptor-24;s.gdt.limit=31;
    memset((void *)0x209900,0xa5,512);
    event_state_t want=s;memcpy(before,(void *)0x8000,4096);
    memcpy(stack_before,(void *)0x209900,512);
    __asm__ volatile("outb %%al,$0xe9"::"a"(3),"D"(&want):"memory");
    memcpy(after,(void *)0x8000,4096);memcpy(stack_after,(void *)0x209900,512);
    memcpy((void *)0x8000,before,4096);memcpy((void *)0x209900,stack_before,512);
    CHECK(neverd_step_v6_1001(ARG(&s))==7);
    CHECK(!memcmp(&s,&want,sizeof(s)) && !memcmp((void *)0x8000,after,4096));
    CHECK(!memcmp((void *)0x209900,stack_after,512));
  }
  return 0;
}
)C",
      false, eventOptions(), {}, true);
}

TEST_F(X64ContinuationSourceTest, EventsLeaveEarlierStateLayoutsUnsupported) {
  auto V4 = controlsOptions();
  V4.Profile = X64ContinuationProfile::KernelFaultAddressNoFaultV4;
  auto V5 = controlsOptions();
  V5.Profile = X64ContinuationProfile::KernelReturnNoFaultV5;
  V5.PagingAddressBits = 48;
  for (auto O : {options(), controlsOptions(), smapOptions(), V4, V5}) {
    auto Units = buildX64ContinuationSource(
        image({0x48, 0xcf, 0xcc, 0xcd, 0, 0xcd, 3, 0xcd, 0x2d, 0xcd, 0xff, 0x48,
               0xcb, 0x48, 0xca, 0xff, 0xff}),
        {0x1000, 0x1002, 0x1003, 0x1005, 0x1007, 0x1009, 0x100b, 0x100d}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    for (const auto &U : *Units)
      EXPECT_TRUE(U.RequiresNativeExecution);
  }
}

TEST_F(X64ContinuationSourceTest, UnqualifiedFarReturnEncodingsRemainNative) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xcb},
                                         {0xca, 0, 0},
                                         {0x66, 0xcb},
                                         {0x66, 0x48, 0xcb},
                                         {0x49, 0xcb},
                                         {0x67, 0x48, 0xca, 0xff, 0xff}}) {
    auto Units =
        buildX64ContinuationSource(image(Bytes), {0x1000}, eventOptions());
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    ASSERT_EQ(Units->size(), 1u);
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
    EXPECT_EQ(Units->front().Bytes, Bytes);
  }
}

TEST_F(X64ContinuationSourceTest, UnqualifiedInterruptEncodingsRemainNative) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x66, 0xcd, 3}, {0x40, 0xcd, 0}, {0xf3, 0xcd, 0x80}, {0xf1}}) {
    auto Units =
        buildX64ContinuationSource(image(Bytes), {0x1000}, eventOptions());
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    ASSERT_EQ(Units->size(), 1u);
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
    EXPECT_EQ(Units->front().Bytes, Bytes);
  }
}

TEST_F(X64ContinuationSourceTest, AccessedDescriptorsMayRemainReadOnly) {
#if !defined(__linux__)
  GTEST_SKIP() << "read-only mapping fixture requires Linux mmap";
#else
  run({0x48, 0xcf, 0xcc, 0x48, 0xcb}, {0x1000, 0x1002, 0x1003},
      std::string(EventHarness) + R"C(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  long page=sysconf(_SC_PAGESIZE);CHECK(page>=4096);
  void *table=mmap(0,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(table!=MAP_FAILED);
  event_word((uintptr_t)table+24,0x00af9b000000ffffULL);
  event_word((uintptr_t)table+32,0x00cf93000000ffffULL);
  CHECK(mprotect(table,page,PROT_READ)==0);
  unsigned char memory[4096];
  event_state_t s=event_seed((uintptr_t)memory);event_frame(&s,0,0,0,0x246);
  s.gdt.base=(uintptr_t)table;
  CHECK(neverd_step_v6_1000(ARG(&s))==6);
  CHECK(s.pc==0x1800 && s.gpr[4]==0x0800000000000000ULL);
  s=event_seed((uintptr_t)memory);event_frame(&s,0,1,0,0x246);s.gdt.base=(uintptr_t)table;
  CHECK(neverd_step_v6_1000(ARG(&s))==6 && (s.unusable&2));
  s=event_seed((uintptr_t)memory);s.pc=0x1002;event_gate(&s,14,0);
  s.gdt.base=(uintptr_t)table;
  CHECK(neverd_step_v6_1002(ARG(&s))==7 && s.pc==0x1800);
  uint64_t saved;memcpy(&saved,(void *)(uintptr_t)s.gpr[4],8);CHECK(saved==0x1003);
  s=event_seed((uintptr_t)memory);s.pc=0x1003;s.gdt.base=(uintptr_t)table;
  uint64_t sp=s.gpr[4];event_word(sp,0x1800);event_word(sp+8,24);
  CHECK(neverd_step_v6_1003(ARG(&s))==6 && s.gpr[4]==sp+16);
  CHECK(munmap(table,page)==0);
  return 0;
}
)C",
      false, eventOptions());
#endif
}

TEST_F(X64ContinuationSourceTest, EventRejectionLeavesStateAndRamUntouched) {
  for (unsigned Bits : {48u, 57u}) {
    auto O = eventOptions();
    O.PagingAddressBits = Bits;
    run({0x48, 0xcf, 0xcc, 0x48, 0x0f, 0x07}, {0x1000, 0x1002, 0x1003},
        std::string(EventHarness) + R"C(
int main(void) {
  unsigned char memory[4096],before[4096];
  for(unsigned bad=0;bad<16;++bad) {
    event_state_t s=event_seed((uintptr_t)memory);
    event_frame(&s,3,0,0,0x246);
    uint64_t frame[5];memcpy(frame,(void *)(uintptr_t)s.gpr[4],40);
    if(bad==0)s.flags|=0x4000;
    if(bad==1)frame[0]=0x0100000000000000ULL;
    if(bad==2)frame[1]=0;
    if(bad==3)s.gdt.limit=20;
    if(bad==4)event_word(s.gdt.base+24,0x00af7a000000ffffULL);
    if(bad==5)event_word(s.gdt.base+24,0x00cfba000000ffffULL);
    if(bad==6)frame[4]=3;
    if(bad==7)frame[4]=32;
    if(bad==8)event_word(s.gdt.base+32,0x00cf90000000ffffULL);
    if(bad==9)s.nmi_blocked=2;
    if(bad==10)s.unusable|=1;
    if(bad==11){frame[1]=7;s.unusable|=64;}
    if(bad==12)s.gdt.base=UINT64_MAX-16;
    if(bad==13)event_word(s.gdt.base+24,0x00af9a000000ffffULL);
    if(bad==14)s.cs.attributes|=0x4000;
    if(bad==15)s.pc++;
    memcpy((void *)(uintptr_t)s.gpr[4],frame,40);
    event_state_t want=s;memcpy(before,memory,4096);
    CHECK(neverd_step_v6_1000(ARG(&s))==5);
    CHECK(memcmp(&s,&want,sizeof(s))==0 && !memcmp(memory,before,4096));
  }
  for(unsigned bad=0;bad<13;++bad) {
    event_state_t s=event_seed((uintptr_t)memory);s.pc=0x1002;
    event_gate(&s,14,1);
    if(bad==0)s.idt.limit=62;
    if(bad==1)s.idt.base=UINT64_MAX-32;
    if(bad==2)memory[0x235]=0x80|13;
    if(bad==3)memory[0x235]=14;
    if(bad==4)memory[0x234]|=8;
    if(bad==5)memory[0x23c]=1;
    if(bad==6)memory[0x232]=0;
    if(bad==7)event_word(s.gdt.base+24,0x00affa000000ffffULL);
    if(bad==8)s.unusable|=128;
    if(bad==9)s.tr.limit=40;
    if(bad==10)s.tr.attributes=0x89;
    if(bad==11)event_word(s.tr.base+36,0x0100000000000000ULL);
    if(bad==12)event_word(s.tr.base+36,32);
    event_state_t want=s;memcpy(before,memory,4096);
    CHECK(neverd_step_v6_1002(ARG(&s))==5);
    CHECK(memcmp(&s,&want,sizeof(s))==0 && !memcmp(memory,before,4096));
  }
  event_state_t s=event_seed((uintptr_t)memory);s.pc=0x1003;s.unusable|=2;
  s.gpr[1]=0x0100000000000000ULL;event_state_t want=s;
  CHECK(neverd_step_v6_1003(ARG(&s))==5 && !memcmp(&s,&want,sizeof(s)));
  s.gpr[1]=0x1800;s.gpr[11]=0x246;
  CHECK(neverd_step_v6_1003(ARG(&s))==6 && !(s.unusable&3));
  return 0;
}
)C",
        false, O);
  }
}

TEST_F(X64ContinuationSourceTest, SysretProducesCompleteUserContext) {
  auto O = smapOptions();
  O.Profile = X64ContinuationProfile::KernelReturnNoFaultV5;
  for (unsigned Bits : {48u, 57u}) {
    O.PagingAddressBits = Bits;
    const std::string Harness =
        "#define ADDRESS_BITS " + std::to_string(Bits) + "\n" + R"C(
int main(void) {
  const uint16_t selectors[]={0,1,3,4,0x23,0xffe8,0xfff0,0xfff8,0xffff};
  const uint64_t pcs[]={0,0x7fffffffffffULL,0xffff800000000000ULL,
                        UINT64_MAX,0x800000000000ULL,0x00ffffffffffffffULL,
                        0xff00000000000000ULL,0x0100000000000000ULL};
  for(unsigned i=0;i<sizeof(selectors)/sizeof(selectors[0]);++i)
    for(unsigned bit=0;bit<65;++bit) for(unsigned pc=0;pc<8;++pc) {
      state_v5_t s=seed_v5(0x1000), want;
      s.core.core.core.gpr[1]=pcs[pc];
      s.core.core.core.gpr[11]=bit==64?UINT64_MAX:1ULL<<bit;
      s.core.core.core.flags|=0x40000;
      s.star=((uint64_t)selectors[i]<<48)|0xabcde0123456ULL;
      s.core.cr2=0x123456789abcdef0ULL;s.reserved=0x1234;
      want=s;
      uint64_t top=pcs[pc]>>(ADDRESS_BITS-1);
      int valid=top==0 || top==(UINT64_MAX>>(ADDRESS_BITS-1));
      if(valid) {
        want.core.core.core.pc=pcs[pc];
        want.core.core.core.flags=(s.core.core.core.gpr[11]&0x3c7fd7)|2;
        want.cs=(segment_t){0,0xffffffff,(uint16_t)(selectors[i]+16)|3,0xa0fb};
        want.ss=(segment_t){0,0xffffffff,(uint16_t)(selectors[i]+8)|3,0xc0f3};
      }
      CHECK(neverd_step_v5_1000(ARG(&s))==(valid?6:5));
      CHECK(memcmp(&s,&want,sizeof(s))==0);
    }
  /* A returned user context may not reenter a supervisor step. */
  for(unsigned invalid=0;invalid<4;++invalid) {
    state_v5_t s=seed_v5(0x1000),want;
    if(invalid==0)s.cs.selector|=3;
    if(invalid==1)s.cs.attributes&=~0x2000;
    if(invalid==2)s.cs.attributes|=0x4000;
    if(invalid==3)s.cs.attributes&=~0x80;
    want=s;
    CHECK(neverd_step_v5_1000(ARG(&s))==5);
    CHECK(memcmp(&s,&want,sizeof(s))==0);
  }
  return 0;
}
)C";
    run({0x48, 0x0f, 0x07}, {0x1000}, Harness, false, O);
  }
}

TEST_F(X64ContinuationSourceTest, SysretKeepsEarlierLayoutsUnsupported) {
  auto V4 = smapOptions();
  V4.Profile = X64ContinuationProfile::KernelFaultAddressNoFaultV4;
  for (auto O : {options(), controlsOptions(), smapOptions(), V4}) {
    auto Units =
        buildX64ContinuationSource(image({0x48, 0x0f, 0x07}), {0x1000}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
}

TEST_F(X64ContinuationSourceTest, SysretMatchesNativePrivilegeTransition) {
  auto O = smapOptions();
  O.Profile = X64ContinuationProfile::KernelReturnNoFaultV5;
  O.PagingAddressBits = 48;
  run({0x48, 0x0f, 0x07}, {0x1000}, R"C(
typedef struct {
  uint64_t gpr[16], flags,pc,star;
  segment_t cs,ss;
} return_query_t;
int main(void) {
  const uint16_t selectors[]={0,1,3,4,0x23,0xffe8,0xfff0,0xfff8,0xffff};
  for(unsigned i=0;i<sizeof(selectors)/sizeof(selectors[0]);++i)
    for(unsigned bit=0;bit<65;++bit) {
      state_v5_t s=seed_v5(0x1000),want;
      s.star=(uint64_t)selectors[i]<<48;
      s.core.core.core.gpr[1]=0x1800;
      s.core.core.core.gpr[11]=bit==64?UINT64_MAX:1ULL<<bit;
      return_query_t q={0};
      memcpy(q.gpr,s.core.core.core.gpr,sizeof(q.gpr));
      q.flags=s.core.core.core.flags;q.star=s.star;
      __asm__ volatile("outb %%al,$0xe9"::"a"(1),"D"(&q):"memory");
      want=s;memcpy(want.core.core.core.gpr,q.gpr,sizeof(q.gpr));
      want.core.core.core.flags=q.flags;want.core.core.core.pc=q.pc;
      want.cs=q.cs;want.ss=q.ss;
      CHECK(neverd_step_v5_1000(ARG(&s))==6);
      CHECK(memcmp(&s,&want,sizeof(s))==0);
    }
  return 0;
}
)C",
      false, O, {}, true);
}

TEST_F(X64ContinuationSourceTest, SegmentBaseMsrsShareLiveGsState) {
  for (unsigned AddressBits : {48u, 57u}) {
    auto O = controlsOptions();
    O.SegmentBaseMSRAddressBits = AddressBits;
    const std::string Harness =
        "#define ADDRESS_BITS " + std::to_string(AddressBits) + R"C(
int main(void) {
  const uint64_t values[]={0,1,0x7fffffffffffULL,0xffff800000000000ULL,
                           UINT64_MAX,0x00ffffffffffffffULL};
  const unsigned bits[]={0,2,4,6,7,11};
  for (unsigned bank=0;bank<3;++bank)
    for (unsigned mask=0;mask<64;++mask)
      for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        state_v2_t s=seed_v2(0x1000), want;
        s.core.fs=0x12345000; s.core.gs=0xffff800012345678ULL;
        s.kernel_gs=0xabcdef;
        s.core.flags=0x202;
        for (unsigned b=0;b<6;++b) s.core.flags|=((mask>>b)&1ULL)<<bits[b];
        s.core.gpr[1]=0x12345678c0000100ULL+bank;
        uint64_t v=bank==0?s.core.fs:bank==1?s.core.gs:s.kernel_gs;
        want=s; want.core.gpr[0]=(uint32_t)v;
        want.core.gpr[2]=(uint32_t)(v>>32); want.core.pc=0x1002;
        CHECK(neverd_step_v2_1000(ARG(&s))==0);
        CHECK(memcmp(&s,&want,sizeof(s))==0);
        v=values[i]; s.core.gpr[0]=0xabcdef0100000000ULL|(uint32_t)v;
        s.core.gpr[2]=0x1234567800000000ULL|(uint32_t)(v>>32);
        want=s;
        const uint64_t top=v>>(ADDRESS_BITS-1);
        const int valid=top==0 || top==(UINT64_MAX>>(ADDRESS_BITS-1));
        if (valid) {
          if (bank==0) want.core.fs=v;
          if (bank==1) want.core.gs=v;
          if (bank==2) want.kernel_gs=v;
          want.core.pc=0x1004;
        }
        CHECK(neverd_step_v2_1002(ARG(&s))==(valid?0:5));
        CHECK(memcmp(&s,&want,sizeof(s))==0);
      }
  state_v2_t s=seed_v2(0x1002), want;
  s.core.gpr[1]=0xc0000101; s.core.gpr[0]=0x12345678;
  s.core.gpr[2]=0xffff8000; s.kernel_gs=0x76543210;
  CHECK(neverd_step_v2_1002(ARG(&s))==0);
  want=s; want.core.pc=0x1007;
  want.core.gs=s.kernel_gs; want.kernel_gs=s.core.gs;
  CHECK(neverd_step_v2_1004(ARG(&s))==0);
  CHECK(memcmp(&s,&want,sizeof(s))==0);
  s.core.pc=0x1000; s.core.gpr[1]=0xc0000102;
  CHECK(neverd_step_v2_1000(ARG(&s))==0);
  CHECK(s.core.gpr[0]==0x12345678 && s.core.gpr[2]==0xffff8000);
  const uint32_t refused[]={0,0x48,0x49,0xc0000082,0xc0000103,0xffffffff};
  for (unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i)
    for (unsigned write=0;write<2;++write) {
      s=seed_v2(write?0x1002:0x1000);s.core.gpr[1]=refused[i];want=s;
      CHECK((write?neverd_step_v2_1002(ARG(&s)):
                   neverd_step_v2_1000(ARG(&s)))==5);
      CHECK(memcmp(&s,&want,sizeof(s))==0);
    }
  return 0;
}
)C";
    run({0x0f, 0x32, 0x0f, 0x30, 0x0f, 0x01, 0xf8}, {0x1000, 0x1002, 0x1004},
        Harness, false, O);
  }
}

TEST_F(X64ContinuationSourceTest, NativeMsrOptInKeepsSegmentWritesInState) {
  auto O = controlsOptions();
  O.SegmentBaseMSRAddressBits = 48;
  O.NativeSpeculationControlMsrs = true;
  run({0x0f, 0x30}, {0x1000}, R"C(
int main(void) {
  for (unsigned bank=0;bank<3;++bank) {
    state_v2_t s=seed_v2(0x1000), want;
    s.core.gpr[1]=0xc0000100+bank;
    s.core.gpr[0]=0x12345678;s.core.gpr[2]=0xffff8000;
    want=s;want.core.pc=0x1002;
    if(bank==0)want.core.fs=0xffff800012345678ULL;
    if(bank==1)want.core.gs=0xffff800012345678ULL;
    if(bank==2)want.kernel_gs=0xffff800012345678ULL;
    CHECK(neverd_step_v2_1000(ARG(&s))==0);
    CHECK(memcmp(&s,&want,sizeof(s))==0);
  }
  state_v2_t s=seed_v2(0x1000), want;
  s.core.gpr[1]=0xc0000080;want=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==5);
  CHECK(memcmp(&s,&want,sizeof(s))==0);
  return 0;
}
)C",
      false, O, "wrmsr");
}

TEST_F(X64ContinuationSourceTest, NativeSpeculationMsrsExecuteOnSupervisorCpu) {
  auto O = controlsOptions();
  O.SegmentBaseMSRAddressBits = 48;
  O.NativeSpeculationControlMsrs = true;
  run({0x0f, 0x30}, {0x1000}, R"C(
static uint64_t native_read(uint32_t index) {
  uint32_t low,high;
  __asm__ volatile("rdmsr":"=a"(low),"=d"(high):"c"(index):"memory");
  return ((uint64_t)high<<32)|low;
}
static void native_write(uint32_t index,uint64_t value) {
  __asm__ volatile("wrmsr"::"c"(index),"a"((uint32_t)value),
                   "d"((uint32_t)(value>>32)):"memory");
}
int main(void) {
  uint32_t a=7,b,c=0,d;
  __asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(c),"=d"(d));
  if(!(d&(1u<<26)))return 77;
  const uint64_t original=native_read(0x48);
  const uint64_t fs=native_read(0xc0000100),gs=native_read(0xc0000101);
  const uint64_t kgs=native_read(0xc0000102);
  for(unsigned command=0;command<2;++command)
    for(unsigned value=0;value<2;++value)
      for(unsigned repeat=0;repeat<2;++repeat) {
        if(!repeat)native_write(0x48,0);
        state_v2_t s=seed_v2(0x1000),want;
        s.core.gpr[1]=0x1234567800000000ULL|(command?0x49:0x48);
        s.core.gpr[0]=0xabcdef0100000000ULL|value;
        s.core.gpr[2]=0x1234567800000000ULL;
        want=s;want.core.pc=0x1002;
        CHECK(neverd_step_v2_1000(ARG(&s))==0);
        CHECK(memcmp(&s,&want,sizeof(s))==0);
        CHECK(native_read(0x48)==(command?0:value));
      }
  native_write(0x48,original);
  CHECK(native_read(0xc0000100)==fs && native_read(0xc0000101)==gs);
  CHECK(native_read(0xc0000102)==kgs);
  return 0;
}
)C",
      false, O, "wrmsr", true);
}

TEST_F(X64ContinuationSourceTest, DescriptorEnvironmentRequiresOptIn) {
  run({0x0f, 0x03, 0xc0, 0x0f, 0x00, 0x2b, 0x0f, 0x01, 0x03},
      {0x1000, 0x1003, 0x1006}, R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000),before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  s.core.pc=0x1003;before=s;
  CHECK(neverd_step_v2_1003(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  s.core.pc=0x1006;before=s;
  CHECK(neverd_step_v2_1006(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
}
)C",
      true, controlsOptions());
}

TEST_F(X64ContinuationSourceTest, NativeDescriptorQueriesUseLiveTables) {
  auto O = controlsOptions();
  O.NativeDescriptorQueries = true;
  run({0x0f, 0x03, 0xc0, 0x0f, 0x00, 0x2b, 0x0f, 0x01, 0x03, 0x65, 0x0f, 0x00,
       0x2b},
      {0x1000, 0x1003, 0x1006, 0x1009}, R"C(
int main(void) {
  const uint16_t selectors[]={0,8,9,16,19,27,35,40,48,63,64,0xffff};
  const uint64_t limits[]={0,0xffffffff,0,0xffffffff,0,0xffffffff,
                           0xffffffff,0x1234,0x67,0,0,0};
  const unsigned writable[]={0,0,0,1,0,0,1,1,0,0,0,0};
  for(unsigned i=0;i<sizeof(selectors)/sizeof(selectors[0]);++i) {
    state_v2_t s=seed_v2(0x1000), want;
    s.core.gpr[0]=0xabcddcba00000000ULL|selectors[i];want=s;
    want.core.pc=0x1003;
    if(limits[i]) {want.core.gpr[0]=limits[i];want.core.flags|=64;}
    else want.core.flags&=~64ULL;
    CHECK(neverd_step_v2_1000(ARG(&s))==0);
    CHECK(memcmp(&s,&want,sizeof(s))==0);
    for(unsigned segment=0;segment<2;++segment) {
      uint16_t selector=selectors[i];
      s=seed_v2(segment?0x1009:0x1003);
      s.core.gs=segment?(uintptr_t)&selector-7:0;
      s.core.gpr[3]=segment?7:(uintptr_t)&selector;want=s;
      want.core.pc=segment?0x100d:0x1006;
      want.core.flags=(want.core.flags&~64ULL)|(writable[i]?64:0);
      CHECK((segment?neverd_step_v2_1009(ARG(&s)):
                      neverd_step_v2_1003(ARG(&s)))==0);
      CHECK(memcmp(&s,&want,sizeof(s))==0);
      CHECK(selector==selectors[i]);
    }
  }
  unsigned char bytes[32],expected[32];memset(bytes,0xa5,sizeof(bytes));
  memcpy(expected,bytes,sizeof(bytes));
  const unsigned char gdtr[]={63,0,0,0x50,0,0,0,0,0,0};
  memcpy(expected+3,gdtr,10);
  state_v2_t s=seed_v2(0x1006),want;
  s.core.gpr[3]=(uintptr_t)(bytes+3);want=s;want.core.pc=0x1009;
  CHECK(neverd_step_v2_1006(ARG(&s))==0);
  CHECK(memcmp(&s,&want,sizeof(s))==0);
  CHECK(memcmp(bytes,expected,sizeof(bytes))==0);
  return 0;
}
)C",
      false, O, {}, true);
}

TEST_F(X64ContinuationSourceTest, NativePagingRequiresExplicitEnvironment) {
  run({0x0f, 0x20, 0xd8, 0x0f, 0x22, 0xdc}, {0x1000, 0x1003}, R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000), before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  s.core.pc=0x1003; before=s;
  CHECK(neverd_step_v2_1003(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
}
)C",
      true, controlsOptions());
}

TEST_F(X64ContinuationSourceTest,
       NativePagingChangesTranslationsAndFlushesTlb) {
  auto O = controlsOptions();
  O.NativePageTableAccess = true;
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Calls, PCs;
  for (unsigned Register = 0; Register != 16; ++Register)
    for (bool Write : {false, true}) {
      const va_t PC = 0x1000 + Bytes.size();
      Entries.push_back(PC);
      if (!Calls.empty()) {
        Calls += ",";
        PCs += ",";
      }
      Calls += "neverd_step_v2_" + llvm::utohexstr(PC);
      PCs += "0x" + llvm::utohexstr(PC);
      if (Register >= 8)
        Bytes.push_back(0x41);
      Bytes.insert(Bytes.end(), {0x0f, uint8_t(Write ? 0x22 : 0x20),
                                 uint8_t(0xd8 | (Register & 7))});
    }
  run(Bytes, Entries,
      "static __typeof__(&neverd_step_v2_1000) steps[]={" + Calls +
          "};\nstatic const uint64_t pcs[]={" + PCs + "};\n" + R"C(
static uint64_t root(void) {
  uint64_t r; __asm__ volatile("mov %%cr3,%0":"=r"(r)::"memory");return r;
}
static void table(uintptr_t base,uintptr_t page) {
  memset((void *)base,0,0x4000);
  *(uint64_t *)base=(base+0x1000)|3;
  *(uint64_t *)(base+0x1000)=(base+0x2000)|3;
  *(uint64_t *)(base+0x2000)=0x83;
  *(uint64_t *)(base+0x2008)=(base+0x3000)|3;
  *(uint64_t *)(base+0x3000)=page|3;
}
int main(void) {
  uint64_t original=root();
  table(0x6000,0xa000);table(0xc000,0xb000);
  *(volatile uint64_t *)0xa000=0x123456789abcdef0ULL;
  *(volatile uint64_t *)0xb000=0xabcdef9876543210ULL;
  const unsigned bits[]={11,7,6,4,2,0};
  for(unsigned reg=0;reg<16;++reg) for(unsigned which=0;which<2;++which)
    for(unsigned flags=0;flags<64;++flags) {
      unsigned n=reg*2+1;
      uint64_t next=which?0xc000:0x6000;
      state_v2_t s=seed_v2(pcs[n]),want;
      s.core.gpr[reg]=next;
      for(unsigned i=0;i<6;++i)s.core.arbitrary[i]=(flags>>i)&1;
      want=s;want.core.pc+=n>=16?4:3;
      for(unsigned i=0;i<6;++i)
        want.core.flags=(want.core.flags&~(1ULL<<bits[i]))|
                        (((uint64_t)(flags>>i)&1)<<bits[i]);
      CHECK(steps[n](ARG(&s))==0 && memcmp(&s,&want,sizeof(s))==0);
      CHECK(root()==next);
      CHECK(*(volatile uint64_t *)0x200000==*(volatile uint64_t *)(which?0xb000:0xa000));
      /* Same CR3 value must still invalidate a cached non-global translation. */
      *(volatile uint64_t *)(next+0x3000)=(which?0xa000:0xb000)|3;
      s.core.pc=pcs[n];want=s;want.core.pc+=n>=16?4:3;
      CHECK(steps[n](ARG(&s))==0 && memcmp(&s,&want,sizeof(s))==0);
      CHECK(*(volatile uint64_t *)0x200000==*(volatile uint64_t *)(which?0xa000:0xb000));
      *(volatile uint64_t *)(next+0x3000)=(which?0xb000:0xa000)|3;
      --n;s.core.pc=pcs[n];want=s;want.core.gpr[reg]=next;
      want.core.pc+=n>=16?4:3;
      CHECK(steps[n](ARG(&s))==0 && memcmp(&s,&want,sizeof(s))==0);
    }
  __asm__ volatile("mov %0,%%cr3"::"r"(original):"memory");
  return 0;
}
)C",
      false, O, {}, true);
}

TEST_F(X64ContinuationSourceTest, FaultAddressReadWriteKeepsEveryOtherBank) {
  auto O = smapOptions();
  O.Profile = X64ContinuationProfile::KernelFaultAddressNoFaultV4;
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Calls, PCs;
  for (unsigned Register = 0; Register != 16; ++Register)
    for (bool Write : {false, true}) {
      const va_t PC = 0x1000 + Bytes.size();
      Entries.push_back(PC);
      if (!Calls.empty()) {
        Calls += ",";
        PCs += ",";
      }
      Calls += "neverd_step_v4_" + llvm::utohexstr(PC);
      PCs += "0x" + llvm::utohexstr(PC);
      if (Register >= 8)
        Bytes.push_back(0x41);
      Bytes.insert(Bytes.end(), {0x0f, uint8_t(Write ? 0x22 : 0x20),
                                 uint8_t(0xd0 | (Register & 7))});
    }
  run(Bytes, Entries,
      "static __typeof__(&neverd_step_v4_1000) steps[]={" + Calls +
          "};\nstatic const uint64_t pcs[]={" + PCs + "};\n" + R"C(
int main(void) {
  const uint64_t values[]={0,1,UINT64_MAX,0xffff800012345678ULL,
                          0x0123456789abcdefULL};
  const unsigned bits[]={11,7,6,4,2,0};
  for(unsigned n=0;n<32;++n) for(unsigned v=0;v<5;++v)
  for(unsigned flags=0;flags<64;++flags) {
    state_v4_t s={0}; s.core=seed_v2(pcs[n]);
    s.cr2=values[v]; s.reserved=0x1234567812345678ULL;
    s.core.core.flags=0x40202;
    s.core.core.gpr[n/2]=values[4-v];
    for(unsigned i=0;i<6;++i) s.core.core.arbitrary[i]=(flags>>i)&1;
    state_v4_t expected=s;
    if(n&1) expected.cr2=s.core.core.gpr[n/2];
    else expected.core.core.gpr[n/2]=s.cr2;
    expected.core.core.pc+=n>=16?4:3;
    for(unsigned i=0;i<6;++i)
      expected.core.core.flags|=((uint64_t)((flags>>i)&1))<<bits[i];
    CHECK(steps[n](ARG(&s))==0 && memcmp(&s,&expected,sizeof(s))==0);
    /* Wrong-PC invocations are atomic refusals, even for writes. */
    s.core.core.pc=0; expected=s;
    CHECK(steps[n](ARG(&s))==5 && memcmp(&s,&expected,sizeof(s))==0);
  }
  return 0;
})C",
      false, O);
}

TEST_F(X64ContinuationSourceTest,
       EarlierProfilesNeverAcquireFaultAddressState) {
  for (auto O : {options(), controlsOptions(), smapOptions()}) {
    auto Units = buildX64ContinuationSource(
        image({0x0f, 0x20, 0xd0, 0x0f, 0x22, 0xd0}), {0x1000, 0x1003}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    for (const auto &Unit : *Units)
      EXPECT_TRUE(Unit.RequiresNativeExecution);
  }
}

TEST_F(X64ContinuationSourceTest, NativeTimestampsKeepAllResultsAndOtherState) {
#if !defined(__x86_64__) && !defined(_M_X64)
  GTEST_SKIP() << "native timestamp execution requires an x64 host";
#else
  auto O = smapOptions();
  O.NativeTimestampReads = true;
  run({0x0f, 0x31, 0x0f, 0x01, 0xf9}, {0x1000, 0x1002}, R"C(
#include <cpuid.h>
#if defined(__linux__)
#include <sched.h>
#endif
static uint64_t clock_sample(unsigned *aux) {
  unsigned lo,hi;
  __asm__ volatile("rdtscp; lfence" : "=a"(lo), "=d"(hi), "=c"(*aux) : : "memory");
  return ((uint64_t)hi<<32)|lo;
}
int main(void) {
  unsigned a,b,c,d;
  CHECK(__get_cpuid(0x80000001,&a,&b,&c,&d) && (d&(1u<<27)));
#if defined(__linux__)
  cpu_set_t available, selected;
  CHECK(sched_getaffinity(0,sizeof(available),&available)==0);
  CPU_ZERO(&selected);
  for(unsigned cpu=0;cpu<CPU_SETSIZE;++cpu)
    if(CPU_ISSET(cpu,&available)) {CPU_SET(cpu,&selected); break;}
  CHECK(sched_setaffinity(0,sizeof(selected),&selected)==0);
#endif
  for(unsigned kind=0;kind<2;++kind) for(unsigned n=0;n<128;++n) {
    state_v2_t s=seed_v2(kind?0x1002:0x1000), expected=s;
    unsigned before_aux,after_aux;
    uint64_t before=clock_sample(&before_aux);
    CHECK((kind?neverd_step_v3_1002(ARG(&s)):neverd_step_v3_1000(ARG(&s)))==0);
    uint64_t after=clock_sample(&after_aux);
    CHECK(!(s.core.gpr[0]>>32) && !(s.core.gpr[2]>>32));
    uint64_t value=(s.core.gpr[2]<<32)|s.core.gpr[0];
    CHECK(value>=before && value<=after);
    expected.core.gpr[0]=s.core.gpr[0]; expected.core.gpr[2]=s.core.gpr[2];
    expected.core.pc=kind?0x1005:0x1002;
    if(kind) {
      CHECK(!(s.core.gpr[1]>>32));
      CHECK(before_aux==after_aux && s.core.gpr[1]==before_aux);
      expected.core.gpr[1]=before_aux;
    }
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
  }
  return 0;
})C",
      false, O);
#endif
}

TEST_F(X64ContinuationSourceTest, TimestampEnvironmentRequiresExplicitOptIn) {
  for (auto O : {options(), controlsOptions(), smapOptions()}) {
    auto Units = buildX64ContinuationSource(
        image({0x0f, 0x31, 0x0f, 0x01, 0xf9}), {0x1000, 0x1002}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    for (const auto &Unit : *Units)
      EXPECT_TRUE(Unit.RequiresNativeExecution);
  }
  auto O = options();
  O.NativeTimestampReads = true;
  auto Units = buildX64ContinuationSource(image({0x0f, 0x31}), {0x1000}, O);
  ASSERT_FALSE(bool(Units));
  llvm::consumeError(Units.takeError());
}

TEST_F(X64ContinuationSourceTest, WritePrefetchKeepsNativeHintAndLiveSegments) {
  auto O = smapOptions();
  O.NativeWritePrefetch = true;
  run({0x0f, 0x0d, 0x8f, 0x90, 0,    0,    0,    0x64, 0x0f, 0x0d, 0x8f,
       0x90, 0,    0,    0,    0x65, 0x0f, 0x0d, 0x8f, 0x90, 0,    0,
       0,    0x65, 0x67, 0x0f, 0x0d, 0x8f, 0x90, 0,    0,    0},
      {0x1000, 0x1007, 0x100f, 0x1017}, R"C(
int main(void) {
  unsigned char memory[512], expected_memory[512];
  memset(memory,0x5a,sizeof(memory)); memcpy(expected_memory,memory,sizeof(memory));
  const uint64_t pcs[]={0x1000,0x1007,0x100f,0x1017};
  __typeof__(&neverd_step_v3_1000) steps[]={neverd_step_v3_1000,
      neverd_step_v3_1007,neverd_step_v3_100F,neverd_step_v3_1017};
  for(unsigned n=0;n<4;++n) {
    state_v2_t s=seed_v2(pcs[n]);
    s.core.gpr[7]=n?0x100:(uintptr_t)memory;
    s.core.fs=s.core.gs=(uintptr_t)memory-0x100;
    if(n==3) s.core.gpr[7]|=0x1234567800000000ULL;
    state_v2_t expected=s; expected.core.pc+=n==0?7:n==3?9:8;
    CHECK(steps[n](ARG(&s))==0 && memcmp(&s,&expected,sizeof(s))==0);
    CHECK(memcmp(memory,expected_memory,sizeof(memory))==0);
  }
  return 0;
})C",
      false, O, "__asm__ volatile(\"prefetchw");
  auto Refused = buildX64ContinuationSource(image({0x0f, 0x0d, 0x0f}), {0x1000},
                                            smapOptions());
  ASSERT_TRUE(bool(Refused)) << llvm::toString(Refused.takeError());
  EXPECT_TRUE(Refused->front().RequiresNativeExecution);
}

TEST_F(X64ContinuationSourceTest, LockedUnaryPreservesCarryAndUnalignedMemory) {
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Calls, PCs;
  for (unsigned Width : {1u, 2u, 4u, 8u})
    for (bool Decrement : {false, true}) {
      const va_t PC = 0x1000 + Bytes.size();
      Entries.push_back(PC);
      if (!Calls.empty()) {
        Calls += ",";
        PCs += ",";
      }
      Calls += "neverd_step_v3_" + llvm::utohexstr(PC);
      PCs += "0x" + llvm::utohexstr(PC);
      Bytes.push_back(0xf0);
      if (Width == 2)
        Bytes.push_back(0x66);
      if (Width == 8)
        Bytes.push_back(0x48);
      Bytes.push_back(Width == 1 ? 0xfe : 0xff);
      Bytes.push_back(Decrement ? 0x0f : 0x07); // [rdi]
    }
  const auto Harness = "static __typeof__(&neverd_step_v3_1000) steps[]={" +
                       Calls + "};\n" + "static const uint64_t pcs[]={" + PCs +
                       "};\n" +
                       R"C(
int main(void) {
  const unsigned widths[]={1,1,2,2,4,4,8,8};
  const uint64_t samples[]={0,1,15,16,127,128,255,256,
                           0x7fffffff,0x80000000,UINT64_MAX,
                           0x7fffffffffffffffULL,0x8000000000000000ULL};
  for (unsigned n=0;n<8;++n) for(unsigned carry=0;carry<2;++carry)
  for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i)
  for(unsigned offset=8;offset<16;++offset) {
    _Alignas(64) unsigned char memory[64], wanted[64];
    memset(memory,0x5a,sizeof(memory));
    unsigned width=widths[n], bits=width*8;
    uint64_t mask=UINT64_MAX>>(64-bits), old=samples[i]&mask;
    memcpy(memory+offset,&old,width);
    memcpy(wanted,memory,sizeof(memory));
    uint64_t value=(n&1 ? old-1 : old+1)&mask;
    memcpy(wanted+offset,&value,width);
    state_v2_t s=seed_v2(pcs[n]), expected;
    s.core.flags=0x40202|carry;
    s.core.gpr[7]=(uintptr_t)(memory+offset);
    expected=s;
    unsigned parity=0;
    for(unsigned b=0;b<8;++b) parity^=(value>>b)&1;
    uint64_t overflow=(n&1) ? old==(UINT64_C(1)<<(bits-1))
                           : old==((UINT64_C(1)<<(bits-1))-1);
    expected.core.flags=(s.core.flags&~UINT64_C(0x8d4)) |
        ((uint64_t)!parity<<2) | (((old^1^value)&16)!=0 ? 16 : 0) |
        ((uint64_t)(value==0)<<6) | (((value>>(bits-1))&1)<<7) |
        (overflow<<11);
    expected.core.pc+=width==2||width==8 ? 4 : 3;
    CHECK(steps[n](ARG(&s))==0);
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(memcmp(memory,wanted,sizeof(memory))==0);
  }
  return 0;
}
)C";
  run(Bytes, Entries, Harness, false, smapOptions(), "lock xadd");
}

TEST_F(X64ContinuationSourceTest, AccessCheckIsGuestStateAcrossFlagSnapshots) {
  run({0x0f, 0x01, 0xcb, 0x9c, 0x58, 0xfa, 0xfb, 0x0f, 0x01, 0xca, 0x90},
      {0x1000, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x100a}, R"C(
int main(void) {
  const unsigned bits[]={0,2,4,6,7,9,10,11,14,18,21};
  for(unsigned pattern=0;pattern<2048;++pattern) {
    uint64_t stack[2]={0x1234,0x5678};
    state_v2_t s=seed_v2(0x1000);
    s.core.flags=2;
    for(unsigned i=0;i<11;++i)s.core.flags|=((pattern>>i)&1ULL)<<bits[i];
    s.core.gpr[4]=(uintptr_t)&stack[1];
    state_v2_t expected=s;
    CHECK(neverd_step_v3_1000(ARG(&s))==0);
    expected.core.flags|=1ULL<<18; expected.core.pc=0x1003;
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(neverd_step_v3_1003(ARG(&s))==0);
    expected.core.gpr[4]-=8; expected.core.pc=0x1004;
    CHECK(stack[0]==expected.core.flags && stack[1]==0x5678);
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(neverd_step_v3_1004(ARG(&s))==0);
    expected.core.gpr[0]=expected.core.flags;
    expected.core.gpr[4]+=8; expected.core.pc=0x1005;
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(neverd_step_v3_1005(ARG(&s))==0);
    expected.core.flags&=~(1ULL<<9); expected.core.pc=0x1006;
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(neverd_step_v3_1006(ARG(&s))==0);
    expected.core.flags|=1ULL<<9; expected.core.pc=0x1007;
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(neverd_step_v3_1007(ARG(&s))==0);
    expected.core.flags&=~(1ULL<<18); expected.core.pc=0x100a;
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
    CHECK(neverd_step_v3_100A(ARG(&s))==0);
    expected.core.pc=0x100b;
    CHECK(memcmp(&s,&expected,sizeof(s))==0);
  }
  return 0;
})C",
      false, smapOptions());
}

TEST_F(X64ContinuationSourceTest, AccessCheckDoesNotExtendPreviousDomains) {
  for (bool V2 : {false, true}) {
    std::string Harness = R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000), before=s;
  CHECK(STEP1000(ARG(&s))==4 && memcmp(&s,&before,sizeof(s))==0);
  s.core.pc=0x1003; before=s;
  CHECK(STEP1003(ARG(&s))==4 && memcmp(&s,&before,sizeof(s))==0);
  s.core.pc=0x1000; s.core.flags|=1ULL<<18; before=s;
  CHECK(STEP1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C";
    const std::string Prefix = V2 ? "neverd_step_v2_" : "neverd_step_";
    for (const char *PC : {"1000", "1003"}) {
      const std::string From = std::string("STEP") + PC;
      for (size_t At = Harness.find(From); At != std::string::npos;
           At = Harness.find(From))
        Harness.replace(At, From.size(), Prefix + PC);
    }
    run({0x0f, 0x01, 0xcb, 0x0f, 0x01, 0xca}, {0x1000, 0x1003}, Harness, true,
        V2 ? controlsOptions() : options());
  }
}

TEST_F(X64ContinuationSourceTest, SmapEntryStillRejectsEveryOtherExcludedFlag) {
  run({0x90}, {0x1000}, R"C(
int main(void) {
  const uint64_t allowed=0xcd5ULL|(1ULL<<9)|(1ULL<<14)|(1ULL<<18)|(1ULL<<21)|2;
  for(unsigned bit=0;bit<64;++bit) {
    if(allowed&(1ULL<<bit))continue;
    state_v2_t s=seed_v2(0x1000); s.core.flags|=(1ULL<<18)|(1ULL<<bit);
    state_v2_t before=s;
    CHECK(neverd_step_v3_1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  }
  state_v2_t s=seed_v2(0x1000); s.core.flags&=~2ULL;
  state_v2_t before=s;
  CHECK(neverd_step_v3_1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  s=seed_v2(0x1000); s.core.flags|=1ULL<<18; s.cr8=16; before=s;
  CHECK(neverd_step_v3_1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  s=seed_v2(0x1000); s.core.flags|=1ULL<<18; s.mxcsr=0x10000; before=s;
  CHECK(neverd_step_v3_1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      false, smapOptions());
}

TEST_F(X64ContinuationSourceTest, SmapNativeBoundaryRetainsAccessCheckState) {
  run({0x0f, 0x32}, {0x1000}, R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000);
  s.core.flags|=1ULL<<18;
  s.kernel_gs=0x1122334455667788ULL;
  s.reserved=0x89abcdef; s.reserved_tail=0xfedcba9876543210ULL;
  state_v2_t before=s;
  CHECK(neverd_step_v3_1000(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      true, smapOptions());
}

TEST_F(X64ContinuationSourceTest, SmapStateRetainsLegacyGSAndMemoryControls) {
  run({0x0f, 0x01, 0xf8, 0x65, 0x0f, 0xae, 0x17, 0x0f, 0xae, 0xe8},
      {0x1000, 0x1003, 0x1007}, R"C(
int main(void) {
  uint32_t mxcsr=0x3f80;
  state_v2_t s=seed_v2(0x1000);
  s.core.flags|=1ULL<<18;
  s.core.gs=0x123456; s.kernel_gs=(uintptr_t)&mxcsr; s.core.gpr[7]=0;
  state_v2_t expected=s;
  CHECK(neverd_step_v3_1000(ARG(&s))==0);
  expected.core.gs=(uintptr_t)&mxcsr; expected.kernel_gs=0x123456;
  expected.core.pc=0x1003;
  CHECK(memcmp(&s,&expected,sizeof(s))==0);
  CHECK(neverd_step_v3_1003(ARG(&s))==0);
  expected.mxcsr=mxcsr; expected.core.pc=0x1007;
  CHECK(memcmp(&s,&expected,sizeof(s))==0);
  CHECK(neverd_step_v3_1007(ARG(&s))==0);
  expected.core.pc=0x100a;
  CHECK(memcmp(&s,&expected,sizeof(s))==0);
  return 0;
})C",
      false, smapOptions());
}

TEST_F(X64ContinuationSourceTest,
       UnsignedWideDivisionKeepsAliasedInputsFlagsAndRefusalState) {
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  // LLVM opaque pointers print as void*, while HighC retains uint8_t*.
  // Direct calls accept either spelling without incompatible function casts.
  std::string Harness = "static uint64_t step(unsigned reg,uint8_t *s) {\n"
                        "switch(reg) {\n";
  for (unsigned Register = 0; Register != 16; ++Register) {
    Entries.push_back(0x1000 + Bytes.size());
    Harness += "case " + std::to_string(Register) + ": return neverd_step_v2_" +
               llvm::utohexstr(Entries.back()) + "(s);\n";
    Bytes.insert(Bytes.end(), {uint8_t(Register < 8 ? 0x48 : 0x49), 0xf7,
                               uint8_t(0xf0 | (Register & 7))});
  }
  Harness += R"C(default: abort();
}}
/* Bit-at-a-time restoring division, independent of host C division or the
   lifted quotient/remainder expressions. Inputs below satisfy high<divisor. */
static uint64_t quotient(uint64_t low,uint64_t high,uint64_t divisor,uint64_t *rem) {
  uint64_t q=0;
  for(unsigned bit=64;bit--;) {
    const unsigned carry=high>>63;
    high=(high<<1)|((low>>bit)&1);
    if(carry || high>=divisor) { high-=divisor; q|=1ULL<<bit; }
  }
  *rem=high; return q;
}
int main(void) {
  const unsigned flagbits[]={11,7,6,4,2,0};
  const uint64_t divisors[]={0,1,2,3,0x8000000000000000ULL,UINT64_MAX};
  const uint64_t lows[]={0,1,0xfedcba9876543210ULL,UINT64_MAX};
  for(unsigned reg=0;reg<16;++reg)
    for(unsigned d=0;d<sizeof(divisors)/sizeof(*divisors);++d)
      for(unsigned l=0;l<sizeof(lows)/sizeof(*lows);++l)
        for(unsigned edge=0;edge<3;++edge)
          for(unsigned pattern=0;pattern<64;++pattern) {
            state_v2_t s=seed_v2(0x1000+3*reg);
            s.core.gpr[0]=lows[l];
            s.core.gpr[2]=edge==0 ? 0 : divisors[d]-(edge==1);
            s.core.gpr[reg]=divisors[d];
            for(unsigned i=0;i<6;++i)s.core.arbitrary[i]=(pattern>>i)&1;
            state_v2_t expected=s;
            const uint64_t low=s.core.gpr[0],high=s.core.gpr[2],divisor=s.core.gpr[reg];
            const int bad=divisor==0 || high>=divisor;
            const uint64_t status=step(reg,ARG(&s));
            CHECK(status==(bad ? 5 : 0));
            if(!bad) {
              expected.core.gpr[0]=quotient(low,high,divisor,&expected.core.gpr[2]);
              expected.core.pc+=3;
              expected.core.flags&=~0x8d5ULL;
              for(unsigned i=0;i<6;++i)expected.core.flags|=expected.core.arbitrary[i]<<flagbits[i];
            }
            CHECK(memcmp(&s,&expected,sizeof(s))==0);
          }
  return 0;
})C";
  run(std::move(Bytes), std::move(Entries), Harness, false, controlsOptions());
}

TEST_F(X64ContinuationSourceTest, WideDivisionDoesNotExtendTheV1Profile) {
  run({0x48, 0xf7, 0xf1}, {0x1000}, R"C(
int main(void) {
  state_t s=seed(0x1000), before=s;
  CHECK(neverd_step_1000(ARG(&s))==4 && memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      true);
}

TEST_F(X64ContinuationSourceTest, TaskPriorityRetainsInvalidWrites) {
  run({0x44, 0x0f, 0x20, 0xc0, 0x44, 0x0f, 0x22, 0xc1}, {0x1000, 0x1004}, R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000), before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==0 && s.core.gpr[0]==3);
  CHECK(s.core.flags==(before.core.flags & ~0x8d5ULL));
  /* Each successful MOV CR8 consumes six independent fresh flag bits. */
  const unsigned bits[]={11,7,6,4,2,0};
  for(unsigned pattern=0;pattern<64;++pattern) {
    s.core.pc=0x1000; s.cr8=pattern%16;
    uint64_t flags=s.core.flags & ~0x8d5ULL;
    for(unsigned i=0;i<6;++i) {
      s.core.arbitrary[i]=(pattern>>i)&1;
      flags|=s.core.arbitrary[i]<<bits[i];
    }
    CHECK(neverd_step_v2_1000(ARG(&s))==0);
    CHECK(s.core.gpr[0]==s.cr8 && s.core.flags==flags);
  }
  s.core.gpr[1]=11;
  CHECK(neverd_step_v2_1004(ARG(&s))==0 && s.cr8==11);
  s.core.pc=0x1004; s.core.gpr[1]=16; before=s;
  CHECK(neverd_step_v2_1004(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  s.core.pc=0x1000; s.cr8=16; before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      false, controlsOptions());
}

TEST_F(X64ContinuationSourceTest, MXCSRUsesGuestBytesAndRejectsReservedBits) {
  run({0x0f, 0xae, 0x17, 0x0f, 0xae, 0x1e, 0x65, 0x0f, 0xae, 0x14, 0x25, 0x80,
       1, 0, 0},
      {0x1000, 0x1003, 0x1006}, R"C(
int main(void) {
  uint32_t input=0x3f80, output=0xdeadbeef;
  state_v2_t s=seed_v2(0x1000);
  s.core.gpr[7]=(uintptr_t)&input; s.core.gpr[6]=(uintptr_t)&output;
  CHECK(neverd_step_v2_1000(ARG(&s))==0 && s.mxcsr==input);
  CHECK(neverd_step_v2_1003(ARG(&s))==0 && output==input);
  input=0x1f80; s.core.gs=(uintptr_t)&input-0x180;
  CHECK(neverd_step_v2_1006(ARG(&s))==0 && s.mxcsr==input);
  for(unsigned i=0;i<2;++i) {
    input=i ? 0x10000 : 0x40;
    s.core.pc=0x1000; state_v2_t before=s;
    CHECK(neverd_step_v2_1000(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
    CHECK(output==0x3f80);
  }
  s.mxcsr=0x10000; s.core.pc=0x1003; state_v2_t before=s;
  CHECK(neverd_step_v2_1003(ARG(&s))==5 && memcmp(&s,&before,sizeof(s))==0);
  CHECK(output==0x3f80);
  return 0;
})C",
      false, controlsOptions());
}

TEST_F(X64ContinuationSourceTest, MXCSRDAZRequiresItsDeclaredWritableMask) {
  run({0x0f, 0xae, 0x17}, {0x1000}, R"C(
int main(void) {
  uint32_t input=0x1fc0;
  state_v2_t s=seed_v2(0x1000), before=s;
  s.core.gpr[7]=(uintptr_t)&input; before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==0 && s.mxcsr==input);
  before.mxcsr=input; before.core.pc=0x1003;
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      false, controlsOptions(0xffff));
}

TEST_F(X64ContinuationSourceTest, LoadFenceRetainsTheHostHardwareBarrier) {
  run({0x0f, 0xae, 0xe8}, {0x1000}, R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000), before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==0);
  before.core.pc=0x1003;
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      false, controlsOptions(), "lfence");
}

TEST_F(X64ContinuationSourceTest, LegacyGSExchangeChangesTheActualMemoryBase) {
  run({0x0f, 0x01, 0xf8, 0x65, 0x48, 0x8b, 0x07}, {0x1000, 0x1003}, R"C(
int main(void) {
  uint64_t old[2]={3,7}, replacement[2]={11,19};
  state_v2_t s=seed_v2(0x1000);
  s.core.gs=(uintptr_t)old; s.kernel_gs=(uintptr_t)replacement;
  s.core.gpr[7]=8; state_v2_t before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==0);
  CHECK(s.core.gs==(uintptr_t)replacement && s.kernel_gs==(uintptr_t)old);
  CHECK(neverd_step_v2_1003(ARG(&s))==0 && s.core.gpr[0]==19);
  s.core.pc=0x1000;
  CHECK(neverd_step_v2_1000(ARG(&s))==0);
  CHECK(s.core.gs==before.core.gs && s.kernel_gs==before.kernel_gs);
  CHECK(neverd_step_v2_1003(ARG(&s))==0 && s.core.gpr[0]==7);
  before.core.pc=s.core.pc; before.core.gpr[0]=7;
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      false, controlsOptions());
}

TEST_F(X64ContinuationSourceTest, WholeVectorXorPreservesUpperLanes) {
  run({0x66, 0x0f, 0xef, 0xc1}, {0x1000}, R"C(
int main(void) {
  state_t s=seed(0x1000), before=s;
  CHECK(neverd_step_1000(ARG(&s))==0);
  for(unsigned i=0;i<16;++i) before.zmm[0][i]^=before.zmm[1][i];
  before.pc=0x1004;
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest,
       MemoryBitStringsRetainSegmentsAndArbitraryFlags) {
  run({0x66, 0x65, 0x0f, 0xba, 0x34, 0x25, 0x60, 8,    0,    0,
       0x12, 0x0f, 0xba, 0x6b, 0x74, 8,    0x48, 0x0f, 0xa3, 0x0f},
      {0x1000, 0x100b, 0x1010}, R"C(
int main(void) {
  const unsigned bits[]={11,7,4,2};
  for(unsigned pattern=0;pattern<16;++pattern) {
    uint16_t word=0xa55f;
    state_t s=seed(0x1000); s.gs=(uintptr_t)&word-0x860;
    uint64_t flags=(s.flags & ~0x895ULL)|1;
    for(unsigned i=0;i<4;++i) {
      s.arbitrary[i]=(pattern>>i)&1; flags|=s.arbitrary[i]<<bits[i];
    }
    state_t before=s;
    CHECK(neverd_step_1000(ARG(&s))==0 && word==0xa55b);
    before.pc=0x100b; before.flags=flags;
    CHECK(memcmp(&s,&before,sizeof(s))==0);
  }
  uint32_t dword=0;
  state_t s=seed(0x100b); s.gpr[3]=(uintptr_t)&dword-0x74;
  CHECK(neverd_step_100B(ARG(&s))==0 && dword==0x100 && !(s.flags&1));
  uint64_t words[]={0x8000000000000000ULL,1,0x1234};
  const int64_t offsets[]={-1,0,64};
  for(unsigned i=0;i<3;++i) {
    s=seed(0x1010); s.gpr[7]=(uintptr_t)&words[1]; s.gpr[1]=offsets[i];
    CHECK(neverd_step_1010(ARG(&s))==0 && (s.flags&1)==(i<2));
    CHECK(words[0]==0x8000000000000000ULL && words[1]==1 && words[2]==0x1234);
  }
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, SelectorAndTailJumpRetainCompleteState) {
  run({0xb8, 0x78, 0, 0, 0, 0x41, 0xff, 0xe3}, {0x1000, 0x1005}, R"C(
int main(void) {
  state_t s=seed(0x1000), before=s;
  CHECK(neverd_step_1000(ARG(&s)) == 0);
  CHECK(s.gpr[0] == 0x78 && s.pc == 0x1005);
  CHECK(neverd_step_1005(ARG(&s)) == 2);
  CHECK(s.pc == before.gpr[11]);
  before.pc=s.pc; before.gpr[0]=0x78;
  CHECK(memcmp(&s,&before,sizeof(s)) == 0);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, MemoryCallLoadsTargetBeforeItsAliasingPush) {
  run({0xff, 0x54, 0x24, 0xf8, 0xc2, 0x10, 0}, {0x1000, 0x1004}, R"C(
int main(void) {
  uint64_t stack[8]={0};
  state_t s=seed(0x1000);
  s.gpr[4]=(uintptr_t)&stack[4]; stack[3]=0x99887766;
  CHECK(neverd_step_1000(ARG(&s)) == 1);
  CHECK(s.pc==0x99887766 && stack[3]==0x1004);
  CHECK(s.gpr[4]==(uintptr_t)&stack[3]);
  /* External code changes both the physical PC cell and a live register. */
  stack[3]=0x55443322; s.gpr[9]=0xdeadbeef; s.pc=0x1004;
  CHECK(neverd_step_1004(ARG(&s)) == 3);
  CHECK(s.pc==0x55443322 && s.gpr[4]==(uintptr_t)&stack[6]);
  CHECK(s.gpr[9]==0xdeadbeef);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, ChangedSavedStackCellControlsReentry) {
  run({0x48, 0x89, 0x54, 0x24, 0x10, 0xe8, 0x76, 0, 0, 0, 0x48, 0x8b, 0x64,
       0x24, 0x10, 0xc3},
      {0x1000, 0x1005, 0x100a, 0x100f}, R"C(
int main(void) {
  uint64_t old[8]={0}, alt[8]={0}, callback[8]={0};
  state_t s=seed(0x1000);
  s.gpr[4]=(uintptr_t)&alt[3]; s.gpr[2]=(uintptr_t)&old[3];
  CHECK(neverd_step_1000(ARG(&s))==0 && alt[5]==(uintptr_t)&old[3]);
  CHECK(neverd_step_1005(ARG(&s))==1 && alt[2]==0x100a);
  /* A callback may alias and overwrite the saved stack cell in the home area. */
  alt[5]=(uintptr_t)&callback[3]; callback[3]=0x778899;
  s.pc=alt[2]; s.gpr[4]+=8;
  CHECK(neverd_step_100A(ARG(&s))==0);
  CHECK(s.gpr[4]==(uintptr_t)&callback[3]);
  CHECK(neverd_step_100F(ARG(&s))==3);
  CHECK(s.pc==0x778899 && s.gpr[4]==(uintptr_t)&callback[4]);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest,
       WideCopyUsesPhysicalBytesAndPreservesUpperLanes) {
  run({0x66, 0x0f, 0x6f, 0x04, 0x24, 0x66, 0x0f, 0x7f, 0x01}, {0x1000, 0x1005},
      R"C(
int main(void) {
  unsigned char input[32] __attribute__((aligned(16)));
  unsigned char output[32] __attribute__((aligned(16)));
  for (unsigned i=0;i<32;++i) { input[i]=i*17+5; output[i]=0x9d; }
  state_t s=seed(0x1000), before=s;
  s.gpr[4]=(uintptr_t)input; s.gpr[1]=(uintptr_t)output;
  CHECK(neverd_step_1000(ARG(&s))==0);
  CHECK(memcmp(s.zmm[0],input,16)==0);
  CHECK(memcmp(s.zmm[0]+16,before.zmm[0]+16,48)==0);
  CHECK(neverd_step_1005(ARG(&s))==0);
  CHECK(memcmp(output,input,16)==0);
  for (unsigned i=16;i<32;++i) CHECK(output[i]==0x9d);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, SegmentBaseMayAliasThePhysicalStack) {
  run({0x65, 0x48, 0x8b, 0x07, 0x65, 0x48, 0x89, 0x07}, {0x1000, 0x1004}, R"C(
int main(void) {
  uint64_t stack[4]={7,0x1122334455667788ULL,9,10};
  state_t s=seed(0x1000);
  s.gs=(uintptr_t)stack; s.gpr[7]=8; s.gpr[4]=(uintptr_t)&stack[1];
  CHECK(neverd_step_1000(ARG(&s))==0 && s.gpr[0]==stack[1]);
  s.gpr[0]=0x99aabbccddeeff00ULL;
  CHECK(neverd_step_1004(ARG(&s))==0 && stack[1]==s.gpr[0]);
  CHECK(s.gpr[4]==(uintptr_t)&stack[1] && stack[0]==7 && stack[2]==9);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, FlagsUseGuestStateAndFreshArbitraryBits) {
  run({0x9c, 0xfa, 0xfb, 0x48, 0x85, 0xc0}, {0x1000, 0x1001, 0x1002, 0x1003},
      R"C(
int main(void) {
  uint64_t stack[4]={0};
  state_t s=seed(0x1000);
  s.gpr[4]=(uintptr_t)&stack[3];
  CHECK(neverd_step_1000(ARG(&s))==0 && stack[2]==0x246);
  CHECK(neverd_step_1001(ARG(&s))==0 && s.flags==0x46);
  CHECK(neverd_step_1002(ARG(&s))==0 && s.flags==0x246);
  s.gpr[0]=0; s.arbitrary[0]=1;
  CHECK(neverd_step_1003(ARG(&s))==0);
  CHECK((s.flags & 0xcd5)==0x54); /* ZF/PF defined; AF is caller's witness. */
  s.pc=0x1003; s.arbitrary[0]=0;
  CHECK(neverd_step_1003(ARG(&s))==0 && (s.flags&0x10)==0);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, ConditionalShiftWitnessesKeepZeroCountFlags) {
  run({0x48, 0xd3, 0xe0}, {0x1000}, R"C(
int main(void) {
  const unsigned counts[]={0,1,2,64};
  for(unsigned i=0;i<4;++i) {
    state_t s=seed(0x1000);
    s.gpr[0]=0x8000000000000001ULL; s.gpr[1]=counts[i];
    for(unsigned j=0;j<8;++j) s.arbitrary[j]=1;
    CHECK(neverd_step_1000(ARG(&s))==0 && s.pc==0x1003);
    if(counts[i]==0 || counts[i]==64) {
      CHECK(s.gpr[0]==0x8000000000000001ULL && s.flags==0x246);
    } else if(counts[i]==1) {
      CHECK(s.gpr[0]==2 && s.flags==0xa13);
    } else {
      CHECK(s.gpr[0]==4 && s.flags==0xa12);
    }
  }
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, PartialRegistersAndInvalidInput) {
  run({0xb4, 0xaa, 0x66, 0xb8, 0x55, 0x66, 0xb8, 0x78, 0x56, 0x34, 0x12},
      {0x1000, 0x1002, 0x1006}, R"C(
int main(void) {
  state_t s=seed(0x1000);
  CHECK(neverd_step_1000(ARG(&s))==0 && s.gpr[0]==0x123456780000aa00ULL);
  CHECK(neverd_step_1002(ARG(&s))==0 && s.gpr[0]==0x1234567800006655ULL);
  CHECK(neverd_step_1006(ARG(&s))==0 && s.gpr[0]==0x12345678);
  state_t before=s;
  CHECK(neverd_step_1000(ARG(&s))==5 && memcmp(&before,&s,sizeof(s))==0);
  s.pc=0x1000; s.flags|=0x100; before=s;
  CHECK(neverd_step_1000(ARG(&s))==5 && memcmp(&before,&s,sizeof(s))==0);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, NativeRequestDoesNotExecuteOrAdvance) {
  run({0x0f, 0x32}, {0x1000}, R"C(
int main(void) {
  state_t s=seed(0x1000), before=s;
  CHECK(neverd_step_1000(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      true);
}

TEST_F(X64ContinuationSourceTest, PrivilegeReturnDoesNotBecomePhysicalNearRet) {
  run({0x48, 0xcf}, {0x1000}, R"C(
int main(void) {
  state_t s=seed(0x1000), before=s;
  /* RSP intentionally points nowhere: IRETQ must not pop a near-RET cell. */
  CHECK(neverd_step_1000(ARG(&s))==4);
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C",
      true);
}

TEST_F(X64ContinuationSourceTest,
       NativeServiceStubKeepsItsFrameAndServiceNumber) {
  // Exact instruction sequence of the investigated ZwAlpcCancelMessage stub;
  // only its code coordinate is changed to this fixture's immutable mapping.
  run({0x48, 0x8b, 0xc4, 0xfa, 0x48, 0x83, 0xec, 0x10, 0x50, 0x9c,
       0x6a, 0x10, 0x48, 0x8d, 0x05, 0x2d, 0x7a, 0,    0,    0x50,
       0xb8, 0x78, 0,    0,    0,    0xe9, 0xf2, 0x65, 1,    0},
      {0x1000, 0x1003, 0x1004, 0x1008, 0x1009, 0x100a, 0x100c, 0x1013, 0x1014,
       0x1019},
      R"C(
int main(void) {
  uint64_t stack[16]={0};
  state_t s=seed(0x1000); s.gpr[4]=(uintptr_t)&stack[12];
  state_t before=s;
  CHECK(neverd_step_1000(ARG(&s))==0);
  CHECK(neverd_step_1003(ARG(&s))==0);
  CHECK(neverd_step_1004(ARG(&s))==0);
  CHECK(neverd_step_1008(ARG(&s))==0);
  CHECK(neverd_step_1009(ARG(&s))==0);
  CHECK(neverd_step_100A(ARG(&s))==0);
  CHECK(neverd_step_100C(ARG(&s))==0);
  CHECK(neverd_step_1013(ARG(&s))==0);
  CHECK(neverd_step_1014(ARG(&s))==0);
  CHECK(neverd_step_1019(ARG(&s))==2);
  CHECK(s.gpr[0]==0x78 && s.gpr[4]==(uintptr_t)&stack[6]);
  CHECK(s.pc==0x17610 && stack[6]==0x8a40 && stack[7]==0x10);
  CHECK(stack[9]==before.gpr[4]);
  uint64_t sub=before.gpr[4]-16;
  unsigned parity=0; for(unsigned i=0;i<8;++i) parity^=(sub>>i)&1;
  CHECK(stack[8]==(2|((uint64_t)!parity<<2)));
  before.gpr[0]=s.gpr[0]; before.gpr[4]=s.gpr[4];
  before.flags=s.flags; before.pc=s.pc;
  CHECK(memcmp(&s,&before,sizeof(s))==0);
  return 0;
})C");
}

TEST_F(X64ContinuationSourceTest, ConditionalWideReadRetainsEarlierCallBytes) {
  run({0xe8, 0x7b, 0,    0,    0,    0x48, 0x83, 0xec, 0x10, 0xe8, 0x72,
       0,    0,    0,    0xf7, 0x04, 0x24, 1,    0,    0,    0,    0x74,
       9,    0x66, 0x0f, 0x6f, 0x04, 0x24, 0x66, 0x0f, 0x7f, 0x01, 0xc3},
      {0x1000, 0x1005, 0x1009, 0x100e, 0x1015, 0x1017, 0x101c}, R"C(
int main(void) {
  for (unsigned flag=0;flag<2;++flag) for(unsigned full=0;full<2;++full) {
    unsigned char stack[64] __attribute__((aligned(16)));
    unsigned char out[16] __attribute__((aligned(16)));
    memset(stack,0xa5,sizeof(stack)); memset(out,0x7d,sizeof(out));
    state_t s=seed(0x1000); s.gpr[4]=(uintptr_t)(stack+32);
    CHECK(neverd_step_1000(ARG(&s))==1);
    uint64_t return_pc; memcpy(&return_pc,stack+24,8);
    CHECK(return_pc==0x1005);
    s.pc=return_pc; s.gpr[4]+=8;
    CHECK(neverd_step_1005(ARG(&s))==0 && s.gpr[4]==(uintptr_t)(stack+16));
    CHECK(neverd_step_1009(ARG(&s))==1);
    if(full) for(unsigned i=0;i<16;++i) stack[16+i]=i*9+17;
    uint32_t bits=flag; memcpy(stack+16,&bits,4);
    unsigned char expected[16]; memcpy(expected,stack+16,16);
    memcpy(&s.pc,stack+8,8); s.gpr[4]+=8; s.gpr[1]=(uintptr_t)out;
    CHECK(s.pc==0x100e && neverd_step_100E(ARG(&s))==0);
    CHECK(neverd_step_1015(ARG(&s))==2);
    if(flag) {
      CHECK(s.pc==0x1017 && neverd_step_1017(ARG(&s))==0);
      CHECK(neverd_step_101C(ARG(&s))==0 && memcmp(out,expected,16)==0);
      if(!full) { memcpy(&return_pc,out+8,8); CHECK(return_pc==0x1005); }
    } else {
      CHECK(s.pc==0x1020);
      for(unsigned i=0;i<16;++i) CHECK(out[i]==0x7d);
    }
  }
  return 0;
})C");
}

TEST(X64ContinuationSource, RequiresExplicitDomainAndBoundedDistinctAddresses) {
  auto Image = image({0x90, 0xc3});
  auto CheckError = [&](std::vector<va_t> Addresses,
                        X64ContinuationSourceOptions O) {
    auto R = buildX64ContinuationSource(Image, Addresses, O);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  };
  CheckError({0x1000}, {});
  CheckError({0x1000, 0x1000}, options());
  CheckError({0x1002}, options());
  auto V2 = controlsOptions();
  V2.MXCSRMask.reset();
  CheckError({0x1000}, V2);
  CheckError({0x1000}, controlsOptions(0x10000));
  auto MSRs = controlsOptions();
  MSRs.SegmentBaseMSRAddressBits = 49;
  CheckError({0x1000}, MSRs);
  MSRs = options();
  MSRs.SegmentBaseMSRAddressBits = 48;
  CheckError({0x1000}, MSRs);
  MSRs.SegmentBaseMSRAddressBits.reset();
  MSRs.NativeSpeculationControlMsrs = true;
  CheckError({0x1000}, MSRs);
  MSRs = options();
  MSRs.NativeDescriptorQueries = true;
  CheckError({0x1000}, MSRs);
  MSRs = options();
  MSRs.NativePageTableAccess = true;
  CheckError({0x1000}, MSRs);
  auto Returns = smapOptions();
  Returns.Profile = X64ContinuationProfile::KernelReturnNoFaultV5;
  CheckError({0x1000}, Returns);
  Returns.PagingAddressBits = 49;
  CheckError({0x1000}, Returns);
  Returns.Profile = X64ContinuationProfile::KernelSmapNoFaultV3;
  Returns.PagingAddressBits = 48;
  CheckError({0x1000}, Returns);
  auto V1 = options();
  V1.MXCSRMask = 0xffbf;
  CheckError({0x1000}, V1);
  auto O = options();
  O.MaxOperations = 1;
  CheckError({0x1000}, O);
  O = options();
  O.MaxInstructions = 1;
  CheckError({0x1000, 0x1001}, O);
}

} // namespace
