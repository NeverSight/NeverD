//===- X64FarPointerContinuationTests.inc - Far-load state oracles --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Included by X64ContinuationSourceTests.cpp to share its source/KVM runner.
//===----------------------------------------------------------------------===//

struct FarPointerFixture {
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::vector<std::vector<uint8_t>> Commands;

  void add(std::vector<uint8_t> Command) {
    Entries.push_back(0x1000 + Bytes.size());
    Bytes.insert(Bytes.end(), Command.begin(), Command.end());
    Bytes.resize(Entries.size() * 16, 0x90);
    Commands.push_back(std::move(Command));
  }

  void addLoad(uint8_t Opcode, unsigned Width, unsigned Register = 0) {
    std::vector<uint8_t> Code;
    if (Width == 2)
      Code.push_back(0x66);
    if (Width == 8 || Register >= 8)
      Code.push_back(0x40 | (Width == 8 ? 8 : 0) | (Register >= 8 ? 4 : 0));
    Code.insert(Code.end(), {0x0f, Opcode, uint8_t((Register & 7) << 3 | 3)});
    add(std::move(Code));
  }

  std::string harness() const {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    OS << EventHarness;
    stepDispatcher(OS, Entries);
    OS << "static const unsigned char commands[][16]={\n";
    for (const auto &Command : Commands) {
      OS << "{" << Command.size();
      for (uint8_t B : Command)
        OS << "," << unsigned(B);
      OS << "},\n";
    }
    OS << "};\n";
    return Text;
  }
};

constexpr char FarPointerNativeCompare[] = R"C(
static void compare_far(unsigned index,event_state_t s) {
  unsigned char before[4096],after[4096];
  unsigned char alias_before[64],alias_after[64];
  s.pc=0x1000+index*16;event_state_t want=s;
  memcpy(before,(void *)0x8000,4096);
  memcpy(alias_before,(void *)0x210000,64);
  __asm__ volatile("outb %%al,$0xe9"::"a"(7),"D"(&want),"S"(commands[index]):"memory");
  memcpy(after,(void *)0x8000,4096);memcpy((void *)0x8000,before,4096);
  memcpy(alias_after,(void *)0x210000,64);memcpy((void *)0x210000,alias_before,64);
  CHECK(step(index,ARG(&s))==0);
  CHECK(!memcmp(&s,&want,sizeof(s)));
  CHECK(!memcmp((void *)0x8000,after,4096));
  CHECK(!memcmp((void *)0x210000,alias_after,64));
}
)C";

TEST_F(X64ContinuationSourceTest,
       FarPointerMatchesNativeRegistersAndDescriptors) {
  for (unsigned Width : {2u, 4u, 8u}) {
    SCOPED_TRACE(Width);
    FarPointerFixture F;
    for (bool GS : {false, true})
      for (unsigned Reg = 0; Reg != 16; ++Reg)
        F.addLoad(GS ? 0xb5 : 0xb4, Width, Reg);
    std::string Harness = F.harness() + FarPointerNativeCompare;
    Harness += "static const unsigned width=" + std::to_string(Width) + ";\n";
    Harness += R"C(
int main(void) {
  const unsigned types[]={0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x9a,0x9b,0x9e,0x9f};
  for(unsigned i=0;i<32;++i)for(unsigned local=0;local<2;++local)
    for(unsigned type=0;type<12;++type)for(unsigned dpl=0;dpl<4;++dpl)
      for(unsigned rpl=0;rpl<4;++rpl) {
        if((types[type]&12)!=12 && rpl>dpl)continue;
        event_state_t s=event_seed(0x8000);
        uint16_t selector=(local?4:32)|rpl;
        uint64_t table=local?s.ldt.base:s.gdt.base;
        uint64_t descriptor=0x89c100efabcd2345ULL|((uint64_t)(types[type]|dpl<<5)<<40);
        event_word(table+(selector&~7),descriptor);
        s.gpr[3]=0x8801;event_word(s.gpr[3],0xfedcba9876543210ULL);
        memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
        s.nmi_blocked=rpl&1;s.unusable|=i<16?16:32;
        compare_far(i,s);
      }
  /* Every descriptor bit that leaves a valid readable data/code descriptor. */
  for(unsigned i=0;i<32;++i)for(unsigned bit=0;bit<64;++bit) {
    uint64_t descriptor=0x00cf92000000ffffULL^(1ULL<<bit),access=descriptor>>40;
    if((access&0x90)!=0x90 || (access&10)==8)continue;
    event_state_t s=event_seed(0x8000);event_word(0x8020,descriptor);
    s.gpr[3]=0x8801;event_word(s.gpr[3],0x8123456789abcdefULL);
    uint16_t selector=32;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    compare_far(i,s);
  }
  /* Offset bytes overlap the descriptor's accessed bit; retain the old value. */
  for(unsigned i=0;i<32;++i) {
    event_state_t s=event_seed(0x8000);s.gpr[3]=0x8020+(width==8?0:4);
    uint16_t selector=32;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    compare_far(i,s);
  }
  /* The selector field itself aliases A: install its old RPL, not the byte
     produced by marking the descriptor accessed. */
  for(unsigned i=0;i<32;++i) {
    event_state_t s=event_seed(0x8000);memset((void *)0x210000,0xa5,64);
    event_word(0x210008,0x00cfd2000000ffffULL);
    s.gdt.base=0x210008-0xcfd0;s.gdt.limit=0xcfd7;
    s.gpr[3]=0x21000d-width;compare_far(i,s);
  }
  for(unsigned i=0;i<32;++i)for(unsigned bit=0;bit<32;++bit) {
    if(!((1ULL<<bit)&0x244ed7))continue;
    event_state_t s=event_seed(0x8000);s.flags=2|(1ULL<<bit);
    s.gpr[3]=0x8801;event_word(s.gpr[3],0x876543210abcdefULL);
    uint16_t selector=32;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    compare_far(i,s);
  }
  return 0;
}
)C";
    run(F.Bytes, F.Entries, Harness, false, eventOptions(), {}, true);
  }
}

