//===- InterpreterMachineRegionChoiceSourceTests.inc - Choice execution ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte/state oracles for runtime architectural choices.

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
  for(unsigned a=0;a<2;++a) for(unsigned b=0;b<2;++b) {
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
    s->choices=(uintptr_t)tape;s->choice_count=2;
    expected=box;memcpy(expected_stack,stack,sizeof(stack));
    memcpy(expected_data,data,sizeof(data));memcpy(expected_tape,tape,sizeof(tape));
    expected.s.gpr[0]=0;expected.s.gpr[1]=0;
    expected.s.gpr[2]=expected.s.gpr[3]=0x246|(b<<4);
    expected_stack[3]=expected.s.gpr[2];
    expected_data[1]=0x246|(a<<4);expected_data[2]=0x246|(b<<4);
    expected.s.gpr[7]+=16;expected.s.gpr[4]+=8;
    expected.s.flags=0x246;expected.s.pc=0x12340;expected.s.kind=3;
    expected.s.choice_cursor=2;
    if(generic_machine_region(MACHINE_ARG(s))) return 1;
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
