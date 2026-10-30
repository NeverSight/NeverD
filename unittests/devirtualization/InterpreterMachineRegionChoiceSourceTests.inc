//===- InterpreterMachineRegionChoiceSourceTests.inc - Choice execution ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte/state oracles for runtime architectural choices.

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourceCompactsLargeExports) {
  // Separate rejection blocks used to exceed the 16384-block source bound.
  // This checks export capacity; the separate execution tests check semantics.
  auto Wrapped = choiceSequence(3300);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  EXPECT_LT(Wrapped->Source.Function.Blocks.size(), 7000U);
  Wrapped->Source.Function.Name = "generic_machine_region";
  LowToMedConverter Converter;
  std::map<va_t, SourceFunctionTypeHint> Hints{
      {Wrapped->Source.Function.Entry, Wrapped->Source.SourceABI}};
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceEntryTypeHints(&Hints);
  auto Med = Converter.convert(Wrapped->Source.Function, Arch::X64);
  Med.SourceTypeHint = Wrapped->Source.SourceABI;
  inferMedTypes(Med, Arch::X64);
  ASSERT_TRUE(verifyMedFunc(Med, "large-choice-source-test"));
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.Format = BinaryFormat::ELF;
    Options.UseUnalignedPointers = false;
    if (LLVM) {
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Med}, Context);
      ASSERT_NE(Module, nullptr);
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
    } else {
      auto High = MedToHighConverter().convert(Med, Arch::X64);
      ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    }
    OS.flush();
    EXPECT_NE(Source.find("generic_machine_region"), std::string::npos);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourceCompactsInstructionSequences) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  // Every XOR consumes its original-PC record, even when overwritten later.
  auto Wrapped = choiceSequence(48);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  const unsigned positions[]={0,1,23,47};
  for(unsigned trial=0;trial<4;++trial) for(unsigned mode=0;mode<3;++mode) {
    uint64_t stack[8],expected_stack[8];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[48],expected_tape[48];
    for(unsigned i=0;i<48;++i)
      tape[i]=(choice_t){UINT64_C(0x140001000)+2*i,0,1,(i+trial)&1};
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)&stack[4];s->flags=0x202;stack[4]=0x12340;
    s->pc=0xabcdef;s->kind=0x987654;
    unsigned stop=mode?positions[trial]:48;
    s->choices=(uintptr_t)tape;s->choice_count=mode==1?stop:48;
    if(mode==2) ++tape[stop].pc;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));
    expected.s.choice_cursor=stop;
    expected.s.pc=UINT64_C(0x140001000)+2*stop;expected.s.kind=0;
    if(stop) {
      expected.s.gpr[0]=0;
      expected.s.flags=0x246|(((stop-1+trial)&1)<<4);
    }
    if(!mode) {
      expected.s.pc=0x12340;expected.s.kind=3;expected.s.gpr[4]+=8;
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(mode==1?5:mode==2?6:0)) return 1;
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
       ArchitecturalChoiceSourceSharesFailuresAcrossBranchArms) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  auto Image =
      loadRegion({0x85, 0xff, 0x74, 12,   0xb8, 0x34, 0x12, 0,    0,
                  0x48, 0x89, 0x06, 0x31, 0xc9, 0xeb, 10,   0xb8, 0x78,
                  0x56, 0,    0,    0x48, 0x89, 0x06, 0x31, 0xd2, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 26, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.ExternalStoresDisjointEntryFrame = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 32, {}};
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  for(unsigned arm=0;arm<2;++arm) for(unsigned a=0;a<2;++a)
  for(unsigned b=0;b<2;++b) for(unsigned mode=0;mode<3;++mode) {
    uint64_t stack[8],expected_stack[8],data[3]={11,22,33},expected_data[3];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[2]={{UINT64_C(0x140001000),0,1,a},
      {UINT64_C(0x140001000)+(arm?12:24),0,1,b}},expected_tape[2];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)&stack[4];s->flags=0x202;stack[4]=0x12340;
    s->gpr[7]=arm;s->gpr[6]=(uintptr_t)&data[1];
    s->pc=0xabcdef;s->kind=0x987654;
    s->choices=(uintptr_t)tape;s->choice_count=mode==1?1:2;
    if(mode==2) tape[1].bits=8;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));memcpy(expected_data,data,sizeof(data));
    expected.s.gpr[0]=expected_data[1]=arm?0x1234:0x5678;
    expected.s.flags=(arm?0x202:0x246)|(a<<4);
    expected.s.pc=UINT64_C(0x140001000)+(arm?12:24);expected.s.kind=0;
    expected.s.choice_cursor=1;
    if(!mode) {
      expected.s.gpr[arm?1:2]=0;expected.s.flags=0x246|(b<<4);
      expected.s.pc=0x12340;expected.s.kind=3;expected.s.gpr[4]+=8;
      expected.s.choice_cursor=2;
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(mode==1?5:mode==2?6:0)) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
    if(memcmp(data,expected_data,sizeof(data))) return 5;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourceChecksAndCommitsWholeInstructions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  auto Image =
      loadRegion({0xb8, 15,   0,    0,    0,  0x83, 0xc0, 1,    0x31, 0xc0,
                  0x9c, 0x59, 0xf6, 0xc1, 16, 0x75, 6,    0xb8, 1,    0,
                  0,    0,    0xc3, 0xb8, 2,  0,    0,    0,    0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 22, ExitKind::Return);
  Options.X64MachineExits.push_back({1, Entry + 28, ExitKind::Return});
  Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  EXPECT_EQ(Wrapped->StateABI, "x64-machine-region-choice-state-v1");
  EXPECT_EQ(Wrapped->StateBytes, 456U);
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
_Static_assert(sizeof(state_t)==456,"choice state layout");
_Static_assert(sizeof(choice_t)==32,"choice record layout");
static void put(unsigned char *p,uint64_t v) {
  for(unsigned i=0;i<8;++i) p[i]=(unsigned char)(v>>(8*i));
}
int main(void) {
  for(unsigned a=0;a<2;++a) for(unsigned b=0;b<2;++b)
  for(unsigned mode=0;mode<14;++mode) {
    _Alignas(16) unsigned char stack[512],expected_stack[512];
    struct {uint64_t before[2];state_t s;uint64_t after[2];} box,expected;
    choice_t tape[3]={{99,88,77,66},
      {UINT64_C(0x140001008),0,1,a},{UINT64_C(0x14000100c),0,1,b}};
    choice_t expected_tape[3];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0x11223344;box.after[1]=0x55667788;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)(stack+264+(mode==13));s->flags=0x202;
    put(stack+264,0x12340);s->pc=0xabcdef;s->kind=0x987654;
    s->choices=(uintptr_t)tape;s->choice_count=3;s->choice_cursor=1;
    if(mode==1) {s->choice_count=1;s->choices=0;}
    if(mode==2) s->choice_count=2;
    if(mode==3) {s->choice_cursor=4;s->choices=0;}
    if(mode==4) {s->choice_count=UINT64_MAX;s->choice_cursor=UINT64_MAX;s->choices=0;}
    if(mode>=5&&mode<=12) {
      choice_t *c=&tape[mode<9?1:2];
      unsigned bad=(mode-5)%4;
      if(bad==0) ++c->pc;
      if(bad==1) ++c->ordinal;
      if(bad==2) ++c->bits;
      if(bad==3) c->value|=2;
    }
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));
    uint64_t status=mode==13?2:mode>=5?6:mode?5:0;
    if(mode!=13) {
      unsigned first=(mode==1||mode==3||mode==4||(mode>=5&&mode<9));
      if(first) {
        expected.s.gpr[0]=16;expected.s.flags=0x212;
        expected.s.pc=UINT64_C(0x140001008);expected.s.kind=0;
      } else {
        expected.s.gpr[0]=0;expected.s.flags=0x246|(a<<4);
        expected.s.gpr[1]=expected.s.flags;expected.s.choice_cursor=2;
        put(expected_stack+256,expected.s.flags);
        expected.s.pc=UINT64_C(0x14000100c);expected.s.kind=0;
        if(!status) {
          expected.s.gpr[0]=1+a;expected.s.gpr[4]+=8;
          expected.s.flags=(a?0x202:0x246)|(b<<4);
          expected.s.pc=0x12340;expected.s.kind=3;
          expected.s.choice_cursor=3;
        }
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
       ArchitecturalChoiceSourceRequiresItsExactEnvironment) {
  auto Image = loadRegion({0x31, 0xc0, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Return);
  auto Selected = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Selected.complete()) << Selected.Diagnostic;
  auto Wrong = wrapInterpreterMachineRegionChoiceSourceX64V1(Selected, Options);
  EXPECT_FALSE(bool(Wrong));
  if (!Wrong)
    llvm::consumeError(Wrong.takeError());
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Full = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
  uint64_t Operations = 0;
  for (const auto &Block : Full->Source.Function.Blocks)
    Operations += Block.Ops.size();
  ASSERT_GT(Operations, 1U);
  for (uint64_t Limit :
       {uint64_t{0}, uint64_t{1}, Operations - 1, Operations}) {
    InterpreterMachineRegionSourceOptions SourceOptions;
    SourceOptions.MaxChoiceSourceOperations = Limit;
    auto Bounded = wrapInterpreterMachineRegionChoiceSourceX64V1(
        Result, Options, SourceOptions);
    EXPECT_EQ(bool(Bounded), Limit == Operations) << Limit;
    if (!Bounded)
      EXPECT_NE(llvm::toString(Bounded.takeError()).find("budget"),
                std::string::npos);
  }
  auto Old = wrapInterpreterMachineRegionSourceX64(Result, Options);
  EXPECT_FALSE(bool(Old));
  if (!Old)
    llvm::consumeError(Old.takeError());
  bool Changed = false;
  for (auto &Block : Result.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (isLowArchitecturalValue(Op)) {
        Op.Inputs[4] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  auto Stale = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  EXPECT_FALSE(bool(Stale));
  if (!Stale)
    llvm::consumeError(Stale.takeError());
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourceReservesAllGuardedShiftValues) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  // The variable-count SHL has two productions (AF, OF), including visits
  // with masked count zero where both guards are false.
  auto Image = loadRegion({0xb8, 15, 0, 0, 0, 0x83, 0xc0, 1, 0xd3, 0xe0, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 10, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  const unsigned counts[]={0,1,2,32,33};
  for(unsigned c=0;c<5;++c) for(unsigned a=0;a<2;++a)
  for(unsigned b=0;b<2;++b) for(unsigned mode=0;mode<3;++mode) {
    uint64_t stack[8],expected_stack[8];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[2]={{UINT64_C(0x140001008),0,1,a},
                      {UINT64_C(0x140001008),1,1,b}};
    choice_t expected_tape[2];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)&stack[4];s->gpr[1]=counts[c];s->flags=0x202;
    stack[4]=0x12340;s->pc=0xabcdef;s->kind=0x987654;
    s->choices=(uintptr_t)tape;s->choice_count=mode==1?1:2;
    if(mode==2) tape[1].ordinal=0;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));
    expected.s.gpr[0]=16;expected.s.flags=0x212;
    expected.s.pc=UINT64_C(0x140001008);expected.s.kind=0;
    if(!mode) {
      unsigned n=counts[c]&31;
      expected.s.gpr[0]=UINT64_C(16)<<n;
      expected.s.flags=n?0x202|(a<<4)|(n>1?b<<11:0):0x212;
      expected.s.pc=0x12340;expected.s.kind=3;
      expected.s.gpr[4]+=8;expected.s.choice_cursor=2;
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(mode==1?5:mode==2?6:0)) return 1;
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
       ArchitecturalChoiceSourceKeepsRepeatedVisitsAndCopiesDistinct) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  // Each iteration makes one production and two PUSHFQ snapshots. Repeated
  // PCs need distinct records; copied/spilled snapshots need the same value.
  auto Image = loadRegion({0xb9, 2,    0,    0,    0,    0x31, 0xc0, 0x9c,
                           0x5a, 0x9c, 0x5b, 0x48, 0x89, 0x17, 0x48, 0x8d,
                           0x7f, 8,    0xff, 0xc9, 0x75, 0xef, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 22, ExitKind::Return);
  Options.ExternalStoresDisjointEntryFrame = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 32, {}};
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  for(unsigned a=0;a<2;++a) for(unsigned b=0;b<2;++b)
  for(unsigned mode=0;mode<3;++mode) {
    uint64_t stack[8],expected_stack[8],data[4]={11,22,33,44},expected_data[4];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[2]={{UINT64_C(0x140001005),0,1,a},
                      {UINT64_C(0x140001005),0,1,b}};
    choice_t expected_tape[2];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)&stack[4];s->gpr[7]=(uintptr_t)&data[1];s->flags=0x202;
    stack[4]=0x12340;s->pc=0xabcdef;s->kind=0x987654;
    s->choices=(uintptr_t)tape;s->choice_count=mode==1?1:2;
    if(mode==2) ++tape[1].pc;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_data,data,sizeof(data));memcpy(expected_tape,tape,sizeof(tape));
    expected.s.gpr[0]=0;expected.s.gpr[1]=0;
    expected.s.gpr[2]=expected.s.gpr[3]=0x246|(b<<4);
    expected_stack[3]=expected.s.gpr[2];
    expected_data[1]=0x246|(a<<4);expected_data[2]=0x246|(b<<4);
    expected.s.gpr[7]+=16;expected.s.gpr[4]+=8;
    expected.s.flags=0x246;expected.s.pc=0x12340;expected.s.kind=3;
    expected.s.choice_cursor=2;
    if(mode) {
      expected.s.gpr[1]=1;expected.s.gpr[4]-=8;expected.s.gpr[7]-=8;
      expected.s.gpr[2]=expected.s.gpr[3]=0x246|(a<<4);
      expected_stack[3]=expected.s.gpr[2];expected_data[2]=data[2];
      expected.s.flags=0x202;expected.s.pc=UINT64_C(0x140001005);
      expected.s.kind=0;expected.s.choice_cursor=1;
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(mode==1?5:mode==2?6:0)) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
    if(memcmp(data,expected_data,sizeof(data))) return 5;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourcePreservesByteViewsAndWholeTemporaryValues) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  auto Image = loadRegion({0x90, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 1, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.ExternalStoresDisjointEntryFrame = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 32, {}};
  const auto Native = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Native.complete()) << Native.Diagnostic;
  ArchitecturalChoiceProvider Provider;
  Provider.Domain = Native.RawEnvironment->Domain;
  Provider.Code[Entry + 1] = Native.ExternalExits.front().Original;
  // An owned provider-level instruction, not a claim about NOP semantics.
  // The full temporary is produced before any ordinary operation; its copy
  // and memory spill must survive later changes to the original destination.
  SpecializationInstruction Instruction;
  const auto Guard = NdVar::tmp(0x100, 1);
  const auto Fresh = NdVar::tmp(0x200, 8);
  const auto Copy = NdVar::tmp(0x280, 8);
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    Op.Addr = Entry;
    Op.Seq = Instruction.Ops.size();
    for (const auto &Input : Inputs)
      Op.addInput(Input);
    Instruction.Ops.push_back(Op);
  };
  Emit(NdOp::INT_AND, Guard, {NdVar::reg(x86reg::R8, 1), NdVar::scalar(1, 1)});
  Emit(NdOp::STORE, {},
       {NdVar::reg(x86reg::RDI, 8), NdVar::scalar(0x12345678, 8)});
  Emit(NdOp::COPY, Copy, {Fresh});
  Emit(NdOp::COPY, NdVar::reg(x86reg::RDX, 8), {Copy});
  Emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RDI, 8), Copy});
  Instruction.Origin.Address = Entry;
  Instruction.Origin.Size = 1;
  Instruction.Origin.OpCount = Instruction.Ops.size();
  Instruction.Fallthrough = {Entry + 1};
  auto &Effects = Instruction.UndefinedEffects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OpCount = Instruction.Ops.size();
  Effects.OperationDigest = lowUndefinedOperationDigest(Instruction.Ops);
  Effects.Effects = {
      {0, Fresh, 0, 64, std::nullopt},
      {Effects.OpCount, NdVar::reg(x86reg::RAX, 4), 8, 8, Guard},
      {Effects.OpCount, NdVar::reg(x86reg::RDX, 4), 0, 32, std::nullopt}};
  Provider.Code[Entry] = Instruction;
  const auto Result = specializeInterpreter(Provider, {Entry}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  for(unsigned guard=0;guard<2;++guard) for(unsigned mode=0;mode<3;++mode) {
    uint64_t stack[8],expected_stack[8],data[3]={11,22,33},expected_data[3];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[3]={{UINT64_C(0x140001000),0,64,UINT64_C(0xfedcba9876543210)},
      {UINT64_C(0x140001000),1,8,0xab},{UINT64_C(0x140001000),2,32,0x87654321}};
    choice_t expected_tape[3];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[0]=UINT64_C(0x123456789abcdef0);s->gpr[8]=guard;
    s->gpr[4]=(uintptr_t)&stack[4];s->gpr[7]=(uintptr_t)&data[1];s->flags=0x202;
    stack[4]=0x12340;s->pc=0xabcdef;s->kind=0x987654;
    s->choices=(uintptr_t)tape;s->choice_count=mode==1?2:3;
    if(mode==2) ++tape[2].pc;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_data,data,sizeof(data));memcpy(expected_tape,tape,sizeof(tape));
    expected.s.pc=UINT64_C(0x140001000);expected.s.kind=0;
    if(!mode) {
      expected.s.gpr[0]=guard?UINT64_C(0x123456789abcabf0):s->gpr[0];
      expected.s.gpr[2]=UINT64_C(0xfedcba9887654321);
      expected_data[1]=UINT64_C(0xfedcba9876543210);
      expected.s.gpr[4]+=8;expected.s.pc=0x12340;expected.s.kind=3;
      expected.s.choice_cursor=3;
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(mode==1?5:mode==2?6:0)) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
    if(memcmp(data,expected_data,sizeof(data))) return 5;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST(ArchitecturalValueShape, RejectsMalformedRetainedOperations) {
  LowUndefinedEffect Effect;
  Effect.Output = NdVar::reg(x86reg::AF, 1);
  Effect.BitCount = 1;
  auto Op = makeLowArchitecturalValue(Effect, 0x1000);
  ASSERT_TRUE(Op);
  ASSERT_TRUE(decodeLowArchitecturalValue(*Op));
  for (unsigned Kind = 0; Kind != 13; ++Kind) {
    auto Bad = *Op;
    switch (Kind) {
    case 0:
      Bad.NumInputs = 4;
      break;
    case 1:
      Bad.Inputs[0].Offset |= UINT64_C(1) << 32;
      break;
    case 2:
      Bad.Inputs[0].Size = 2;
      break;
    case 3:
      Bad.Inputs[1] = NdVar::reg(x86reg::CF, 1);
      break;
    case 4:
      Bad.Inputs[2] = NdVar::scalar(2, 1);
      break;
    case 5:
      Bad.Inputs[2] = NdVar::reg(x86reg::ZF, 1);
      break;
    case 6:
      Bad.Inputs[3] = NdVar::scalar(8, 8);
      break;
    case 7:
      Bad.Inputs[4] = NdVar::scalar(0, 8);
      break;
    case 8:
      Bad.Output = NdVar::tmp(UINT64_MAX, 8);
      break;
    case 9:
      Bad.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 10:
      Bad.Inputs[5] = NdVar::scalar(0, 8);
      break;
    case 11:
      Bad.ConsumesX87Argument = true;
      break;
    case 12:
      Bad.Inputs[4] = NdVar::address(1, 8);
      break;
    }
    EXPECT_FALSE(decodeLowArchitecturalValue(Bad)) << Kind;
  }
  Effect.Output = NdVar::tmp(16, 8);
  Effect.BitCount = 64;
  Op = makeLowArchitecturalValue(Effect, 0x1000);
  ASSERT_TRUE(Op);
  EXPECT_TRUE(decodeLowArchitecturalValue(*Op));
  EXPECT_EQ(Op->Inputs[1], NdVar::scalar(0, 8));
  Op->Inputs[2] = NdVar::scalar(0, 1);
  EXPECT_FALSE(decodeLowArchitecturalValue(*Op));
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourceOrdersQueriesAndChoices) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  auto Image =
      loadRegion({0x31, 0xc0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2, 0x31, 0xd2, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 11, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {uint32_t leaf,subleaf,answers[4];} query_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  for(unsigned mode=0;mode<4;++mode) {
    uint64_t stack[8],expected_stack[8];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[2]={{UINT64_C(0x140001000),0,1,1},
                      {UINT64_C(0x140001009),0,1,0}},expected_tape[2];
    query_t query={0,3,{0x80000001u,22,33,0xffffffffu}},expected_query;
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)&stack[4];s->flags=0x202;stack[4]=0x12340;
    s->pc=0xabcdef;s->kind=0x987654;
    s->choices=(uintptr_t)tape;s->choice_count=mode?1:2;
    s->queries=(uintptr_t)&query;s->count=mode==1?0:1;
    if(mode==2) ++query.subleaf;
    expected=box;expected_query=query;
    memcpy(expected_stack,stack,sizeof(stack));memcpy(expected_tape,tape,sizeof(tape));
    expected.s.gpr[0]=0;expected.s.gpr[1]=3;expected.s.flags=0x256;
    expected.s.pc=UINT64_C(0x140001007);expected.s.kind=0;
    expected.s.choice_cursor=1;
    if(mode==0||mode==3) {
      expected.s.gpr[0]=0x80000001u;expected.s.gpr[3]=22;
      expected.s.gpr[1]=33;expected.s.gpr[2]=0xffffffffu;
      expected.s.cursor=1;expected.s.pc=UINT64_C(0x140001009);
      if(!mode) {
        expected.s.gpr[2]=0;expected.s.flags=0x246;
        expected.s.pc=0x12340;expected.s.kind=3;expected.s.gpr[4]+=8;
        expected.s.choice_cursor=2;
      }
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(mode==1?3:mode==2?4:mode==3?5:0)) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
    if(memcmp(&query,&expected_query,sizeof(query))) return 5;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourceCommitsBeforeAnExplicitTrapStop) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  auto Image = loadRegion({0x31, 0xc0, 0xcc});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 2, ExitKind::Trap);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Default = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  EXPECT_FALSE(bool(Default));
  if (!Default)
    llvm::consumeError(Default.takeError());
  InterpreterMachineRegionSourceOptions SourceOptions;
  SourceOptions.StopBeforeTraps = true;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options,
                                                               SourceOptions);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
