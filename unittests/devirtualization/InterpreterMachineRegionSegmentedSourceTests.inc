//===- InterpreterMachineRegionSegmentedSourceTests.inc - Base inputs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte/state oracles for explicit segmented source inputs.
// Included after the shared machine-region source fixture.

TEST_F(InterpreterMachineRegionSourceTest,
       SegmentedChoiceSourcePreservesByteAliasesAndReturnCells) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "segmented source execution requires clang";
  auto Image =
      loadRegion({0x65, 0x48, 0x8b, 0x01,          // MOV RAX,GS:[RCX]
                  0x64, 0x48, 0x89, 0x42, 1,       // MOV FS:[RDX+1],RAX
                  0x65, 0x66, 0x89, 0x5a, 3,       // MOV GS:[RDX+3],BX
                  0x64, 0x88, 0x5a, 7,             // MOV FS:[RDX+7],BL
                  0x65, 0x89, 0x5a, 9,             // MOV GS:[RDX+9],EBX
                  0x64, 0x44, 0x0f, 0xb6, 0x41, 2, // MOVZX R8D,byte FS:[RCX+2]
                  0x65, 0x44, 0x0f, 0xb7, 0x51, 3, // MOVZX R10D,word GS:[RCX+3]
                  0x65, 0x44, 0x8b, 0x49, 1,       // MOV R9D,dword GS:[RCX+1]
                  0x4c, 0x8b, 0x1e,                // MOV R11,[RSI]
                  0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 42, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.X64SegmentedReads = Options.X64SegmentedWrites = true;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  EXPECT_EQ(Wrapped->StateABI, "x64-machine-region-choice-state-v2");
  EXPECT_EQ(Wrapped->StateBytes, 472U);
  EXPECT_EQ(Wrapped->Environment.IdentityDigest,
            Result.RawEnvironment->IdentityDigest);
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase;
} state_t;
_Static_assert(sizeof(state_t)==472,"segmented region layout");
static uint64_t get(const unsigned char *p,unsigned n) {
  uint64_t v=0;for(unsigned i=0;i<n;++i)v|=(uint64_t)p[i]<<(8*i);return v;
}
static void put(unsigned char *p,uint64_t v,unsigned n) {
  for(unsigned i=0;i<n;++i)p[i]=(unsigned char)(v>>(8*i));
}
int main(void) {
  const int differences[]={0,-7,3,24},deltas[]={0,-8,8,16};
  for(unsigned trial=0;trial<80;++trial) {
    unsigned char arena[512],expected_arena[512];
    struct {uint64_t before[2];state_t s;uint64_t after[2];} box,expected;
    memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0x11223344;box.after[1]=0x55667788;
    state_t *s=&box.s;
    for(unsigned i=0;i<512;++i)arena[i]=(unsigned char)(i*19+trial*7);
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9a76543210000000)+i*31+trial;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-trial-i;
    }
    unsigned gs=248+(trial%16),fs=gs+differences[(trial/4)%4];
    const uint64_t offsets[]={0,UINT64_C(0x7ffffffffffffff0),
      UINT64_C(0xfffffff000000000),UINT64_MAX-17,(uintptr_t)(arena+gs)};
    uint64_t offset=offsets[trial/16];
    int delta=deltas[trial%4];
    s->fsbase=(uintptr_t)(arena+fs)-offset;
    s->gsbase=(uintptr_t)(arena+gs)-offset;
    s->gpr[1]=offset;s->gpr[2]=offset+delta;
    s->gpr[4]=(uintptr_t)(arena+256);s->gpr[6]=(uintptr_t)(arena+257);
    s->flags=2|(trial&1?0xcd5:0x244);s->pc=0xabcdef;s->kind=0x987654;
    memcpy(expected_arena,arena,sizeof(arena));expected=box;
    expected.s.gpr[0]=get(expected_arena+gs,8);
    put(expected_arena+fs+delta+1,expected.s.gpr[0],8);
    put(expected_arena+gs+delta+3,s->gpr[3],2);
    put(expected_arena+fs+delta+7,s->gpr[3],1);
    put(expected_arena+gs+delta+9,s->gpr[3],4);
    expected.s.gpr[8]=get(expected_arena+fs+2,1);
    expected.s.gpr[10]=get(expected_arena+gs+3,2);
    expected.s.gpr[9]=get(expected_arena+gs+1,4);
    expected.s.gpr[11]=get(expected_arena+257,8);
    expected.s.pc=get(expected_arena+256,8);expected.s.kind=3;
    expected.s.gpr[4]+=8;
    if(generic_machine_region(MACHINE_ARG(s)))return 1;
    if(memcmp(&box,&expected,sizeof(box)))return 2;
    if(memcmp(arena,expected_arena,sizeof(arena)))return 3;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       SegmentedChoiceSourceKeepsInstructionAndQueryReservations) {
  // Dynamic byte SHL reserves AF/OF/CF even when a runtime guard is false.
  // Reject an incomplete reservation before its memory read-modify-write.
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "segmented source execution requires clang";
  auto Image = loadRegion({0x64, 0xc6, 0x07, 0x5a,    // MOV byte FS:[RDI],0x5a
                           0x31, 0xc0,                // XOR EAX,EAX
                           0xb9, 3,    0,    0,    0, // MOV ECX,3
                           0x0f, 0xa2,                // CPUID
                           0x65, 0x48, 0x89, 0x47, 1, // MOV GS:[RDI+1],RAX
                           0x64, 0xd2, 0x67, 2,       // SHL byte FS:[RDI+2],CL
                           0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 22, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.X64SegmentedReads = Options.X64SegmentedWrites = true;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {uint64_t pc,ordinal,bits,value;} choice_t;
typedef struct {uint32_t leaf,subleaf,answer[4];} query_t;
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase;
} state_t;
int main(void) {
  const unsigned counts[]={0,1,2,7,8,31,32,33};
  for(unsigned trial=0;trial<8;++trial)for(unsigned bits=0;bits<16;++bits)
  for(unsigned mode=0;mode<9;++mode) {
    unsigned shift=counts[trial],count=shift&31;
    unsigned char arena[64],expected_arena[64];
    uint64_t stack[8],expected_stack[8];
    choice_t choices[6]={{0x1111,9,2,3},{0x2222,8,2,2},
      {UINT64_C(0x140001004),0,1,bits&1},
      {UINT64_C(0x140001012),0,1,(bits>>1)&1},
      {UINT64_C(0x140001012),1,1,(bits>>2)&1},
      {UINT64_C(0x140001012),2,1,(bits>>3)&1}},expected_choices[6];
    query_t queries[2]={{77,88,{1,2,3,4}},
                       {0,3,{0x88776655,0x99,shift,0xdd}}},expected_queries[2];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    memset(&box,0,sizeof(box));memset(arena,0xa5,sizeof(arena));
    memset(stack,0x5a,sizeof(stack));stack[4]=0x12340;
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][0]=UINT64_C(0x123456789abcdef0)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-i;
    }
    s->gpr[4]=(uintptr_t)&stack[4];s->gpr[7]=UINT64_MAX-31;
    s->fsbase=s->gsbase=(uintptr_t)(arena+32)-s->gpr[7];
    s->flags=0xa03;s->pc=0x987654;s->kind=0xabcdef;
    s->choices=(uintptr_t)choices;s->choice_count=6;s->choice_cursor=2;
    s->queries=(uintptr_t)queries;s->count=2;s->cursor=1;
    if(mode==1)s->choice_count=2;
    if(mode==2)++choices[2].pc;
    if(mode==3)s->count=1;
    if(mode==4)++queries[1].subleaf;
    if(mode==5)s->choice_count=5;
    if(mode==6)++choices[5].ordinal;
    if(mode==7)choices[5].value=2;
    if(mode==8)choices[5].bits=2;
    expected=box;memcpy(expected_arena,arena,sizeof(arena));
    memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_choices,choices,sizeof(choices));
    memcpy(expected_queries,queries,sizeof(queries));
    expected_arena[32]=0x5a;
    expected.s.pc=UINT64_C(0x140001004);expected.s.kind=0;
    uint64_t status=mode==1?5:mode==2?6:0;
    if(mode!=1&&mode!=2) {
      expected.s.gpr[0]=0;expected.s.gpr[1]=3;
      expected.s.flags=0x246|((bits&1)<<4);expected.s.choice_cursor=3;
      expected.s.pc=UINT64_C(0x14000100b);
      if(mode==3||mode==4)status=mode==3?3:4;
      else {
        expected.s.cursor=2;
        expected.s.gpr[0]=0x88776655;expected.s.gpr[3]=0x99;
        expected.s.gpr[1]=shift;expected.s.gpr[2]=0xdd;
        for(unsigned i=0;i<8;++i)
          expected_arena[33+i]=(unsigned char)(expected.s.gpr[0]>>(8*i));
        expected.s.pc=UINT64_C(0x140001012);
        if(mode>=5)status=mode==5?5:6;
        else {
          unsigned value=expected_arena[34];
          if(count) {
            unsigned carry=count>=8?(bits>>3)&1:(value>>(8-count))&1;
            unsigned result=count>=8?0:(value<<count)&255,parity=0;
            for(unsigned i=0;i<8;++i)parity^=(result>>i)&1;
            unsigned overflow=count==1?((result>>7)^carry):(bits>>2)&1;
            expected.s.flags=0x202|carry|((parity^1)<<2)|
              (((bits>>1)&1)<<4)|((result==0)<<6)|((result>>7)<<7)|(overflow<<11);
            expected_arena[34]=(unsigned char)result;
          }
          expected.s.choice_cursor=6;expected.s.gpr[4]+=8;
          expected.s.pc=0x12340;expected.s.kind=3;
        }
      }
    }
    if(generic_machine_region(MACHINE_ARG(s))!=status)return 1;
    if(memcmp(&box,&expected,sizeof(box)))return 2;
    if(memcmp(arena,expected_arena,sizeof(arena)))return 3;
    if(memcmp(stack,expected_stack,sizeof(stack)))return 4;
    if(memcmp(choices,expected_choices,sizeof(choices)))return 5;
    if(memcmp(queries,expected_queries,sizeof(queries)))return 6;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       SegmentedChoiceSourceAddsBasesAfterNarrowOffsetWrapping) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "segmented source execution requires clang";
  // The constant displacement and the address-size override both reach
  // ordinary guest bytes. High GPR halves must not enter the 32-bit offset.
  auto Image = loadRegion(
      {0x65, 0x48, 0x8b, 0x04, 0x25, 0x38, 1, 0, 0, // MOV RAX,GS:[0x138]
       0x64, 0x67, 0x4c, 0x8b, 0x44, 0xcb, 7,       // MOV R8,FS:[EBX+ECX*8+7]
       0x65, 0x67, 0x4c, 0x89, 0x44, 0xca, 3,       // MOV GS:[EDX+ECX*8+3],R8
       0xc3});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 23, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.X64SegmentedReads = Options.X64SegmentedWrites = true;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase;
} state_t;
static uint64_t get(const unsigned char *p) {
  uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i);return v;
}
int main(void) {
  const uint32_t indexes[]={0,1,0x20000000,0xffffffff};
  for(unsigned trial=0;trial<16;++trial) {
    unsigned char arena[256],expected_arena[256];
    uint64_t stack[8],expected_stack[8];
    struct {uint64_t before;state_t s;uint64_t after;} box,expected;
    memset(&box,0,sizeof(box));memset(stack,0x5a,sizeof(stack));
    box.before=0x12345678;box.after=0xabcdef01;
    state_t *s=&box.s;
    for(unsigned i=0;i<256;++i)arena[i]=(unsigned char)(i*19+trial*7);
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9a76543210000000)+i*31+trial;
      s->xmm[i][0]=UINT64_C(0x9123456789abcdef)+i;
      s->xmm[i][1]=UINT64_C(0xfedcba9876543210)-trial-i;
    }
    uint32_t index=indexes[trial%4],base=0xfffffff8u+trial/4;
    uint32_t offset=base+index*8+7,target=0x120u-index*8;
    s->gpr[1]=UINT64_C(0xabcdef1200000000)|index;
    s->gpr[2]=UINT64_C(0x9876543200000000)|target;
    s->gpr[3]=UINT64_C(0xfedcba9800000000)|base;
    s->gpr[4]=(uintptr_t)&stack[4];stack[4]=0x12340;
    s->fsbase=(uintptr_t)(arena+64)-offset;
    s->gsbase=(uintptr_t)(arena+80)-0x138;
    s->flags=0xad7;s->pc=0xabcdef;s->kind=0x987654;
    expected=box;memcpy(expected_arena,arena,sizeof(arena));
    memcpy(expected_stack,stack,sizeof(stack));
    expected.s.gpr[0]=get(arena+80);expected.s.gpr[8]=get(arena+64);
    for(unsigned i=0;i<8;++i)expected_arena[59+i]=arena[64+i];
    expected.s.gpr[4]+=8;expected.s.pc=0x12340;expected.s.kind=3;
    if(generic_machine_region(MACHINE_ARG(s)))return 1;
    if(memcmp(&box,&expected,sizeof(box)))return 2;
    if(memcmp(arena,expected_arena,sizeof(arena)))return 3;
    if(memcmp(stack,expected_stack,sizeof(stack)))return 4;
  }
  return 0;
}
)C",
             {}, nullptr, true, true);
}