TEST_F(X64ContinuationSourceTest,
       FarPointerMatchesNativeAddressingAndOldBases) {
  for (bool SS : {false, true}) {
    SCOPED_TRACE(SS);
    FarPointerFixture F;
    // All base registers, address sizes, SIB scales and index registers,
    // including RSP/RBP and R12/R13. Each address includes a negative disp8.
    for (unsigned Address : {8u, 4u})
      for (unsigned Base = 0; Base != 16; ++Base) {
        const unsigned Index = (Base + 5) % 16;
        std::vector<uint8_t> Code;
        if (Address == 4)
          Code.push_back(0x67);
        Code.push_back(Base & 1 ? 0x64 : 0x65);
        Code.push_back(0x48 | (Base >= 8 ? 1 : 0) | (Index >= 8 ? 2 : 0));
        Code.insert(Code.end(),
                    {0x0f,
                     uint8_t(SS         ? 0xb2
                             : Base & 1 ? 0xb4
                                        : 0xb5),
                     0x44,
                     uint8_t((Base % 4) << 6 | (Index & 7) << 3 | (Base & 7)),
                     0xf9});
        F.add(std::move(Code));
      }
    // RIP/EIP-relative and no-base SIB with disp32; REX.B does not turn those
    // encodings into R13. REX.X does turn the SIB no-index field into R12.
    for (bool Address32 : {false, true})
      for (unsigned Form = 0; Form != 4; ++Form) {
        std::vector<uint8_t> Code;
        if (Address32)
          Code.push_back(0x67);
        Code.insert(Code.end(),
                    {0x65, uint8_t(Form == 3 ? 0x4b : 0x49), 0x0f,
                     uint8_t(SS ? 0xb2 : 0xb5), uint8_t(Form ? 4 : 5)});
        if (Form)
          Code.push_back(Form == 2 ? 0x85 : 0x25);
        Code.insert(Code.end(), {0xf9, 0xff, 0xff, 0xff});
        F.add(std::move(Code));
      }
    run(F.Bytes, F.Entries, F.harness() + FarPointerNativeCompare + R"C(
int main(void) {
  for(unsigned i=0;i<40;++i)for(unsigned unaligned=0;unaligned<10;++unaligned) {
    event_state_t s=event_seed(0x8000);uint64_t pointer=0x8800+unaligned;
    uint64_t ea;
    if(i<32) {
      unsigned base=i%16,index=(base+5)%16,scale=1u<<(base%4);
      s.gpr[base]=0xfffffff0;s.gpr[index]=index==4?0x987654321ULL:0x37;
      /* SIB index=RSP without REX.X denotes no index. */
      ea=s.gpr[base]+(index==4?0:s.gpr[index]*scale)-7;
      if(i>=16)ea=(uint32_t)ea;
      if(base&1)s.fs_base=pointer-ea;else s.gs_base=pointer-ea;
    } else {
      unsigned form=(i-32)%4;
      s.gpr[0]=0x37;s.gpr[12]=0x74;
      ea=form==0?0x1000+i*16+commands[i][0]-7:
         form==1?(uint64_t)-7:form==2?s.gpr[0]*4-7:s.gpr[12]-7;
      if(i>=36)ea=(uint32_t)ea;
      if(i==37)pointer=0x100007ff9ULL+unaligned;
      s.gs_base=pointer-ea;
    }
    event_word(pointer,0x123456789abcdef0ULL);uint16_t selector=32;
    uint64_t selector_address=i==37?s.gs_base+(uint32_t)(ea+8):pointer+8;
    memcpy((void *)(uintptr_t)selector_address,&selector,2);
    compare_far(i,s);
  }
  return 0;
}
)C",
        false, eventOptions(), {}, true);
  }
}