int main(void) {
  for(unsigned a=0;a<2;++a) {
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape={UINT64_C(0x140001000),0,1,a},expected_tape=tape;
    memset(&box,0,sizeof(box));box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9876543200000000)+i;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->flags=0x202;s->choices=(uintptr_t)&tape;s->choice_count=1;
    s->pc=0xabcdef;s->kind=0x987654;expected=box;
    expected.s.gpr[0]=0;expected.s.flags=0x246|(a<<4);
    expected.s.pc=UINT64_C(0x140001002);expected.s.kind=4;expected.s.choice_cursor=1;
    if(generic_machine_region(MACHINE_ARG(s))) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(&tape,&expected_tape,sizeof(tape))) return 3;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       ArchitecturalChoiceSourcePackedArithmeticAndImmediateProducts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  // Packed lanes have no arbitrary outputs. Each immediate product consumes
  // four independently supplied flag bits before committing any of its state.
  auto Image =
      loadRegion({0x66, 0x0f, 0xfc, 0xc1, 0x66, 0x0f, 0xfb, 0xd3, 0x48, 0x6b,
                  0xc1, 0xfd, 0x4d, 0x69, 0xc8, 0x78, 0x56, 0x34, 0x12, 0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 19, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
static uint64_t product(uint64_t bits,int64_t factor,unsigned *overflow) {
  int64_t signed_input, signed_low;
  memcpy(&signed_input,&bits,8);
  __int128 full=(__int128)signed_input*factor;
  uint64_t low=(uint64_t)full;
  memcpy(&signed_low,&low,8);
  *overflow=full!=(__int128)signed_low;
  return low;
}
static uint64_t product_flags(uint64_t old,unsigned overflow,const choice_t *tape) {
  return (old&~UINT64_C(0x8d5))|overflow|(overflow<<11)|
    (tape[0].value<<7)|(tape[1].value<<6)|(tape[2].value<<4)|(tape[3].value<<2);
}
int main(void) {
  const uint64_t values[]={0,1,UINT64_MAX,UINT64_C(0x8000000000000000),
    UINT64_C(0x7fffffffffffffff),UINT64_C(0x100000005)};
  const unsigned corrupt[]={0,3,4,7};
  for(unsigned seed=0;seed<16;++seed) for(unsigned mode=0;mode<13;++mode) {
    uint64_t stack[8],expected_stack[8];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[8],expected_tape[8];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=UINT64_C(0x123456789abcdef0);box.after=~box.before;
    for(unsigned i=0;i<16;++i) {
      box.s.gpr[i]=UINT64_C(0xdeadbeef00000000)+i;
      box.s.xmm[i][0]=UINT64_C(0xff01fe02fd03fc04)*(1+i+seed);
      box.s.xmm[i][1]=UINT64_C(0x80ff7f000001ffff)*(1+i+seed);
    }
    box.s.gpr[1]=values[seed%6];box.s.gpr[8]=values[(seed+3)%6];
    box.s.gpr[4]=(uintptr_t)&stack[4];stack[4]=0x12340;
    box.s.flags=0x202|((seed&1)?0x400:0)|((seed&2)?0x8d5:0);
    box.s.pc=0xabcdef;box.s.kind=0x987654;
    for(unsigned i=0;i<8;++i)
      tape[i]=(choice_t){UINT64_C(0x140001000)+(i<4?8:12),i%4,1,
                         ((seed^(i<4?0:15))>>(i%4))&1};
    box.s.choices=(uintptr_t)tape;box.s.choice_count=mode<9?mode:8;
    if(mode>=9) ++tape[corrupt[mode-9]].ordinal;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));
    unsigned char *dst=(unsigned char *)expected.s.xmm[0];
    const unsigned char *src=(const unsigned char *)box.s.xmm[1];
    for(unsigned i=0;i<16;++i) dst[i]=(unsigned char)(dst[i]+src[i]);
    for(unsigned i=0;i<2;++i) expected.s.xmm[2][i]-=box.s.xmm[3][i];
    unsigned committed=mode<9?(mode<4?0:mode<8?4:8):(corrupt[mode-9]<4?0:4);
    unsigned overflow=0;
    if(committed>=4) {
      expected.s.gpr[0]=product(box.s.gpr[1],-3,&overflow);
      expected.s.flags=product_flags(expected.s.flags,overflow,tape);
    }
    if(committed==8) {
      expected.s.gpr[9]=product(box.s.gpr[8],0x12345678,&overflow);
      expected.s.flags=product_flags(expected.s.flags,overflow,tape+4);
      expected.s.pc=0x12340;expected.s.kind=3;expected.s.gpr[4]+=8;
    } else {
      expected.s.pc=UINT64_C(0x140001000)+(committed?12:8);expected.s.kind=0;
    }
    expected.s.choice_cursor=committed;
    uint64_t status=committed==8?0:mode<9?5:6;
    if(generic_machine_region(MACHINE_ARG(&box.s))!=status) return 1;
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
       ArchitecturalChoiceSourcePackedSelectionsAndAlignedRead) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "choice source execution requires clang";
  auto Image = loadRegion(
      {0x66, 0x0f, 0xdb, 0x64, 0x24, 0x10,   // PAND XMM4,[RSP+16]
       0x66, 0x0f, 0x70, 0xc1, 0x1b,         // PSHUFD XMM0,XMM1,0x1b
       0x66, 0x0f, 0x76, 0xd3,               // PCMPEQD XMM2,XMM3
       0x66, 0x0f, 0xdb, 0xc2,               // PAND XMM0,XMM2
       0x66, 0x0f, 0x6b, 0xee,               // PACKSSDW XMM5,XMM6
       0x66, 0x0f, 0x63, 0xed,               // PACKSSWB XMM5,XMM5
       0x66, 0x0f, 0x6c, 0xf8,               // PUNPCKLQDQ XMM7,XMM0
       0x66, 0x44, 0x0f, 0x50, 0xc2, 0xc3}); // MOVMSKPD R8D,XMM2; RET
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 36, ExitKind::Return);
  Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 0};
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  unsigned Guards = 0;
  for (const auto &Block : Wrapped->Source.Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 3 &&
          Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == uint64_t(Intrinsic::RequireAligned)) {
        EXPECT_FALSE(Op.Inputs[1].isConst());
        EXPECT_EQ(Op.Inputs[2], NdVar::scalar(16, 8));
        ++Guards;
      }
  EXPECT_EQ(Guards, 1U);
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor;
} state_t;
static int16_t narrow_word(int32_t value) {
  return value < -32768 ? -32768 : value > 32767 ? 32767 : (int16_t)value;
}
static int8_t narrow_byte(int16_t value) {
  return value < -128 ? -128 : value > 127 ? 127 : (int8_t)value;
}
int main(void) {
  const int32_t values[]={INT32_MIN,-32769,-32768,-129,-128,-1,0,
                         127,128,32767,32768,INT32_MAX};
  for(unsigned seed=0;seed<16;++seed) for(unsigned bad=0;bad<2;++bad) {
    _Alignas(32) unsigned char stack[96],expected_stack[96];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    choice_t tape[3]={{1,2,1,0},{3,4,1,1},{5,6,1,0}},expected_tape[3];
    memset(&box,0,sizeof(box));memset(stack,0xa5,sizeof(stack));
    box.before=UINT64_C(0x123456789abcdef0);box.after=~box.before;
    for(unsigned i=0;i<16;++i) {
      box.s.gpr[i]=UINT64_C(0xdeadbeef00000000)+i;
      box.s.xmm[i][0]=UINT64_C(0xff01fe02fd03fc04)*(1+i+seed);
      box.s.xmm[i][1]=UINT64_C(0x80ff7f000001ffff)*(1+i+seed);
    }
    uint32_t left[4],right[4];
    for(unsigned i=0;i<4;++i) {
      left[i]=(uint32_t)values[(seed+i)%12];
      right[i]=left[i]^((seed&(1u<<i))?0:0x100);
    }
    memcpy(box.s.xmm[2],left,16);memcpy(box.s.xmm[3],right,16);
    for(unsigned i=0;i<8;++i)
      memcpy((unsigned char *)box.s.xmm[5+i/4]+(i%4)*4,&values[(seed+i)%12],4);
    box.s.gpr[4]=(uintptr_t)(stack+32+bad);
    uint64_t return_pc=0x12340;memcpy(stack+32+bad,&return_pc,8);
    box.s.flags=0x202|((seed&1)?0x400:0)|((seed&2)?0x8d5:0);
    box.s.pc=0xabcdef;box.s.kind=0x987654;
    box.s.choices=(uintptr_t)tape;box.s.choice_count=3;box.s.choice_cursor=1;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_tape,tape,sizeof(tape));
    if(!bad) {
      unsigned char *masked=(unsigned char *)expected.s.xmm[4];
      for(unsigned i=0;i<16;++i) masked[i]&=stack[48+i];
      uint32_t shuffled[4],source[4],compared[4];
      memcpy(source,box.s.xmm[1],16);
      for(unsigned i=0;i<4;++i) {
        compared[i]=left[i]==right[i]?UINT32_MAX:0;
        shuffled[i]=source[3-i]&compared[i];
      }
      memcpy(expected.s.xmm[0],shuffled,16);
      memcpy(expected.s.xmm[2],compared,16);
      int32_t wide[8];int16_t words[8];int8_t bytes[16];
      memcpy(wide,box.s.xmm[5],16);memcpy(wide+4,box.s.xmm[6],16);
      for(unsigned i=0;i<8;++i) words[i]=narrow_word(wide[i]);
      for(unsigned i=0;i<16;++i) bytes[i]=narrow_byte(words[i%8]);
      memcpy(expected.s.xmm[5],bytes,16);
      expected.s.xmm[7][1]=expected.s.xmm[0][0];
      expected.s.gpr[8]=(compared[1]>>31)|((uint64_t)(compared[3]>>31)<<1);
      expected.s.gpr[4]+=8;expected.s.pc=return_pc;expected.s.kind=3;
    }
    if(generic_machine_region(MACHINE_ARG(&box.s))!=(bad?2:0)) return 1;
    if(memcmp(&box,&expected,sizeof(box))) return 2;
    if(memcmp(stack,expected_stack,sizeof(stack))) return 3;
    if(memcmp(tape,expected_tape,sizeof(tape))) return 4;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}
