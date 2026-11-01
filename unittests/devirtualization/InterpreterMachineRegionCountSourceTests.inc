//===- InterpreterMachineRegionCountSourceTests.inc - Runtime counts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Full-state C oracles for finite live repeated-transfer counts.

TEST_F(InterpreterMachineRegionSourceTest,
       FiniteRepeatedCountsPreserveFullState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "finite-count source execution requires clang";
  for (bool Fill : {false, true})
    for (bool Kernel : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Fill << ':' << Kernel);
      // MOV R10D,7; CMP RCX,R10; CMOVA RCX,R10; REP MOVSW/STOSW; RET.
      // No entry hint fixes RCX, and the dispatch cut is a fallthrough.
      const uint8_t Code[]{
          0x41, 0xba, 7,    0,    0,    0,    0x4c, 0x39,
          0xd1, 0x49, 0x0f, 0x47, 0xca, 0xf3, 0x66, uint8_t(Fill ? 0xab : 0xa5),
          0xc3};
      auto Image = loadRegion(Code);
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Options = regionOptions(Entry + sizeof(Code) - 1, ExitKind::Return);
      Options.DiscoverControlState = true;
      Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 8, {}};
      Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
      Options.ProveStoreFrameSeparation =
          Options.ProveStoreReturnSlotSeparation = true;
      if (Kernel) {
        Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
        Options.X64FlagsProfile =
            InterpreterMachineStateProfile::KernelX64NoFaultV1;
        Options.X64CR4PGEState = true;
      }
      const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      auto Wrapped =
          Kernel
              ? wrapInterpreterMachineRegionChoiceSourceX64V3(Result, Options)
              : wrapInterpreterMachineRegionSourceX64(Result, Options);
      ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
      Wrapped->Source.Function.Name = "generic_machine_region";
      emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
#if KERNEL
  uint64_t choices,choice_count,choice_cursor,fsbase,gsbase,cr4;
#endif
} state_t;
typedef struct {uint64_t before[2];state_t s;uint64_t after[2];} box_t;
int main(void) {
  const uint64_t counts[]={0,1,2,3,4,5,6,7,8,UINT64_MAX,UINT64_C(1)<<63};
  const int gaps[]={-32,-8,-1,0,1,8,32};
  for(unsigned c=0;c<11;++c)for(unsigned backward=0;backward<2;++backward)
  for(unsigned g=0;g<7;++g)for(unsigned trial=0;trial<2;++trial) {
    unsigned char memory[256],wanted[256],wanted_stack[128];
    _Alignas(16) unsigned char stack[128];
    box_t box,want;memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0xfedcba98;box.after[1]=0x87654321;
    for(unsigned i=0;i<256;++i)memory[i]=(unsigned char)(i*29+i/7+trial);
    for(unsigned i=0;i<128;++i)stack[i]=(unsigned char)(i*23+trial);
    uint64_t resume=0x12345678;memcpy(stack+72,&resume,8);
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s->xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s->xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial;
    }
    int source=backward?160:64,destination=source+gaps[g];
    uint64_t count=counts[c]>7?7:counts[c];
    s->gpr[1]=counts[c];s->gpr[4]=(uintptr_t)(stack+72);
    s->gpr[6]=(uintptr_t)(memory+source);s->gpr[7]=(uintptr_t)(memory+destination);
    if(!count){s->gpr[6]=UINT64_MAX;s->gpr[7]=0;}
    s->flags=2|((trial&1)?0x8d5:0x200)|((uint64_t)backward<<10);
    s->pc=0xabcdef;s->kind=0x987654;
#if KERNEL
    s->fsbase=0x11223344;s->gsbase=0x55667788;s->cr4=0x620|(trial<<7);
#endif
    memcpy(wanted,memory,sizeof(memory));memcpy(wanted_stack,stack,sizeof(stack));
    want=box;
    for(unsigned i=0;i<count;++i) {
      int delta=(backward?-1:1)*(int)(i*2);unsigned char element[2];
      for(unsigned b=0;b<2;++b)
        element[b]=FILL?(unsigned char)(s->gpr[0]>>(8*b)):wanted[source+delta+(int)b];
      for(unsigned b=0;b<2;++b)wanted[destination+delta+(int)b]=element[b];
    }
    uint64_t difference=counts[c]-7,arithmetic=counts[c]<7;
    unsigned parity=0;for(unsigned bit=0;bit<8;++bit)parity^=(difference>>bit)&1;
    arithmetic|=(uint64_t)!parity<<2;
    arithmetic|=((counts[c]^7^difference)&16);
    arithmetic|=(uint64_t)(difference==0)<<6;
    arithmetic|=(difference>>63)<<7;
    arithmetic|=(((counts[c]^7)&(counts[c]^difference))>>63)<<11;
    want.s.flags=(s->flags&~UINT64_C(0x8d5))|arithmetic;
    const uint64_t delta=backward?UINT64_C(0)-count*2:count*2;
    if(!FILL)want.s.gpr[6]+=delta;
    want.s.gpr[7]+=delta;want.s.gpr[1]=0;want.s.gpr[10]=7;want.s.gpr[4]+=8;
    want.s.pc=resume;want.s.kind=3;
    if(generic_machine_region(MACHINE_ARG(s)))return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
    if(memcmp(memory,wanted,sizeof(memory)))return 3;
    if(memcmp(stack,wanted_stack,sizeof(stack)))return 4;
  }
  return 0;
}
)C",
                 "#define KERNEL " + std::to_string(Kernel) +
                     "\n#define FILL " + std::to_string(Fill) + "\n",
                 nullptr, true, true);
    }
}