TEST_F(X64ContinuationSourceTest,
       FarPointerWrapsEachAddress32FieldBeforeSegmentBase) {
  FarPointerFixture F;
  for (uint8_t Opcode : {0xb4, 0xb5, 0xb2})
    for (unsigned Width : {2u, 4u, 8u}) {
      std::vector<uint8_t> Code{0x67, uint8_t(Opcode == 0xb5 ? 0x65 : 0x64)};
      if (Width != 4)
        Code.push_back(Width == 2 ? 0x66 : 0x48);
      Code.insert(Code.end(), {0x0f, Opcode, 3});
      F.add(std::move(Code));
    }
  run(F.Bytes, F.Entries, F.harness() + FarPointerNativeCompare + R"C(
int main(void) {
  const unsigned widths[]={2,4,8};
  for(unsigned i=0;i<9;++i)for(unsigned tail=0;tail<16;++tail) {
    event_state_t s=event_seed(0x8000);s.gpr[3]=0xfffffff0ULL+tail;
    unsigned width=widths[i%3];s.fs_base=s.gs_base=0x8000;
    uint64_t pointer=s.gpr[3]+0x8000;
    uint64_t selector_address=0x8000ULL+(uint32_t)(s.gpr[3]+width);
    memset((void *)0x207ff0,0x5a,32);
    uint64_t value=0xfedcba9876543210ULL;uint16_t selector=32;
    memcpy((void *)(uintptr_t)pointer,&value,width);
    memcpy((void *)(uintptr_t)selector_address,&selector,2);
    unsigned char saved[32];memcpy(saved,(void *)0x207ff0,32);
    compare_far(i,s);CHECK(!memcmp(saved,(void *)0x207ff0,32));
  }
  return 0;
}
)C",
      false, eventOptions(), {}, true);
}

TEST_F(X64ContinuationSourceTest, FarPointerRejectsBeforePublishingStateOrRAM) {
  FarPointerFixture F;
  for (uint8_t Opcode : {0xb4, 0xb5})
    for (unsigned Width : {2u, 4u, 8u})
      F.addLoad(Opcode, Width);
  run(F.Bytes, F.Entries, F.harness() + R"C(
int main(void) {
  unsigned char ram[4096],saved_ram[4096];
  const unsigned widths[]={2,4,8};
  for(unsigned i=0;i<6;++i)for(unsigned bad=0;bad<17;++bad) {
    uint64_t memory=(uintptr_t)ram;event_state_t s=event_seed(memory);
    s.pc=0x1000+i*16;s.gpr[3]=memory+0x800;uint16_t selector=32;
    event_word(s.gpr[3],0x123456789abcdef0ULL);
    if(bad<4) {selector=bad;s.gdt.base=1ULL<<47;}
    if(bad==4)s.gdt.limit=38;
    if(bad==5)s.gdt.base=1ULL<<47;
    if(bad==6)s.gdt.base=UINT64_MAX-31;
    if(bad==7)event_word(memory+32,0x00cf12000000ffffULL);
    if(bad==8)event_word(memory+32,0x00cf82000000ffffULL);
    if(bad==9)event_word(memory+32,0x00cf98000000ffffULL);
    if(bad==10)selector=35;
    if(bad==11){selector=4;s.unusable|=64;}
    if(bad==12){selector=4;s.ldt.attributes=0x89;}
    if(bad==13){selector=4;s.ldt.limit=6;}
    if(bad==14)s.cs.selector|=3;
    memcpy((void *)(uintptr_t)(s.gpr[3]+widths[i%3]),&selector,2);
    if(bad==15)s.gpr[3]=1ULL<<47;
    if(bad==16)s.gpr[3]=UINT64_MAX-widths[i%3];
    event_state_t saved=s;memcpy(saved_ram,ram,sizeof(ram));
    CHECK(step(i,ARG(&s))==5);
    CHECK(!memcmp(&s,&saved,sizeof(s)) && !memcmp(ram,saved_ram,sizeof(ram)));
  }
  return 0;
}
)C",
      false, eventOptions());
}