TEST_F(InterpreterMachineRegionSourceTest,
       SegmentedChoiceSourceAuthenticatesEnvironmentAndPreservesV1) {
  auto Image = loadRegion({0x65, 0x48, 0x8b, 0x04, 0x25, 0x38, 1, 0, 0,
                           0xc3}); // MOV RAX,GS:[0x138]; RET
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 9, ExitKind::Return);
  Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
  Options.X64SegmentedReads = true;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto V2 = wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
  ASSERT_TRUE(bool(V2)) << llvm::toString(V2.takeError());
  auto V1 = wrapInterpreterMachineRegionChoiceSourceX64V1(Result, Options);
  EXPECT_FALSE(bool(V1));
  if (!V1)
    EXPECT_NE(llvm::toString(V1.takeError()).find("source environment"),
              std::string::npos);
  const auto Refused = [](const SpecializationResult &Candidate,
                          const SpecializationOptions &Expected) {
    auto Source =
        wrapInterpreterMachineRegionChoiceSourceX64V2(Candidate, Expected);
    EXPECT_FALSE(bool(Source));
    if (!Source)
      EXPECT_FALSE(llvm::toString(Source.takeError()).empty());
  };
  auto Wrong = Options;
  Wrong.X64SegmentedReads = false;
  Refused(Result, Wrong);
  Wrong = Options;
  Wrong.X64SegmentedWrites = true;
  Refused(Result, Wrong);
  auto Changed = Result;
  Changed.RawEnvironment.reset();
  Refused(Changed, Options);
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    Changed = Result;
    bool Found = false;
    for (auto &Block : Changed.Residual.Blocks)
      for (auto &Op : Block.Ops)
        if (!Found && Op.MemoryAddressSpace == NdMemoryAddressSpace::X86GS) {
          if (Kind == 0)
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          else if (Kind == 1)
            Op.Output.Size = 4;
          else
            Op.Inputs[Op.NumInputs >= 2 ? 1 : 0].Size = 4;
          Found = true;
        }
    ASSERT_TRUE(Found);
    Refused(Changed, Options);
  }
  InterpreterMachineRegionSourceOptions Budget;
  Budget.MaxChoiceSourceOperations = 1;
  auto Bounded =
      wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options, Budget);
  EXPECT_FALSE(bool(Bounded));
  if (!Bounded)
    EXPECT_NE(llvm::toString(Bounded.takeError()).find("budget exhausted"),
              std::string::npos);
}

TEST_F(InterpreterMachineRegionSourceTest,
       SegmentedChoiceSourceStillRefusesOtherArchitecturalBanks) {
  auto Image = loadRegion({0xff, 0xd0});
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Options = regionOptions(Entry, ExitKind::Call);
    Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
    Options.X64SegmentedReads = Options.X64SegmentedWrites = true;
    if (Kind == 0)
      Options.X64MXCSRMask = 0xffbf;
    else {
      Options.X64FlagsProfile =
          InterpreterMachineStateProfile::KernelX64NoFaultV1;
      if (Kind == 1)
        Options.X64LegacyGSState = true;
      else
        Options.X64CR8State = true;
    }
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Source =
        wrapInterpreterMachineRegionChoiceSourceX64V2(Result, Options);
    EXPECT_FALSE(bool(Source));
    if (!Source)
      EXPECT_NE(llvm::toString(Source.takeError()).find("source environment"),
                std::string::npos);
  }
}
