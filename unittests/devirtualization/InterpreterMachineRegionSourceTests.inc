//===- InterpreterMachineRegionSourceTests.inc - Physical exit source
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte/state oracles for checked region source lowering.
// Included inside the InterpreterMachineStateTests.cpp test namespace.

class InterpreterMachineRegionSourceTest : public InterpreterMachineSourceTest {
protected:
  static constexpr va_t Entry = 0x140001000;
  using ExitKind = SpecializationX64MachineExitKind;

  llvm::Expected<BinaryImage> loadRegion(llvm::ArrayRef<uint8_t> Code) {
    auto Bytes = neverd::web::test::nativePE();
    neverd::web::test::nativePut(Bytes, 0x98 + 240 + 8, 512, 4);
    std::fill(Bytes.begin() + 512, Bytes.end(), char(0xcc));
    if (Code.size() > 512)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "region fixture exceeds text section");
    std::copy(Code.begin(), Code.end(), Bytes.begin() + 512);
    const auto Path = tmpFile("machine-region.exe");
    std::ofstream(Path, std::ios::binary).write(Bytes.data(), Bytes.size());
    auto Reader = Loader::create(Path);
    if (!Reader)
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "region fixture has no loader");
    return Reader->load(Path);
  }

  static SpecializationOptions regionOptions(va_t Address, ExitKind Kind) {
    SpecializationOptions Options;
    Options.ExplicitMachineState = Options.NormalNonfaultingExecution =
        Options.X64CetDisabled = true;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    Options.MaxChainedTransfers = 32;
    Options.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
    Options.X64MachineExits = {{1, Address, Kind}};
    return Options;
  }
};