TEST_F(X64ContinuationSourceTest,
       FarPointerUsesExactMemoryWidthsAndReadOnlyAccessedDescriptors) {
  FarPointerFixture F;
  for (uint8_t Opcode : {0xb4, 0xb5, 0xb2})
    for (unsigned Width : {2u, 4u, 8u})
      F.addLoad(Opcode, Width);
  run(F.Bytes, F.Entries, F.harness() + R"C(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  const size_t page=(size_t)sysconf(_SC_PAGESIZE);
  unsigned char *ram=mmap(0,page*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(ram!=MAP_FAILED);CHECK(!mprotect(ram+page*3,page,PROT_NONE));
  const unsigned widths[]={2,4,8};
  for(unsigned i=0;i<9;++i) {
    event_state_t s=event_seed((uintptr_t)ram);s.pc=0x1000+i*16;
    unsigned width=widths[i%3];s.gpr[3]=(uintptr_t)(ram+page*3-width-2);
    event_word((uintptr_t)ram+32,0x00cf93000000ffffULL);
    uint64_t value=0x123456789abcdef0ULL;uint16_t selector=32;
    memcpy((void *)(uintptr_t)s.gpr[3],&value,width);
    memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    CHECK(!mprotect(ram,page,PROT_READ));CHECK(step(i,ARG(&s))==0);
    CHECK(s.pc==0x1000+i*16+commands[i][0]);
    CHECK((i<3?s.fs.selector:i<6?s.gs.selector:s.ss.selector)==32);
    CHECK(!mprotect(ram,page,PROT_READ|PROT_WRITE));
  }
  CHECK(!munmap(ram,page*4));return 0;
}
)C",
      false, eventOptions());
}

TEST_F(X64ContinuationSourceTest, FarPointerRequiresDescriptorOwningProfile) {
  run({0x48, 0x0f, 0xb4, 3}, {0x1000}, R"C(
int main(void) {
  state_t s=seed(0x1000),saved=s;
  CHECK(neverd_step_1000(ARG(&s))==4 && !memcmp(&s,&saved,sizeof(s)));
  return 0;
}
)C",
      true);
}

TEST_F(X64ContinuationSourceTest,
       FarPointerRetainsOnlyCompleteSupportedEncodings) {
  for (uint8_t Opcode : {0xb2, 0xb4, 0xb5}) {
    std::vector<uint8_t> Prefixes{0x64, 0x66, 0x67};
    do {
      auto Code = Prefixes;
      Code.insert(Code.end(), {0x4f, 0x0f, Opcode, 0x44, 0xac, 0xf9});
      auto Units =
          buildX64ContinuationSource(image(Code), {0x1000}, eventOptions());
      ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
      ASSERT_EQ(Units->size(), 1u);
      EXPECT_FALSE(Units->front().RequiresNativeExecution);
      EXPECT_EQ(Units->front().Bytes, Code);
    } while (std::next_permutation(Prefixes.begin(), Prefixes.end()));
  }
  for (std::vector<uint8_t> Bad : {std::vector<uint8_t>{0xf0, 0x0f, 0xb4, 3},
                                   {0xf2, 0x0f, 0xb4, 3},
                                   {0xf3, 0x0f, 0xb4, 3},
                                   {0x66, 0x66, 0x0f, 0xb4, 3},
                                   {0x67, 0x67, 0x0f, 0xb4, 3},
                                   {0x64, 0x65, 0x0f, 0xb4, 3},
                                   {0x48, 0x66, 0x0f, 0xb4, 3},
                                   {0x2e, 0x0f, 0xb4, 3},
                                   {0x0f, 0xb4, 0xc3},
                                   {0x0f, 0xb2, 0xc3},
                                   {0x0f, 0xb4, 0x04},
                                   {0x0f, 0xb4, 0x05, 0, 0, 0}}) {
    auto Units =
        buildX64ContinuationSource(image(Bad), {0x1000}, eventOptions());
    EXPECT_FALSE(bool(Units));
    if (!Units)
      llvm::consumeError(Units.takeError());
  }
}

