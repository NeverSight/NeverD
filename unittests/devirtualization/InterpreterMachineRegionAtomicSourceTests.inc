//===- InterpreterMachineRegionAtomicSourceTests.inc - Atomic source ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte and full-state oracles for authenticated atomics.

TEST_F(InterpreterMachineRegionSourceTest,
       AtomicSourceRequiresAnUnmodifiedSupportedReceipt) {
  for (bool Compare : {false, true}) {
    const std::vector<uint8_t> Code = {
        0xf0, 0x48, 0x0f, uint8_t(Compare ? 0xb1 : 0xc1), 0x01, 0xc3};
    auto Image = loadRegion(Code);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Options = regionOptions(Entry + 5, ExitKind::Return);
    Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 8, {}};
    Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
    const auto Result = specializeBinaryInterpreter(*Image, Entry, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    ASSERT_TRUE(Result.RawEnvironment);
    auto Control = wrapInterpreterMachineRegionSourceX64(Result, Options);
    if (Compare) {
      ASSERT_FALSE(bool(Control));
      EXPECT_NE(llvm::toString(Control.takeError()).find("ordered"),
                std::string::npos);
      continue;
    }
    ASSERT_TRUE(bool(Control)) << llvm::toString(Control.takeError());
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto Changed = Result;
      if (!Mutation)
        Changed.RawEnvironment.reset();
      else
        for (auto &Block : Changed.Residual.Blocks)
          for (auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::ATOMIC_ADD) {
              switch (Mutation) {
              case 1:
                Op.MemoryOrdering = NdMemoryOrdering::None;
                break;
              case 2:
                Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
                break;
              case 3:
                Op.Output.Size = 3;
                break;
              case 4:
                Op.Inputs[0].Size = 4;
                break;
              case 5:
                Op.addInput(NdVar::scalar(1, 8));
                break;
              case 6:
                Op.Opcode = NdOp::ATOMIC_XCHG;
                break;
              }
            }
      auto Wrapped = wrapInterpreterMachineRegionSourceX64(Changed, Options);
      ASSERT_FALSE(bool(Wrapped)) << Mutation;
      llvm::consumeError(Wrapped.takeError());
    }
  }
}