TEST_F(InterpreterMachineRegionSourceTest,
       FiniteCopyAndPaddingPreserveLoopState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "finite-count loop source execution requires clang";
  // AND RCX,7; reserve a local frame; copy words until zero or count
  // exhaustion; after a zero word, DEC RCX; XOR EAX,EAX; REP STOSW; restore
  // RSP; RET. LEA reserves/restores the frame without changing the final
  // logical flags.
  const uint8_t Code[]{
      0x48, 0x83, 0xe1, 0x07, 0x48, 0x8d, 0x64, 0x24, 0xa0, 0x48, 0x8d, 0x7c,
      0x24, 0x20, 0x48, 0x85, 0xc9, 0x74, 0x22, 0x66, 0x8b, 0x06, 0x66, 0x89,
      0x07, 0x48, 0x8d, 0x7f, 0x02, 0x66, 0x85, 0xc0, 0x74, 0x0b, 0x48, 0x8d,
      0x76, 0x02, 0x48, 0xff, 0xc9, 0x75, 0xe8, 0xeb, 0x08, 0x48, 0xff, 0xc9,
      0x31, 0xc0, 0xf3, 0x66, 0xab, 0x48, 0x8d, 0x64, 0x24, 0x60, 0xc3};
  auto Image = loadRegion(Code);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + sizeof(Code) - 1, ExitKind::Return);
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 8, {}};
  Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  Options.ProveStoreFrameSeparation = Options.ProveStoreReturnSlotSeparation =
      true;
  Options.DiscoverControlState = true;
  Options.StopChainingAtRepeatedDestination = true;
  Options.MaxChainedTransfers = 512;
  Options.MaxSolverQueries = 16384;
  const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Wrapped = wrapInterpreterMachineRegionSourceX64(Result, Options);
  ASSERT_TRUE(bool(Wrapped)) << llvm::toString(Wrapped.takeError());
  Wrapped->Source.Function.Name = "generic_machine_region";
  emitAndRun(Wrapped->Source, R"C(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  uint64_t gpr[16],flags,queries,count,cursor,xmm[16][2],pc,kind;
} state_t;
int main(void) {
  const uint64_t counts[]={0,1,2,3,4,5,6,7,8,UINT64_MAX};
  const int gaps[]={-32,-2,0,2,72};
  for(unsigned c=0;c<10;++c)for(unsigned g=0;g<5;++g)
  for(unsigned zero=0;zero<8;++zero)for(unsigned backward=0;backward<2;++backward)
  for(unsigned trial=0;trial<2;++trial) {
    _Alignas(16) unsigned char stack[512],wanted[512];
    state_t s,want;memset(&s,0,sizeof(s));
    for(unsigned i=0;i<512;++i)stack[i]=(unsigned char)(i*23+trial);
    for(unsigned i=0;i<16;++i) {
      s.gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s.xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s.xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial;
    }
    int source=200+gaps[g];
    for(unsigned i=0;i<8;++i) {
      uint16_t value=i==zero?0:(uint16_t)(0x8123+i);
      memcpy(stack+source+2*i,&value,2);
    }
    uint64_t resume=0x12340;memcpy(stack+264,&resume,8);
    s.gpr[4]=(uintptr_t)(stack+264);s.gpr[6]=(uintptr_t)(stack+source);
    s.gpr[1]=counts[c];s.flags=2|(trial?0x8d5:0x200)|((uint64_t)backward<<10);
    s.pc=0xabcdef;s.kind=0x987654;
    want=s;memcpy(wanted,stack,sizeof(stack));
    unsigned left=(unsigned)(counts[c]&7);int read=source,write=200;
    while(left) {
      uint16_t value;memcpy(&value,wanted+read,2);
      memcpy(wanted+write,&value,2);write+=2;--left;
      want.gpr[0]=(want.gpr[0]&~UINT64_C(65535))|value;
      if(!value) {
        want.gpr[0]=0;
        while(left){wanted[write]=wanted[write+1]=0;write+=backward?-2:2;--left;}
        break;
      }
      read+=2;
    }
    want.gpr[1]=0;want.gpr[4]+=8;
    want.gpr[6]=(uintptr_t)(stack+read);want.gpr[7]=(uintptr_t)(stack+write);
    want.flags=(s.flags&~UINT64_C(0x8d5))|0x44|
               ((counts[c]&7)?0:(s.flags&0x10));
    want.pc=resume;want.kind=3;
    if(generic_machine_region(MACHINE_ARG(&s)))return 1;
    if(memcmp(&s,&want,sizeof(s))) {
      for(unsigned i=0;i<sizeof(s);i+=8) {
        uint64_t got,expected;memcpy(&got,(char *)&s+i,8);
        memcpy(&expected,(char *)&want+i,8);
        if(got!=expected)fprintf(stderr,"case %u/%u/%u/%u/%u offset %u: %llx != %llx\n",
          c,g,zero,backward,trial,i,(unsigned long long)got,(unsigned long long)expected);
      }
      return 2;
    }
    if(memcmp(stack,wanted,sizeof(stack)))return 3;
  }
  return 0;
}
)C",
             "", nullptr, true, true);
}