TEST_F(X64ContinuationSourceTest,
       FarPointerLSSMatchesNativeStateWithoutShadow) {
  for (unsigned Width : {2u, 4u, 8u}) {
    SCOPED_TRACE(Width);
    FarPointerFixture F;
    for (unsigned Reg = 0; Reg != 16; ++Reg)
      F.addLoad(0xb2, Width, Reg);
    std::string Harness = F.harness() + FarPointerNativeCompare;
    Harness += "static const unsigned width=" + std::to_string(Width) + ";\n";
    Harness += R"C(
int main(void) {
  const unsigned types[]={0x92,0x93,0x96,0x97};
  for(unsigned i=0;i<16;++i)for(unsigned local=0;local<2;++local)
    for(unsigned type=0;type<4;++type) {
      event_state_t s=event_seed(0x8000);uint16_t selector=local?4:32;
      uint64_t table=local?s.ldt.base:s.gdt.base;
      event_word(table+(selector&~7),0x89c100efabcd2345ULL|((uint64_t)types[type]<<40));
      s.gpr[3]=0x8801;event_word(s.gpr[3],0xfedcba9876543210ULL);
      memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
      s.nmi_blocked=type&1;compare_far(i,s);
    }
  for(unsigned i=0;i<16;++i)for(unsigned bit=0;bit<64;++bit) {
    uint64_t d=0x00cf92000000ffffULL^(1ULL<<bit);
    if(((d>>40)&0xfa)!=0x92)continue;
    event_state_t s=event_seed(0x8000);event_word(0x8020,d);
    s.gpr[3]=0x8801;event_word(s.gpr[3],0x8123456789abcdefULL);
    uint16_t selector=32;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    compare_far(i,s);
  }
  for(unsigned i=0;i<16;++i) {
    event_state_t s=event_seed(0x8000);s.gpr[3]=0x8020+(width==8?0:4);
    uint16_t selector=32;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    compare_far(i,s);
    /* A aliases the high selector byte: 0x9200 must not become 0x9300. */
    s=event_seed(0x8000);memset((void *)0x210000,0xa5,64);
    event_word(0x210008,0x00cf92000000ffffULL);
    s.gdt.base=0x210008-0x9200;s.gdt.limit=0x9207;
    s.gpr[3]=0x21000c-width;compare_far(i,s);
  }
  for(unsigned i=0;i<16;++i)for(unsigned bit=0;bit<32;++bit) {
    if(!((1ULL<<bit)&0x244ed7))continue;
    event_state_t s=event_seed(0x8000);s.flags=2|(1ULL<<bit);
    s.gpr[3]=0x8801;event_word(s.gpr[3],0x876543210abcdefULL);
    uint16_t selector=32;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
    compare_far(i,s);
  }
  /* Null SS at CPL0 performs no descriptor access; ignored cache bytes stay. */
  for(unsigned i=0;i<16;++i)for(unsigned old_null=0;old_null<2;++old_null)
    for(unsigned nmi=0;nmi<2;++nmi) {
      event_state_t s=event_seed(0x8000);s.gdt.base=0x400000;s.gdt.limit=0;
      s.gpr[3]=0x8801;event_word(s.gpr[3],0x12345678abcdefULL);
      uint16_t selector=0;memcpy((void *)(uintptr_t)(s.gpr[3]+width),&selector,2);
      if(old_null){s.ss.selector=0;s.unusable|=2;}
      s.nmi_blocked=nmi;compare_far(i,s);
    }
  return 0;
}
)C";
    run(F.Bytes, F.Entries, Harness, false, eventOptions(), {}, true);
  }
}