TEST_F(InterpreterMachineRegionSourceTest,
       ScalarAtomicsRetainTransactionsAndPhysicalState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "atomic region source execution requires clang";
  for (bool Add : {false, true})
    for (unsigned Width : {1, 2, 4, 8})
      for (bool Kernel : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Add << ':' << Width << ':' << Kernel);
        std::vector<uint8_t> Code;
        if (Add)
          Code.push_back(0xf0);
        if (Width == 2)
          Code.push_back(0x66);
        if (Width == 8)
          Code.push_back(0x48);
        if (Add)
          Code.push_back(0x0f);
        Code.push_back(Add ? (Width == 1 ? 0xc0 : 0xc1)
                           : (Width == 1 ? 0x86 : 0x87));
        Code.push_back(0x01); // [RCX], AL/AX/EAX/RAX
        const auto Return = Entry + Code.size();
        Code.push_back(0xc3);
        auto Image = loadRegion(Code);
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        auto Options = regionOptions(Return, ExitKind::Return);
        Options.EntryFrameBounds = SpecializationEntryFrameBounds{0, 8, {}};
        Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
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
        const auto Opcode = Add ? NdOp::ATOMIC_ADD : NdOp::ATOMIC_XCHG;
        unsigned Transactions = 0;
        for (const auto &Block : Wrapped->Source.Function.Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Opcode == Opcode) {
              ++Transactions;
              EXPECT_EQ(Op.MemoryOrdering,
                        NdMemoryOrdering::SequentiallyConsistent);
              EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
            }
        EXPECT_EQ(Transactions, 1u);
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
static uint64_t get(const unsigned char *p,unsigned bytes) {
  uint64_t v=0;for(unsigned i=0;i<bytes;++i)v|=(uint64_t)p[i]<<(8*i);return v;
}
static void put(unsigned char *p,uint64_t v,unsigned bytes) {
  for(unsigned i=0;i<bytes;++i)p[i]=(unsigned char)(v>>(8*i));
}
int main(void) {
  const uint64_t mask=UINT64_MAX>>(64-8*WIDTH),sign=UINT64_C(1)<<(8*WIDTH-1);
  const uint64_t values[]={0,1,mask,sign-1,sign,sign+1,0x55aa55aa55aa55aaULL};
  for(unsigned trial=0;trial<7;++trial)for(unsigned offset=0;offset<4;++offset)
  for(unsigned alias=0;alias<2;++alias) {
    _Alignas(64) unsigned char memory[64],wanted[64];
    _Alignas(16) unsigned char stack[128],wanted_stack[128];
    struct {uint64_t before[2];state_t s;uint64_t after[2];} box,want;
    memset(&box,0,sizeof(box));
    box.before[0]=0x12345678;box.before[1]=0xabcdef01;
    box.after[0]=0xfedcba98;box.after[1]=0x87654321;
    for(unsigned i=0;i<64;++i)memory[i]=(unsigned char)(i*29+trial);
    for(unsigned i=0;i<128;++i)stack[i]=(unsigned char)(i*23+offset);
    state_t *s=&box.s;
    for(unsigned i=0;i<16;++i) {
      s->gpr[i]=UINT64_C(0x9123456789abcdef)+31*i+trial;
      s->xmm[i][0]=UINT64_C(0xfedcba9876543210)+i;
      s->xmm[i][1]=UINT64_C(0x8123456789abcdef)-i-trial;
    }
    put(memory+16+offset,values[trial],WIDTH);
    put(stack+40,0x12340,8);
    s->gpr[0]=alias?UINT64_C(0x21000):values[(trial+2)%7];
    s->gpr[1]=(uintptr_t)(alias?stack+40:memory+16+offset);
    s->gpr[4]=(uintptr_t)(stack+40);
    s->flags=2|((trial&1)?0x8d5:0x200)|((uint64_t)(trial&1)<<10);
    s->pc=0xabcdef;s->kind=0x987654;
#if KERNEL
    s->fsbase=0x11223344;s->gsbase=0x55667788;s->cr4=0x620|((trial&1)<<7);
#endif
    memcpy(wanted,memory,sizeof(memory));memcpy(wanted_stack,stack,sizeof(stack));
    want=box;
    unsigned char *at=alias?wanted_stack+40:wanted+16+offset;
    uint64_t old=get(at,WIDTH),input=s->gpr[0]&mask;
    uint64_t value=ADD?(old+input)&mask:input;
    put(at,value,WIDTH);
    want.s.gpr[0]=WIDTH>=4?old:(s->gpr[0]&~mask)|old;
    if(ADD) {
      unsigned parity=0;for(unsigned i=0;i<8;++i)parity^=(unsigned)(value>>i)&1;
      uint64_t flags=(value<old)|((uint64_t)!parity<<2)|
        ((old^input^value)&0x10)|((uint64_t)(value==0)<<6)|
        ((uint64_t)((value&sign)!=0)<<7)|
        ((uint64_t)((~(old^input)&(old^value)&sign)!=0)<<11);
      want.s.flags=(s->flags&~UINT64_C(0x8d5))|flags;
    }
    want.s.gpr[4]+=8;want.s.pc=get(wanted_stack+40,8);want.s.kind=3;
    if(generic_machine_region(MACHINE_ARG(s)))return 1;
    if(memcmp(&box,&want,sizeof(box)))return 2;
    if(memcmp(memory,wanted,sizeof(memory)))return 3;
    if(memcmp(stack,wanted_stack,sizeof(stack)))return 4;
  }
  return 0;
}
)C",
                   "#define KERNEL " + std::to_string(Kernel) +
                       "\n#define ADD " + std::to_string(Add) +
                       "\n#define WIDTH " + std::to_string(Width) + "\n",
                   nullptr, true, true, BinaryFormat::ELF,
                   Add ? "lock xadd" : "xchg");
      }
}
