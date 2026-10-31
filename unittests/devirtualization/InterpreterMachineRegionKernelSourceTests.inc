//===- InterpreterMachineRegionKernelSourceTests.inc - Kernel state -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent complete-state oracles for the explicit kernel source.
// Included after the shared machine-region source fixture.

SpecializationOptions kernelPageGlobalOptions(SpecializationOptions Options) {
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  Options.X64CR4PGEState = true;
  return Options;
}

TEST_F(InterpreterMachineRegionSourceTest,
       KernelPageGlobalSourcePreservesCompleteStatesAndReservations) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "kernel source execution requires clang";
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Image = loadRegion(
        {0x41, 0x0f, 0x20, 0xe2, // MOV R10,CR4
         0x49, 0x81,
         uint8_t(Kind == 0   ? 0xca
                 : Kind == 1 ? 0xe2
                             : 0xf2),
         uint8_t(Kind == 1 ? 0x7f : 0x80), uint8_t(Kind == 1 ? 0xff : 0),
         uint8_t(Kind == 1 ? 0xff : 0), uint8_t(Kind == 1 ? 0xff : 0), 0x41,
         0x0f, 0x22, 0xe2, // MOV CR4,R10
         0x0f, 0x20, 0xe0, // MOV RAX,CR4
         0xc3});
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options =
        kernelPageGlobalOptions(regionOptions(Entry + 18, ExitKind::Return));
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    std::vector<uint64_t> ChoiceRegisters;
    for (const auto &Block : Result.Residual.Blocks)
      for (const auto &Op : Block.Ops)
        if (const auto Choice = decodeLowArchitecturalValue(Op))
          ChoiceRegisters.push_back(Choice->Output.Offset);
    const std::vector<uint64_t> ExpectedRegisters{
        x86reg::OF, x86reg::SF, x86reg::ZF, x86reg::AF, x86reg::PF,
        x86reg::CF, x86reg::AF, x86reg::OF, x86reg::SF, x86reg::ZF,
        x86reg::AF, x86reg::PF, x86reg::CF, x86reg::OF, x86reg::SF,
        x86reg::ZF, x86reg::AF, x86reg::PF, x86reg::CF};
    ASSERT_EQ(ChoiceRegisters, ExpectedRegisters);
    auto Wrapped =
        wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    EXPECT_EQ(Wrapped->StateABI, "x64-machine-region-choice-state-v3");
    EXPECT_EQ(Wrapped->StateBytes, 480U);
    EXPECT_EQ(Wrapped->Environment.IdentityDigest,
              Result.RawEnvironment->IdentityDigest);
    Wrapped->Source.Function.Name = "generic_machine_region";
    emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase,cr4;
} state_t;
typedef struct {uint64_t pc,ordinal,bits,value;} record_t;
typedef struct {uint64_t before[2];state_t s;uint64_t after[2];} box_t;
_Static_assert(sizeof(state_t)==480,"kernel region allocation");
static const uint64_t entry=UINT64_C(0x140001000);
static const unsigned starts[]={0,6,7,13},sizes[]={6,1,6,6},pcs[]={0,4,11,15};
/* The bound production order is OF,SF,ZF,AF,PF,CF, not bit-number order. */
static const unsigned bits[]={11,7,6,4,2,0};
static void arbitrary(state_t *s,const record_t *records,unsigned at) {
  for(unsigned i=0;i<6;++i)
    s->flags=(s->flags&~(UINT64_C(1)<<bits[i]))|(records[at+i].value<<bits[i]);
}
static unsigned expected(state_t *s,const record_t *records,const unsigned char *stack) {
  for(unsigned instruction=0;instruction<4;++instruction) {
    unsigned n=sizes[instruction],at=(unsigned)s->choice_cursor;
    if(s->choice_cursor>s->choice_count || n>s->choice_count-s->choice_cursor) {
      s->pc=entry+pcs[instruction];s->kind=0;return 5;
    }
    for(unsigned i=0;i<n;++i) {
      const record_t *r=&records[at+i];
      if(r->pc!=entry+pcs[instruction] || r->ordinal!=i || r->bits!=1 || r->value>1) {
        s->pc=entry+pcs[instruction];s->kind=0;return 6;
      }
    }
    if(instruction==0) {s->gpr[10]=s->cr4;arbitrary(s,records,at);}
    if(instruction==1) {
      uint64_t v=TRANSITION_KIND==0?s->gpr[10]|128:
                 TRANSITION_KIND==1?s->gpr[10]&~UINT64_C(128):s->gpr[10]^128;
      s->gpr[10]=v;
      unsigned parity=0;for(unsigned i=0;i<8;++i)parity^=(unsigned)((v>>i)&1);
      s->flags=(s->flags&~UINT64_C(0x8d5))|((uint64_t)(!parity)<<2)|
               (records[at].value<<4)|((uint64_t)(v==0)<<6)|((v>>63)<<7);
    }
    if(instruction==2) {s->cr4=s->gpr[10];arbitrary(s,records,at);}
    if(instruction==3) {s->gpr[0]=s->cr4;arbitrary(s,records,at);}
    s->choice_cursor+=n;
  }
  memcpy(&s->pc,stack+128,8);s->gpr[4]+=8;s->kind=3;return 0;
}
int main(void) {
  const uint64_t controls[]={0x620,0x30620,0x1000620,0x1800620};
  for(unsigned trial=0;trial<8;++trial)for(unsigned mode=0;mode<96;++mode) {
    unsigned char arena[256],expected_arena[256];
    record_t records[21],expected_records[21];box_t box,want;
    memset(&box,0,sizeof(box));memset(records,0,sizeof(records));
    for(unsigned i=0;i<256;++i)arena[i]=(unsigned char)(i*23+trial);
    box.before[0]=0x12345678;box.before[1]=0x87654321;
    box.after[0]=0xabcdef01;box.after[1]=0xfedcba10;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9182736450000000)+i*73+trial;
      s->xmm[i][0]=UINT64_C(0x76543210fedcba98)+i;
      s->xmm[i][1]=UINT64_C(0x876543219abcdef0)-i-trial;
    }
    s->gpr[4]=(uintptr_t)(arena+128);s->flags=2|((trial*0x394fd)&0x204ed5);
    s->queries=0x12340000;s->count=43;s->cursor=17;
    s->choices=(uintptr_t)records;s->choice_count=21;s->choice_cursor=2;
    s->pc=0xabcdef;s->kind=0xfedcba;s->fsbase=0x12345678000;s->gsbase=0xabcdef000;
    s->cr4=controls[trial/2]|(trial&1?128:0);
    for(unsigned instruction=0;instruction<4;++instruction)
      for(unsigned i=0;i<sizes[instruction];++i) {
        unsigned at=2+starts[instruction]+i;
        records[at]=(record_t){entry+pcs[instruction],i,1,(trial+at*3)&1};
      }
    if(mode>0 && mode<20)s->choice_count=2+mode-1;
    if(mode>=20) {
      unsigned record=2+(mode-20)/4,field=(mode-20)%4;
      if(field==0)records[record].pc^=1;
      if(field==1)records[record].ordinal+=1;
      if(field==2)records[record].bits=2;
      if(field==3)records[record].value=2;
    }
    want=box;memcpy(expected_arena,arena,sizeof(arena));
    memcpy(expected_records,records,sizeof(records));
    unsigned status=expected(&want.s,records,arena);
    if(generic_machine_region(MACHINE_ARG(s))!=status)return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
    if(memcmp(arena,expected_arena,sizeof(arena)))return 3;
    if(memcmp(records,expected_records,sizeof(records)))return 4;
  }
  return 0;
}
)C",
               "#define TRANSITION_KIND " + std::to_string(Kind) + "\n",
               nullptr, true, true);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       KernelPageGlobalSourceUsesSupervisorFlagWrites) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "kernel source execution requires clang";
  for (uint32_t Flags : {2u, 0x202u, 0x204ed7u}) {
    SCOPED_TRACE(Flags);
    auto Image = loadRegion({0x68, uint8_t(Flags), uint8_t(Flags >> 8),
                             uint8_t(Flags >> 16), 0, 0x9d, 0x9c,
                             0x5a,                   // POPFQ; PUSHFQ; POP RDX
                             0xfa, 0x9c, 0x41, 0x58, // CLI; PUSHFQ; POP R8
                             0xfb, 0x9c, 0x41, 0x59, // STI; PUSHFQ; POP R9
                             0xc3});
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options =
        kernelPageGlobalOptions(regionOptions(Entry + 16, ExitKind::Return));
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Wrapped =
        wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options);
    ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
    Wrapped->Source.Function.Name = "generic_machine_region";
    emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase,cr4;
} state_t;
int main(void) {
  for(unsigned trial=0;trial<64;++trial) {
    unsigned char memory[256],want_memory[256];state_t s,want;
    memset(&s,0,sizeof(s));
    for(unsigned i=0;i<256;++i)memory[i]=(unsigned char)(i*13+trial);
    for(unsigned i=0;i<16;++i) {
      s.gpr[i]=UINT64_C(0x76543210abcdef00)+trial+i;
      s.xmm[i][0]=UINT64_C(0x9876543210012345)+i;
      s.xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s.gpr[4]=(uintptr_t)(memory+128);s.flags=2|((trial*0x19ab3)&0x204ed5);
    s.cr4=0x620|(trial&1?128:0);s.fsbase=0x1234567800;s.gsbase=0xabcdef000;
    s.queries=0x111000;s.count=25;s.cursor=7;
    s.choices=0x222000;s.choice_count=31;s.choice_cursor=3;
    want=s;memcpy(want_memory,memory,sizeof(memory));
    want.gpr[2]=POPPED_FLAGS;want.gpr[8]=POPPED_FLAGS&~UINT64_C(512);
    want.gpr[9]=POPPED_FLAGS|512;want.flags=want.gpr[9];
    memcpy(want_memory+120,&want.flags,8);
    memcpy(&want.pc,memory+128,8);want.kind=3;want.gpr[4]+=8;
    if(generic_machine_region(MACHINE_ARG(&s)))return 1;
    if(memcmp(&s,&want,sizeof(s)))return 2;
    if(memcmp(memory,want_memory,sizeof(memory)))return 3;
  }
  return 0;
}
)C",
               "#define POPPED_FLAGS UINT64_C(" + std::to_string(Flags) + ")\n",
               nullptr, true, true);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       KernelPageGlobalSourceRequiresItsExactDomainAndAllocation) {
  auto Image = loadRegion({0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options =
      kernelPageGlobalOptions(regionOptions(Entry, ExitKind::Return));
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (unsigned Case = 0; Case != 8; ++Case) {
    auto Changed = Options;
    if (Case == 0)
      Changed.X64CR4PGEState = false;
    if (Case == 1)
      Changed.X64FlagsProfile =
          InterpreterMachineStateProfile::UserX64NoFaultV1;
    if (Case == 2)
      Changed.X64FlagsProfile =
          InterpreterMachineStateProfile::KernelX64SmapNoFaultV2;
    if (Case == 3)
      Changed.X64CR8State = true;
    if (Case == 4)
      Changed.X64LegacyGSState = true;
    if (Case == 5)
      Changed.X64MXCSRMask = 0xffbf;
    if (Case == 6)
      Changed.NormalNonfaultingExecution = false;
    if (Case == 7)
      Changed.ExplicitMachineState = false;
    auto Wrapped =
        wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Changed);
    EXPECT_FALSE(bool(Wrapped));
    if (!Wrapped)
      llvm::consumeError(Wrapped.takeError());
  }
  auto Old = wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
  EXPECT_FALSE(bool(Old));
  if (!Old)
    llvm::consumeError(Old.takeError());
  InterpreterMachineRegionSourceOptions Limit;
  Limit.MaxChoiceSourceOperations = 1;
  auto Limited =
      wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options, Limit);
  EXPECT_FALSE(bool(Limited));
  if (!Limited)
    llvm::consumeError(Limited.takeError());
}