TEST_F(X64ContinuationSourceTest, FarPointerNullFSAndGSMatchNativeClearPolicy) {
  FarPointerFixture F;
  for (uint8_t Opcode : {0xb4, 0xb5})
    for (unsigned Width : {2u, 4u, 8u})
      for (unsigned Reg = 0; Reg != 16; ++Reg)
        F.addLoad(Opcode, Width, Reg);
  auto O = eventOptions();
  O.NullSegmentBasePolicy = X64NullSegmentBasePolicy::Clear;
  run(F.Bytes, F.Entries, F.harness() + FarPointerNativeCompare + R"C(
int main(void) {
  const unsigned widths[]={2,4,8};
  const uint64_t bases[]={0,0x123456780000ULL,0xffffffff80000000ULL,0x7fffffff0000ULL};
  for(unsigned i=0;i<96;++i)for(unsigned selector=0;selector<4;++selector)
    for(unsigned base=0;base<4;++base)for(unsigned old_null=0;old_null<2;++old_null)
      for(unsigned nmi=0;nmi<2;++nmi) {
        event_state_t s=event_seed(0x8000);s.gdt.base=0x400000;s.gdt.limit=0;
        s.gpr[3]=0x8801;event_word(s.gpr[3],0xfedcba9876543210ULL);
        uint16_t sel=selector;unsigned width=widths[(i/16)%3];
        memcpy((void *)(uintptr_t)(s.gpr[3]+width),&sel,2);
        if(i<48){s.fs_base=bases[base];if(old_null){s.fs.selector=2;s.unusable|=16;}}
        else{s.gs_base=bases[base];if(old_null){s.gs.selector=3;s.unusable|=32;}}
        s.nmi_blocked=nmi;compare_far(i,s);
      }
  return 0;
}
)C",
      false, O, {}, true);
}

TEST_F(X64ContinuationSourceTest, FarPointerNullPoliciesNeverReadDescriptors) {
  for (auto Policy :
       {X64NullSegmentBasePolicy::Clear, X64NullSegmentBasePolicy::Preserve}) {
    SCOPED_TRACE(static_cast<unsigned>(Policy));
    FarPointerFixture F;
    for (uint8_t Opcode : {0xb4, 0xb5, 0xb2})
      for (unsigned Width : {2u, 4u, 8u})
        F.addLoad(Opcode, Width);
    auto O = eventOptions();
    O.NullSegmentBasePolicy = Policy;
    std::string Harness = F.harness();
    Harness += std::string("static const unsigned clear_base=") +
               (Policy == X64NullSegmentBasePolicy::Clear ? "1;\n" : "0;\n");
    Harness += R"C(
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  const size_t page=(size_t)sysconf(_SC_PAGESIZE);
  unsigned char *ram=mmap(0,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(ram!=MAP_FAILED);CHECK(!mprotect(ram+page,page,PROT_NONE));
  const unsigned widths[]={2,4,8};
  for(unsigned i=0;i<9;++i)for(unsigned selector=0;selector<(i<6?4:1);++selector)
    for(unsigned bad_table=0;bad_table<3;++bad_table) {
      event_state_t s=event_seed((uintptr_t)ram);s.pc=0x1000+i*16;
      unsigned width=widths[i%3];s.gpr[3]=(uintptr_t)(ram+page-width-2);
      uint64_t value=0xfedcba9876543210ULL;uint16_t sel=selector;
      memcpy((void *)(uintptr_t)s.gpr[3],&value,width);
      memcpy((void *)(uintptr_t)(s.gpr[3]+width),&sel,2);
      s.gdt.base=bad_table==0?(uintptr_t)(ram+page):bad_table==1?1ULL<<47:UINT64_MAX;
      s.gdt.limit=0;s.ldt.base=UINT64_MAX;s.ldt.limit=0;s.unusable|=64;
      event_state_t want=s;want.pc+=commands[i][0];
      want.gpr[0]=width==2?(s.gpr[0]&~65535ULL)|(value&65535):width==4?(uint32_t)value:value;
      if(i<3){want.fs.selector=sel;want.unusable|=16;if(clear_base)want.fs_base=0;}
      else if(i<6){want.gs.selector=sel;want.unusable|=32;if(clear_base)want.gs_base=0;}
      else{want.ss.selector=0;want.unusable|=2;}
      CHECK(!mprotect(ram,page,PROT_READ));
      CHECK(step(i,ARG(&s))==0 && !memcmp(&s,&want,sizeof(s)));
      CHECK(!mprotect(ram,page,PROT_READ|PROT_WRITE));
    }
  CHECK(!munmap(ram,page*2));return 0;
}
)C";
    run(F.Bytes, F.Entries, Harness, false, O);
  }
}

