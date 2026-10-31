//===- InterpreterMachineRegionEntryObjectTests.inc - Entry domains -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete-state source checks for caller-declared frame/object separation.
/// Included after the shared machine-region and kernel source fixtures.
//===----------------------------------------------------------------------===//

llvm::Expected<InterpreterMachineRegionSource>
entryObjectSource(const SpecializationResult &Result,
                  const SpecializationOptions &Options, unsigned Version) {
  switch (Version) {
  case 0:
    return wrapInterpreterMachineRegionSourceX64(Result, Options);
  case 1:
    return wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  case 2:
    return wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
  default:
    return wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       EntryObjectsRejectBeforeGuestWritesAndQueries) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "entry object source execution requires clang";
  auto Image = loadRegion({0x48, 0x89, 0x01, // MOV [RCX],RAX
                           0x48, 0x89, 0x1a, // MOV [RDX],RBX
                           0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2,
                           0xc3}); // CPUID; RET
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (unsigned Version = 0; Version != 4; ++Version)
    for (bool Aligned : {false, true}) {
      SCOPED_TRACE(Version);
      SCOPED_TRACE(Aligned);
      auto Options = regionOptions(Entry + 18, ExitKind::Return);
      Options.EntryFrameBounds = SpecializationEntryFrameBounds{-16, 16};
      Options.EntryFrameExcludedObjects = {{{x86reg::RCX, 8}, -4, 12},
                                           {{x86reg::RDX, 8}, 0, 8}};
      Options.ProveStoreFrameSeparation = true;
      if (Aligned)
        Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 0};
      if (Version)
        Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
      if (Version >= 2)
        Options.X64SegmentedReads = true;
      if (Version == 3)
        Options = kernelPageGlobalOptions(Options);
      const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      auto Wrapped = entryObjectSource(Result, Options, Version);
      ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
      Wrapped->Source.Function.Name = "generic_machine_region";
      emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint32_t leaf,subleaf,answer[4];} query_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
#if VERSION > 0
  uint64_t choices,choice_count,choice_cursor;
#endif
#if VERSION >= 2
  uint64_t fsbase,gsbase;
#endif
#if VERSION == 3
  uint64_t cr4;
#endif
} state_t;
int main(void) {
  for(unsigned trial=0;trial<4;++trial) for(unsigned mode=0;mode<15;++mode) {
    _Alignas(16) unsigned char arena[512],want_arena[512];
    struct {uint64_t before;state_t s;uint64_t after;} box,want;
    query_t tape={7,3,{0x80000000u+trial,11+trial,12+trial,13+trial}},want_tape;
    memset(&box,0,sizeof(box));
    for(unsigned i=0;i<512;++i)arena[i]=(unsigned char)(i*19+trial);
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456700000000)+i+trial;
      s->xmm[i][0]=UINT64_C(0x8877665544332211)+i;
      s->xmm[i][1]=UINT64_C(0x1122334455667788)-i-trial;
    }
    s->flags=0x202|(trial&1?0xcd5:0);
    s->gpr[4]=(uintptr_t)(arena+256);
    s->gpr[1]=(uintptr_t)(arena+64);s->gpr[2]=(uintptr_t)(arena+128);
    s->queries=(uintptr_t)&tape;s->count=1;
    s->pc=0xabcdef;s->kind=0x987654;
#if VERSION > 0
    s->choices=1;s->choice_count=83;s->choice_cursor=17;
#endif
#if VERSION >= 2
    s->fsbase=0x12340000;s->gsbase=0x56780000;
#endif
#if VERSION == 3
    s->cr4=0x620|(trial&1?128:0);