TEST_F(InterpreterMachineRegionSourceTest, TransfersPreservePhysicalState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  // Each expected state is computed from instruction bytes below, without
  // evaluating the recovered IR or using its receipt as a semantic oracle.
  const std::vector<std::vector<uint8_t>> Code = {
      {0xe8, 7, 0, 0, 0},                  // CALL original entry+12
      {0xff, 0xd0},                        // CALL RAX
      {0xff, 0x54, 0x24, 0xf8},            // CALL [RSP-8], target aliases push
      {0xff, 0xd4},                        // CALL RSP, before the decrement
      {0xff, 0xe0},                        // JMP RAX
      {0xc3},                              // RET
      {0x48, 0x89, 0x01, 0xc2, 24, 0},     // MOV [RCX],RAX; RET 24
      {0x48, 0x89, 0x01, 0xc2, 255, 255}}; // RET 65535
  for (unsigned Kind = 0; Kind != Code.size(); ++Kind) {
    SCOPED_TRACE(Kind);
    auto Image = loadRegion(Code[Kind]);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    const auto Options = regionOptions(Entry + (Kind >= 6 ? 3 : 0),
                                       Kind < 4    ? ExitKind::Call
                                       : Kind == 4 ? ExitKind::Jump
                                                   : ExitKind::Return);
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Wrapped = wrapInterpreterMachineRegionSourceX64(Result, Options);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    EXPECT_EQ(Wrapped->Environment.IdentityDigest,
              Result.RawEnvironment->IdentityDigest);
    Wrapped->Source.Function.Name = "generic_machine_region";
    emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16], flags, queries, count, cursor, xmm[16][2], pc, kind;
} state_t;
_Static_assert(sizeof(state_t)==432,"region layout");
static void put(unsigned char *p,uint64_t v) {
  for(unsigned i=0;i<8;++i) p[i]=(unsigned char)(v>>(8*i));
}
static uint64_t get(const unsigned char *p) {
  uint64_t v=0; for(unsigned i=0;i<8;++i) v|=(uint64_t)p[i]<<(8*i);
  return v;
}
int main(void) {
  const int offsets[]={-7,0,1,8,32};
  for(unsigned trial=0;trial<10;++trial) {
    _Alignas(16) unsigned char stack[512], expected_stack[512];
    struct {uint64_t before[2];state_t s;uint64_t after[2];} box, expected;
    memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0x11223344;box.after[1]=0x55667788;
    state_t *s=&box.s;
    for(unsigned i=0;i<512;++i) stack[i]=(unsigned char)(i*19+trial*7);
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9a76543210000000)+i*31+trial;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-trial-i;
    }
    s->flags=2|(trial&1?0xcd5:0x244);
    s->queries=0;s->count=0;s->cursor=0;
    s->pc=0xabcdef;s->kind=0x7654321;
    s->gpr[0]=0x345678;
    s->gpr[4]=(uintptr_t)(stack+264);
    s->gpr[1]=(uintptr_t)(stack+264+offsets[trial%5]);
    put(stack+256,0x234560);put(stack+264,0x12340);
    memcpy(expected_stack,stack,sizeof(stack));
    expected=box;
    if(KIND<4) {
      const unsigned lengths[]={5,2,4,2};
      expected.s.pc=KIND==0?UINT64_C(0x14000100c):
                    KIND==1?s->gpr[0]:KIND==2?get(stack+256):s->gpr[4];
      put(expected_stack+256,UINT64_C(0x140001000)+lengths[KIND]);
      expected.s.gpr[4]-=8;expected.s.kind=1;
    } else if(KIND==4) {
      expected.s.pc=s->gpr[0];expected.s.kind=2;
    } else {
      if(KIND>=6) put(expected_stack+264+offsets[trial%5],s->gpr[0]);
      expected.s.pc=get(expected_stack+264);expected.s.kind=3;
      expected.s.gpr[4]+=8+(KIND==6?24:KIND==7?65535:0);
    }
    if(generic_machine_region(MACHINE_ARG(s))) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
  }
  return 0;
}
)C",
               "#define KIND " + std::to_string(Kind) + "\n", nullptr, true,
               true);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       MemoryJumpExitsReadCurrentGuestSlots) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "fixed guest mappings require a 64-bit Linux host";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  for (bool Fresh : {false, true}) {
    for (unsigned Kind = 0; Kind != 3; ++Kind) {
      SCOPED_TRACE(Fresh);
      SCOPED_TRACE(Kind);
      const unsigned Slot = 256 + (Kind == 1);
      const unsigned Prefix = Kind == 2 ? 3 : 0;
      std::vector<uint8_t> Code;
      if (Prefix)
        Code = {0x48, 0x89, 0x01}; // MOV [RCX],RAX
      Code.insert(Code.end(),
                  {0xff, 0x25, static_cast<uint8_t>(Slot - Prefix - 6), 0, 0,
                   0}); // JMP [RIP+disp32]
      auto Image = loadRegion(Code);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Options = regionOptions(Entry + Prefix, ExitKind::Jump);
      if (Fresh)
        Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
      const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      auto Wrapped =
          Fresh ? wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options)
                : wrapInterpreterMachineRegionSourceX64(Result, Options);
      ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
      Wrapped->Source.Function.Name = "generic_machine_region";
      // These fixed PE addresses occupy ASan's shadow gap. Use UBSan here;
      // the register/memory transfer fixtures also run with ASan.
      emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
#if FRESH
  uint64_t choices,choice_count,choice_cursor;
#endif
} state_t;
_Static_assert(sizeof(state_t)==432+24*FRESH,"region state layout");
static void put(unsigned char *p,uint64_t v) {
  for(unsigned i=0;i<8;++i) p[i]=(unsigned char)(v>>(8*i));
}
int main(void) {
  void *wanted=(void *)(uintptr_t)UINT64_C(0x140001000);
  unsigned char *memory=mmap(wanted,4096,PROT_READ|PROT_WRITE,
                             MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(memory!=wanted) return 1;
  const uint64_t targets[]={0,0x12340,UINT64_C(0xffff800012345678),
                            UINT64_C(0x140001100)};
  for(unsigned trial=0;trial<4;++trial) {
    _Alignas(16) unsigned char stack[512],expected_stack[512];
    unsigned char expected_memory[4096];
    struct {uint64_t before[2];state_t s;uint64_t after[2];} box,expected;
    memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0x11223344;box.after[1]=0x55667788;
    for(unsigned i=0;i<4096;++i) memory[i]=(unsigned char)(i*19+trial);
    for(unsigned i=0;i<512;++i) stack[i]=(unsigned char)(i*17+trial);
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-trial-i;
    }
    s->gpr[0]=targets[trial];s->gpr[1]=(uintptr_t)(memory+SLOT);
    s->gpr[4]=(uintptr_t)(stack+264);s->flags=0x202;
    s->pc=0xabcdef;s->kind=0x987654;
    put(memory+SLOT,JUMP_STORE?0x55667788:targets[trial]);
    expected=box;memcpy(expected_memory,memory,4096);
    memcpy(expected_stack,stack,512);
    expected.s.pc=targets[trial];expected.s.kind=2;
    if(JUMP_STORE) put(expected_memory+SLOT,targets[trial]);
    if(generic_machine_region(MACHINE_ARG(s))) return 2;
    if(memcmp(&box,&expected,sizeof(box))) return 3;
    if(memcmp(memory,expected_memory,4096)) return 4;
    if(memcmp(stack,expected_stack,512)) return 5;
  }
  return munmap(memory,4096)!=0;
}
)C",
                 "#define _GNU_SOURCE\n#define FRESH " + std::to_string(Fresh) +
                     "\n#define SLOT " + std::to_string(Slot) +
                     "\n#define JUMP_STORE " + std::to_string(Kind == 2) +
                     "\n");
    }
  }