TEST_F(X64ContinuationSourceTest,
       FarPointerLSSRejectsBeforePublishingStateOrRAM) {
  FarPointerFixture F;
  for (unsigned Width : {2u, 4u, 8u})
    F.addLoad(0xb2, Width);
  run(F.Bytes, F.Entries, F.harness() + R"C(
int main(void) {
  unsigned char ram[4096],saved_ram[4096];const unsigned widths[]={2,4,8};
  for(unsigned i=0;i<3;++i)for(unsigned bad=0;bad<264;++bad) {
    uint64_t memory=(uintptr_t)ram;event_state_t s=event_seed(memory);
    s.pc=0x1000+i*16;s.gpr[3]=memory+0x800;uint16_t selector=32;
    event_word(s.gpr[3],0x123456789abcdef0ULL);
    if(bad<256){if((bad&0xfa)==0x92)continue;event_word(memory+32,0x00cf00000000ffffULL|((uint64_t)bad<<40));}
    if(bad>=256 && bad<259){selector=bad-255;s.gdt.base=1ULL<<47;}
    if(bad>=259 && bad<262)selector=32+bad-258;
    if(bad==262)s.gdt.limit=38;
    if(bad==263){selector=4;s.unusable|=64;}
    memcpy((void *)(uintptr_t)(s.gpr[3]+widths[i]),&selector,2);
    event_state_t saved=s;memcpy(saved_ram,ram,sizeof(ram));
    CHECK(step(i,ARG(&s))==5);
    CHECK(!memcmp(&s,&saved,sizeof(s)) && !memcmp(ram,saved_ram,sizeof(ram)));
  }
  return 0;
}
)C",
      false, eventOptions());
}

TEST_F(X64ContinuationSourceTest, FarPointerNullPolicyRequiresOwningProfile) {
  for (auto Policy :
       {X64NullSegmentBasePolicy::Clear, X64NullSegmentBasePolicy::Preserve,
        static_cast<X64NullSegmentBasePolicy>(255)}) {
    auto O = options();
    O.NullSegmentBasePolicy = Policy;
    auto Units = buildX64ContinuationSource(image({0x90}), {0x1000}, O);
    EXPECT_FALSE(bool(Units));
    if (!Units)
      llvm::consumeError(Units.takeError());
  }
  auto O = eventOptions();
  O.NullSegmentBasePolicy = static_cast<X64NullSegmentBasePolicy>(255);
  auto Units = buildX64ContinuationSource(image({0x90}), {0x1000}, O);
  EXPECT_FALSE(bool(Units));
  if (!Units)
    llvm::consumeError(Units.takeError());
}

TEST_F(X64ContinuationSourceTest,
       FarPointerNullLoadUsesOldBaseAndPublishesNewBase) {
  for (auto Policy :
       {X64NullSegmentBasePolicy::Clear, X64NullSegmentBasePolicy::Preserve}) {
    auto O = eventOptions();
    O.NullSegmentBasePolicy = Policy;
    std::string Harness = EventHarness;
    Harness += std::string("static const unsigned clear_base=") +
               (Policy == X64NullSegmentBasePolicy::Clear ? "1;\n" : "0;\n");
    Harness += R"C(
int main(void) {
  unsigned char ram[4096];event_state_t s=event_seed((uintptr_t)ram);
  unsigned char a[10],b[10];uint64_t value=0xfedcba9876543210ULL,next=0x123456789abcdef0ULL;
  uint16_t selector=3;memcpy(a,&value,8);memcpy(a+8,&selector,2);memcpy(b,&next,8);
  s.gpr[3]=(uintptr_t)b;s.fs_base=(uintptr_t)a-(uintptr_t)b;
  uint64_t base=s.fs_base;s.gdt.base=UINT64_MAX;s.gdt.limit=0;
  CHECK(neverd_step_v6_1000(ARG(&s))==0);
  CHECK(s.pc==0x1005 && s.gpr[0]==value && s.fs.selector==3 && (s.unusable&16));
  CHECK(s.fs_base==(clear_base?0:base));
  CHECK(neverd_step_v6_1005(ARG(&s))==0 && s.pc==0x1009);
  CHECK(s.gpr[1]==(clear_base?next:value));return 0;
}
)C";
    run({0x64, 0x48, 0x0f, 0xb4, 3, 0x64, 0x48, 0x8b, 0x0b}, {0x1000, 0x1005},
        Harness, false, O);
  }
}