#endif
    if(mode==1)s->gpr[2]=s->gpr[1];
    if(mode==2)s->gpr[2]=s->gpr[1]+3;
    if(mode==3) {s->gpr[1]=s->gpr[4]-28;s->gpr[2]=s->gpr[4]+16;}
    if(mode==4) {s->gpr[1]=s->gpr[4]+20;s->gpr[2]=s->gpr[4]-24;}
    if(mode==5)s->gpr[1]=s->gpr[4]-27; /* only the object's last byte overlaps */
    if(mode==6)s->gpr[1]=s->gpr[4]+19; /* only the object's first byte overlaps */
    if(mode==7)s->gpr[2]=s->gpr[4]+15;
    if(mode==8)s->gpr[1]=0;
    if(mode==9)s->gpr[1]=UINT64_MAX-7;
    if(mode==10)s->gpr[2]=UINT64_MAX-3;
    if(mode==11)s->gpr[4]=0;
    if(mode==12)s->gpr[4]=UINT64_MAX-7;
    if(mode==13)s->gpr[4]+=1;
    if(mode==14)s->gpr[2]=s->gpr[4]-16;
    unsigned rejected=(mode>=5 && mode!=13)||(mode==13 && ALIGNED);
    if(rejected)s->queries=1; /* rejection must precede the query tape read */
    uint64_t resume=UINT64_C(0x123456789abcdef0)+trial;
    if(mode!=11 && mode!=12)memcpy((void *)(uintptr_t)s->gpr[4],&resume,8);
    want=box;want_tape=tape;memcpy(want_arena,arena,sizeof(arena));
    if(!rejected) {
      memcpy(want_arena+(s->gpr[1]-(uintptr_t)arena),&s->gpr[0],8);
      memcpy(want_arena+(s->gpr[2]-(uintptr_t)arena),&s->gpr[3],8);
      want.s.gpr[0]=tape.answer[0];want.s.gpr[3]=tape.answer[1];
      want.s.gpr[1]=tape.answer[2];want.s.gpr[2]=tape.answer[3];
      want.s.cursor=1;want.s.gpr[4]+=8;want.s.pc=resume;want.s.kind=3;
    }
    if(generic_machine_region(MACHINE_ARG(s))!=(rejected?2:0))return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
    if(memcmp(arena,want_arena,sizeof(arena)))return 3;
    if(memcmp(&tape,&want_tape,sizeof(tape)))return 4;
  }
  return 0;
}

)C",
                 "#define VERSION " + std::to_string(Version) +
                     "\n#define ALIGNED " + std::to_string(Aligned) + "\n",
                 nullptr, true, true);
    }
}