#endif
}

TEST_F(InterpreterMachineRegionSourceTest, QueryStopsNameOriginalPC) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  const uint8_t Code[]{0xf3, 0x0f, 0x6f, 0xc1,    // MOVDQU XMM0,XMM1
                       0xb8, 7,    0,    0,    0, // MOV EAX,7
                       0xb9, 3,    0,    0,    0, // MOV ECX,3
                       0x0f, 0xa2,                // CPUID (entry+14)
                       0x41, 0xff, 0xd3};         // CALL R11 (entry+16)
  auto Image = loadRegion(Code);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 16, ExitKind::Call);
  Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionSourceX64(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint32_t leaf,subleaf,answers[4];} query_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
} state_t;
int main(void) {
  for(unsigned kind=0;kind<5;++kind) {
    _Alignas(16) unsigned char stack[512],expected_stack[512];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    query_t tape[2]={{1,2,{3,4,5,6}},{7,3,{0x80000001u,22,33,0xffffffffu}}};
    query_t expected_tape[2];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x8765432100000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)(stack+264+(kind==4?1:0));
    s->gpr[11]=0x12345678;s->flags=0x246;
    s->queries=(uintptr_t)tape;s->count=kind==0?1:2;s->cursor=1;
    s->pc=0xabcdef;s->kind=0x987654;
    if(kind==1) tape[1].leaf=8;
    if(kind==2) tape[1].subleaf=4;
    expected=box;memcpy(expected_tape,tape,sizeof(tape));
    memcpy(expected_stack,stack,sizeof(stack));
    uint64_t status=kind==4?2:kind==0?3:kind<3?4:0;
    if(kind!=4) {
      expected.s.gpr[0]=7;expected.s.gpr[1]=3;
      memcpy(expected.s.xmm[0],s->xmm[1],16);
      if(status) {expected.s.pc=UINT64_C(0x14000100e);expected.s.kind=0;}
      else {
        const unsigned regs[]={0,3,1,2};
        for(unsigned i=0;i<4;++i) expected.s.gpr[regs[i]]=tape[1].answers[i];
        expected.s.cursor=2;expected.s.pc=s->gpr[11];expected.s.kind=1;
        expected.s.gpr[4]-=8;
        uint64_t ret=UINT64_C(0x140001013);
        for(unsigned i=0;i<8;++i) expected_stack[256+i]=(unsigned char)(ret>>(8*i));
      }
    }
    if(generic_machine_region(MACHINE_ARG(s))!=status) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       BranchExitsKeepSelectedKindAndTarget) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "region source execution requires clang";
  // JRCXZ return; JMP RAX; return: RET. The condition preserves flags.
  auto Image = loadRegion({0xe3, 2, 0xff, 0xe0, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Jump);
  Options.X64MachineExits.push_back({1, Entry + 4, ExitKind::Return});
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.ExternalExits.size(), 2U);
  auto Wrapped = wrapInterpreterMachineRegionSourceX64(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
int main(void) {
  for(unsigned branch=0;branch<2;++branch) {
    uint64_t state[54],expected[54],stack[8],expected_stack[8];
    for(unsigned i=0;i<54;++i) state[i]=UINT64_C(0x8123456789abc000)+i;
    for(unsigned i=0;i<8;++i) stack[i]=UINT64_C(0x1234567800000000)+i;
    state[1]=branch;state[4]=(uintptr_t)&stack[3];state[16]=0x246;
    state[17]=0;state[18]=0;state[19]=0;
    memcpy(expected,state,sizeof(state));
    memcpy(expected_stack,stack,sizeof(stack));
    expected[52]=branch?state[0]:stack[3];expected[53]=branch?2:3;
    if(!branch) expected[4]+=8;
    if(generic_machine_region(MACHINE_ARG(state))) return 1;
    if(memcmp(state,expected,sizeof(state))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       AuthenticatesResultAndExpectedDomain) {
  auto Image = loadRegion({0xff, 0xd0});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Options = regionOptions(Entry, ExitKind::Call);
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Good = wrapInterpreterMachineRegionSourceX64(Result, Options);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto Refused = [&](const SpecializationResult &Candidate,
                           const SpecializationOptions &Expected) {
    auto Source = wrapInterpreterMachineRegionSourceX64(Candidate, Expected);
    EXPECT_FALSE(bool(Source));
    if (!Source)
      EXPECT_FALSE(llvm::toString(Source.takeError()).empty());
  };
  auto Changed = Result;
  Changed.Status = SpecializationStatus::BudgetExceeded;
  Refused(Changed, Options);
  Changed = Result;
  Changed.RawEnvironment.reset();
  Refused(Changed, Options);
  Changed = Result;
  Changed.RawEnvironment->IdentityDigest[0] ^= 1;
  Refused(Changed, Options);
  Changed = Result;
  Changed.Residual.Blocks.back().Ops.back().Inputs[0] = NdVar::scalar(42, 8);
  Refused(Changed, Options);
  Changed = Result;
  ASSERT_FALSE(Changed.Origins.empty());
  ++Changed.Origins.front().NativeInstruction.Address;
  Refused(Changed, Options);
  Changed = Result;
  Changed.ExternalExits.clear();
  Refused(Changed, Options);
  auto WrongDomain = Options;
  WrongDomain.X64MachineExits.front().Kind = ExitKind::Jump;
  Refused(Result, WrongDomain);
  WrongDomain = Options;
  WrongDomain.ExternalStoresDisjointEntryFrame = true;
  Refused(Result, WrongDomain);
  WrongDomain = Options;
  WrongDomain.X64MachineExits.clear();
  Refused(Result, WrongDomain);
  for (unsigned ABI = 0; ABI != 3; ++ABI) {
    auto Ordinary =
        ABI == 0   ? wrapInterpreterMachineStateX64(
                         Result.Residual, BinaryFormat::ELF,
                         InterpreterMachineStateProfile::UserX64NoFaultV1)
        : ABI == 1 ? wrapInterpreterMachineQueryStateX64(
                         Result.Residual, BinaryFormat::ELF,
                         InterpreterMachineStateProfile::UserX64NoFaultV1)
                   : wrapInterpreterMachineQueryStateX64V2(
                         Result.Residual, BinaryFormat::ELF,
                         InterpreterMachineStateProfile::UserX64NoFaultV1);
    EXPECT_FALSE(bool(Ordinary));
    if (!Ordinary)
      EXPECT_NE(llvm::toString(Ordinary.takeError()).find("raw environment"),
                std::string::npos);
  }
}

TEST_F(InterpreterMachineRegionSourceTest, RefusesUnusedExtraEnvironment) {
  auto Image = loadRegion({0xff, 0xd0});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Options = regionOptions(Entry, ExitKind::Call);
    if (Kind == 0)
      Options.X64SegmentedReads = true;
    else if (Kind == 1)
      Options.X64SegmentedWrites = true;
    else if (Kind == 2)
      Options.X64CodeRegions.push_back(
          {1, 0x400000, {0xc3}, "unused source control"});
    else if (Kind == 3)
      Options.X64MachineExits.push_back({1, Entry + 2, ExitKind::Trap});
    else if (Kind == 4)
      Options.X64FlagsProfile =
          InterpreterMachineStateProfile::KernelX64NoFaultV1;
    else
      Options.X64MXCSRMask = 0xffbf;
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Valid = validateSpecializationRawEnvironment(Result, Options);
    ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
    auto Source = wrapInterpreterMachineRegionSourceX64(Result, Options);
    EXPECT_FALSE(bool(Source));
    if (!Source)
      EXPECT_NE(
          llvm::toString(Source.takeError()).find("machine-region source"),
          std::string::npos);
  }
}
