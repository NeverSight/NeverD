//===- InterpreterMachineRegionStringSourceTests.inc - Runtime direction --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent full-state oracles for bounded repeated transfers.

TEST_F(InterpreterMachineRegionSourceTest,
       RepeatedTransfersRetainRuntimeDirectionAndOverlap) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "repeated-transfer source execution requires clang";
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    const bool Fill = Kind >= 4;
    const unsigned Width = 1u << (Kind % 4);
    for (bool Kernel : {false, true}) {
      for (unsigned Count : {0, 3}) {
        SCOPED_TRACE(::testing::Message()
                     << Kind << ':' << Kernel << ':' << Count);
        std::vector<uint8_t> Code{0xb9, uint8_t(Count), 0, 0,
                                  0}; // MOV ECX,count
        if (Width == 2)
          Code.push_back(0x66);
        Code.push_back(0xf3);
        if (Width == 8)
          Code.push_back(0x48);
        Code.push_back(Fill ? (Width == 1 ? 0xaa : 0xab)
                            : (Width == 1 ? 0xa4 : 0xa5));
        const va_t Return = Entry + Code.size();
        Code.push_back(0xc3);
        auto Image = loadRegion(Code);
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        auto Options = regionOptions(Return, ExitKind::Return);
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
  const int gaps[]={-32,-8,-1,0,1,8,32};
  for(unsigned backward=0;backward<2;++backward)
  for(unsigned g=0;g<7;++g)for(unsigned trial=0;trial<4;++trial) {
    unsigned char memory[256],wanted[256],stack[128],wanted_stack[128];
    box_t box,want;memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0xfedcba98;box.after[1]=0x87654321;
    for(unsigned i=0;i<256;++i)memory[i]=(unsigned char)(i*29+i/7+trial);
    for(unsigned i=0;i<128;++i)stack[i]=(unsigned char)(i*23+trial);
    uint64_t resume=0x12345678;memcpy(stack+64,&resume,8);
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s->xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s->xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial;
    }
    int source=backward?160:64,destination=source+gaps[g];
    s->gpr[4]=(uintptr_t)(stack+64);
    s->gpr[6]=(uintptr_t)(memory+source);
    s->gpr[7]=(uintptr_t)(memory+destination);
    if(!COUNT){s->gpr[6]=UINT64_MAX;s->gpr[7]=0;}
    s->flags=2|((trial&1)?0x8d5:0x200)|((uint64_t)backward<<10);
    s->pc=0xabcdef;s->kind=0x987654;
#if KERNEL
    s->fsbase=0x11223344;s->gsbase=0x55667788;s->cr4=0x620|(trial<<7);
#endif
    memcpy(wanted,memory,sizeof(memory));memcpy(wanted_stack,stack,sizeof(stack));
    want=box;
    for(unsigned i=0;i<COUNT;++i) {
      int delta=(backward?-1:1)*(int)(i*WIDTH);unsigned char element[8];
      for(unsigned b=0;b<WIDTH;++b)
        element[b]=FILL?(unsigned char)(s->gpr[0]>>(8*b)):wanted[source+delta+(int)b];
      for(unsigned b=0;b<WIDTH;++b)wanted[destination+delta+(int)b]=element[b];
    }
    const uint64_t delta=backward?UINT64_C(0)-COUNT*WIDTH:COUNT*WIDTH;
    if(!FILL)want.s.gpr[6]+=delta;
    want.s.gpr[7]+=delta;want.s.gpr[1]=0;want.s.gpr[4]+=8;
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
                       "\n#define FILL " + std::to_string(Fill) +
                       "\n#define COUNT " + std::to_string(Count) +
                       "\n#define WIDTH " + std::to_string(Width) + "\n",
                   nullptr, true, true);
      }
    }
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       RuntimeDirectionDoesNotPermitUnboundedRepeatedCount) {
  for (unsigned Kind = 0; Kind != 8; ++Kind) {
    const unsigned Width = 1u << (Kind % 4);
    std::vector<uint8_t> Code{0xf3};
    if (Width == 2)
      Code.push_back(0x66);
    if (Width == 8)
      Code.push_back(0x48);
    Code.push_back((Kind >= 4 ? 0xaa : 0xa4) + (Width != 1));
    const va_t Return = Entry + Code.size();
    Code.push_back(0xc3);
    auto Image = loadRegion(Code);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options = regionOptions(Return, ExitKind::Return);
    Options.UndefinedModel = SpecializationUndefinedModel::FreshScalarV1;
    Options.X64FlagsProfile =
        InterpreterMachineStateProfile::KernelX64NoFaultV1;
    Options.X64CR4PGEState = true;
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    EXPECT_FALSE(Result.complete());
    EXPECT_NE(Result.Diagnostic.find("proved count"), std::string::npos);
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       BoundedCopyDiscoversPointerAndCountContexts) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "bounded-copy source execution requires clang";
  // A masked count controls a word-copy loop into the caller's local frame.
  // Both the count and the advancing destination remain runtime inputs to
  // the recovered graph; the caller supplies no control-register hints.
  const uint8_t Code[]{
      0x48, 0x8d, 0x64, 0x24, 0xb0, 0x48, 0x8d, 0x7c, 0x24, 0x28, 0x83, 0xe1,
      0x03, 0xe8, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x64, 0x24, 0x50, 0xc3,
      0x48, 0x85, 0xc9, 0x74, 0x1b, 0x66, 0x8b, 0x06, 0x66, 0x89, 0x07, 0x48,
      0x8d, 0x76, 0x02, 0x48, 0x8d, 0x7f, 0x02, 0x48, 0xff, 0xc9, 0x66, 0x85,
      0xc0, 0x74, 0x05, 0x48, 0x85, 0xc9, 0x75, 0xe5, 0xc3};
  auto Image = loadRegion(Code);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Options = regionOptions(Entry + 0x17, ExitKind::Return);
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 8, {}};
  Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  Options.ProveStoreFrameSeparation = Options.ProveStoreReturnSlotSeparation =
      true;
  Options.DiscoverControlState = true;
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
  const uint64_t counts[]={0,1,2,UINT64_MAX};
  const int gaps[]={-2,0,2,72};
  for(unsigned c=0;c<4;++c)for(unsigned g=0;g<4;++g)
  for(unsigned zero=0;zero<4;++zero)for(unsigned trial=0;trial<2;++trial) {
    _Alignas(16) unsigned char stack[256],wanted[256];
    state_t s,want;memset(&s,0,sizeof(s));
    for(unsigned i=0;i<256;++i)stack[i]=(unsigned char)(i*23+trial);
    for(unsigned i=0;i<16;++i) {
      s.gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s.xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s.xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial;
    }
    int source=96+gaps[g];
    for(unsigned i=0;i<4;++i) {
      uint16_t value=i==zero?0:(uint16_t)(0x8123+i);
      memcpy(stack+source+2*i,&value,2);
    }
    uint64_t resume=0x12340;memcpy(stack+136,&resume,8);
    s.gpr[4]=(uintptr_t)(stack+136);s.gpr[6]=(uintptr_t)(stack+source);
    s.gpr[1]=counts[c];s.flags=2|0x8d5|((uint64_t)trial<<10);
    s.pc=0xabcdef;s.kind=0x987654;
    want=s;memcpy(wanted,stack,sizeof(stack));
    uint64_t internal=UINT64_C(0x140001012);memcpy(wanted+48,&internal,8);
    unsigned left=(unsigned)(counts[c]&3);int read=source,write=96;
    while(left) {
      uint16_t value;memcpy(&value,wanted+read,2);
      memcpy(wanted+write,&value,2);
      want.gpr[0]=(want.gpr[0]&~UINT64_C(65535))|value;
      read+=2;write+=2;--left;if(!value)break;
    }
    want.gpr[1]=left;want.gpr[4]+=8;
    want.gpr[6]=(uintptr_t)(stack+read);want.gpr[7]=(uintptr_t)(stack+write);
    /* Selected TEST/AND semantics retain AF. A nonempty loop's DEC sets it
       to zero for counts 1..3 before the final TEST. */
    want.flags=(s.flags&~UINT64_C(0x8d5))|0x44|
               ((counts[c]&3)?0:(s.flags&0x10));
    want.pc=resume;want.kind=3;
    if(generic_machine_region(MACHINE_ARG(&s)))return 1;
    if(memcmp(&s,&want,sizeof(s))) {
      for(unsigned i=0;i<sizeof(s);i+=8) {
        uint64_t got,expected;memcpy(&got,(char *)&s+i,8);
        memcpy(&expected,(char *)&want+i,8);
        if(got!=expected)fprintf(stderr,"case %u/%u/%u/%u offset %u: %llx != %llx\n",
          c,g,zero,trial,i,(unsigned long long)got,(unsigned long long)expected);
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