TEST_F(InterpreterMachineRegionSourceTest,
       EntryObjectsKeepFullWidthEndpointsAndSharedRoots) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "entry object source execution requires clang";
  auto Image = loadRegion({0xff, 0xe0}); // JMP RAX; no guest memory access
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const int64_t Bounds[][4] = {{-8, 8, -4, 4},
                               {16, 24, -8, 0},
                               {-24, -16, 0, 8},
                               {INT64_MIN, INT64_MAX, 0, 1},
                               {0, 1, INT64_MIN, INT64_MIN + 1},
                               {-1, 0, INT64_MAX - 1, INT64_MAX}};
  for (const auto &B : Bounds) {
    auto Options =
        kernelPageGlobalOptions(regionOptions(Entry, ExitKind::Jump));
    Options.EntryFrameBounds = SpecializationEntryFrameBounds{B[0], B[1]};
    Options.EntryFrameExcludedObjects = {{{x86reg::RCX, 8}, B[2], B[3]},
                                         {{x86reg::RDX, 8}, B[2], B[3]}};
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Wrapped = entryObjectSource(Result, Options, 3);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    Wrapped->Source.Function.Name = "generic_machine_region";
    const auto SignedLiteral = [](int64_t Value) {
      return Value == INT64_MIN ? std::string("INT64_MIN")
                                : "INT64_C(" + std::to_string(Value) + ")";
    };
    emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase,cr4;
} state_t;
static int range(uint64_t root,int64_t begin,int64_t end,__int128 *first,__int128 *last) {
  *first=(__int128)root+begin;*last=(__int128)root+end-1;
  return *first>=0 && *last<=UINT64_MAX;
}
int main(void) {
  const uint64_t roots[]={0,1,3,4,7,8,15,16,23,24,31,32,
    UINT64_C(0x7fffffffffffffff),UINT64_C(0x8000000000000000),
    UINT64_C(0x8000000000000001),UINT64_MAX-32,UINT64_MAX-23,
    UINT64_MAX-8,UINT64_MAX-7,UINT64_MAX-3,UINT64_MAX-1,UINT64_MAX};
  for(unsigned i=0;i<sizeof(roots)/sizeof(*roots);++i)
  for(unsigned j=0;j<sizeof(roots)/sizeof(*roots);++j)
  for(unsigned k=0;k<2;++k) {
    struct {uint64_t before;state_t s;uint64_t after;} box={0},want;
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;s->flags=0x202;s->cr4=0x620;
    s->gpr[0]=UINT64_C(0xfedcba9876543210);s->gpr[4]=roots[i];
    s->gpr[1]=roots[j];s->gpr[2]=k?roots[i]:roots[j];
    s->queries=s->choices=1;s->cursor=19;s->choice_cursor=23;
    s->pc=0xabcdef;s->kind=0x987654;
    __int128 f,l,of,ol,pf,pl;
    int valid=range(s->gpr[4],FB,FE,&f,&l);
    valid&=range(s->gpr[1],OB,OE,&of,&ol);
    valid&=range(s->gpr[2],OB,OE,&pf,&pl);
    valid&=(l<of || ol<f) && (l<pf || pl<f);
    want=box;if(valid) {want.s.pc=s->gpr[0];want.s.kind=2;}
    if(generic_machine_region(MACHINE_ARG(s))!=(valid?0:2))return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
  }
  return 0;
}
)C",
               "#define FB " + SignedLiteral(B[0]) + "\n#define FE " +
                   SignedLiteral(B[1]) + "\n#define OB " + SignedLiteral(B[2]) +
                   "\n#define OE " + SignedLiteral(B[3]) + "\n",
               nullptr, true, true);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       EntryObjectsBindSourceBudgetAndAuthenticatedDomain) {
  auto Image = loadRegion({0x31, 0xc0, 0xff, 0xe0}); // XOR EAX,EAX; JMP RAX
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options =
      kernelPageGlobalOptions(regionOptions(Entry + 2, ExitKind::Jump));
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{16, 24};
  Options.EntryFrameExcludedObjects = {{{x86reg::RCX, 8}, -8, 0},
                                       {{x86reg::RDX, 8}, 0, 8}};
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = entryObjectSource(Result, Options, 3);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  uint64_t Operations = 0;
  for (const auto &Block : Wrapped->Source.Function.Blocks)
    Operations += Block.Ops.size();
  for (uint64_t Limit : {Operations - 1, Operations}) {
    InterpreterMachineRegionSourceOptions SourceOptions;
    SourceOptions.MaxChoiceSourceOperations = Limit;
    auto Bounded = wrapInterpreterMachineRegionChoiceSourceX64V3(
        Result, Options, SourceOptions);
    EXPECT_EQ(bool(Bounded), Limit == Operations);
    if (!Bounded)
      EXPECT_NE(llvm::toString(Bounded.takeError()).find("budget"),
                std::string::npos);
  }
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Changed = Options;
    if (Mutation == 0)
      ++Changed.EntryFrameExcludedObjects[0].Begin;
    else if (Mutation == 1)
      ++Changed.EntryFrameBounds->End;
    else if (Mutation == 2)
      Changed.EntryFrameExcludedObjects[0].Register.Offset = x86reg::R8;
    else
      std::swap(Changed.EntryFrameExcludedObjects[0],
                Changed.EntryFrameExcludedObjects[1]);
    auto Refused = entryObjectSource(Result, Changed, 3);
    EXPECT_FALSE(bool(Refused));
    if (!Refused)
      llvm::consumeError(Refused.takeError());
  }
  auto Ordinary = wrapInterpreterMachineStateX64(Result.Residual);
  EXPECT_FALSE(bool(Ordinary));
  if (!Ordinary)
    llvm::consumeError(Ordinary.takeError());
}