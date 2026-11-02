//===- X64NativeRegisterContinuationTests.inc - Native control banks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute bound debug-register moves and bounded CR4 writes on native KVM.
//===----------------------------------------------------------------------===//

TEST_F(X64ContinuationSourceTest, NativeRegistersRequireExplicitBindings) {
  run({0x0f, 0x21, 0xc0, 0x0f, 0x23, 0xf8, 0x0f, 0x20, 0xe0, 0x0f, 0x22, 0xe0},
      {0x1000, 0x1003, 0x1006, 0x1009}, R"C(
int main(void) {
  const uint64_t pcs[]={0x1000,0x1003,0x1006,0x1009};
  __typeof__(&neverd_step_v2_1000) steps[]={neverd_step_v2_1000,neverd_step_v2_1003,
                                         neverd_step_v2_1006,neverd_step_v2_1009};
  for(unsigned n=0;n<4;++n){state_v2_t s=seed_v2(pcs[n]),before=s;
    CHECK(steps[n](ARG(&s))==4 && !memcmp(&s,&before,sizeof(s)));}
  return 0;
})C",
      true, controlsOptions());
}

TEST_F(X64ContinuationSourceTest,
       NativeRegistersRetainBanksFlagsAndBoundedWrites) {
  auto O = controlsOptions();
  O.NativeDebugRegisterAccess = O.NativePageGlobalAccess = true;
  std::vector<uint8_t> Bytes;
  std::vector<va_t> Entries;
  std::string Calls, PCs;
  for (uint8_t Bank : {0, 1, 2, 3, 6, 7, 8})
    for (unsigned Register = 0; Register != 16; ++Register)
      for (bool Write : {false, true}) {
        const va_t PC = 0x1000 + Bytes.size();
        Entries.push_back(PC);
        if (!Calls.empty()) {
          Calls += ',';
          PCs += ',';
        }
        Calls += "neverd_step_v2_" + llvm::utohexstr(PC);
        PCs += "0x" + llvm::utohexstr(PC);
        if (Register >= 8)
          Bytes.push_back(0x41);
        Bytes.insert(
            Bytes.end(),
            {0x0f,
             uint8_t(Bank == 8 ? (Write ? 0x22 : 0x20) : (Write ? 0x23 : 0x21)),
             uint8_t(0xc0 | ((Bank == 8 ? 4 : Bank) << 3) | (Register & 7))});
      }
  run(Bytes, Entries,
      "static __typeof__(&neverd_step_v2_1000) steps[]={" + Calls +
          "};\nstatic const uint64_t pcs[]={" + PCs + "};\n" + R"C(
static uint64_t read_bank(unsigned bank) {
  uint64_t value;
  switch(bank) {
  case 0:__asm__ volatile("mov %%dr0,%0":"=r"(value)::"memory","cc");break;
  case 1:__asm__ volatile("mov %%dr1,%0":"=r"(value)::"memory","cc");break;
  case 2:__asm__ volatile("mov %%dr2,%0":"=r"(value)::"memory","cc");break;
  case 3:__asm__ volatile("mov %%dr3,%0":"=r"(value)::"memory","cc");break;
  case 4:__asm__ volatile("mov %%dr6,%0":"=r"(value)::"memory","cc");break;
  case 5:__asm__ volatile("mov %%dr7,%0":"=r"(value)::"memory","cc");break;
  default:__asm__ volatile("mov %%cr4,%0":"=r"(value)::"memory","cc");
  }
  return value;
}

static void write_bank(unsigned bank,uint64_t value) {
  switch(bank) {
  case 0:__asm__ volatile("mov %0,%%dr0"::"r"(value):"memory","cc");break;
  case 1:__asm__ volatile("mov %0,%%dr1"::"r"(value):"memory","cc");break;
  case 2:__asm__ volatile("mov %0,%%dr2"::"r"(value):"memory","cc");break;
  case 3:__asm__ volatile("mov %0,%%dr3"::"r"(value):"memory","cc");break;
  case 4:__asm__ volatile("mov %0,%%dr6"::"r"(value):"memory","cc");break;
  case 5:__asm__ volatile("mov %0,%%dr7"::"r"(value):"memory","cc");break;
  default:__asm__ volatile("mov %0,%%cr4"::"r"(value):"memory","cc");
  }
}
int main(void) {
  uint64_t initial[7];for(unsigned b=0;b<7;++b)initial[b]=read_bank(b);
  write_bank(5,0x400);
  const unsigned bits[]={11,7,6,4,2,0};
  for(unsigned b=0;b<7;++b)for(unsigned reg=0;reg<16;++reg)
  for(unsigned write=0;write<2;++write)for(unsigned pattern=0;pattern<64;++pattern){
    const unsigned n=b*32+reg*2+write;
    uint64_t value=b<4?0x180000+reg*64+pattern:
                    b==4?0xffff0ff0ULL|(pattern&15):
                    b==5?0x400|((pattern&3)<<8):initial[6]^((pattern&1)<<7);
    state_v2_t s=seed_v2(pcs[n]);s.core.gpr[reg]=value;
    for(unsigned i=0;i<6;++i)s.core.arbitrary[i]=(pattern>>i)&1;
    state_v2_t want=s;want.core.pc+=reg<8?3:4;want.core.flags&=~0x8d5ULL;
    for(unsigned i=0;i<6;++i)want.core.flags|=s.core.arbitrary[i]<<bits[i];
    write_bank(b,value);uint64_t expected=read_bank(b);
    if(write)write_bank(b,b==5?0x400:initial[b]);
    else want.core.gpr[reg]=expected;
    uint64_t before[7];for(unsigned k=0;k<7;++k)before[k]=read_bank(k);
    CHECK(steps[n](ARG(&s))==0 && !memcmp(&s,&want,sizeof(s)));
    for(unsigned k=0;k<7;++k)CHECK(read_bank(k)==(k==b?expected:before[k]));
  }
  for(unsigned bit=0;bit<64;++bit)if(bit!=7){
    unsigned n=6*32+1;state_v2_t s=seed_v2(pcs[n]);
    uint64_t current=read_bank(6);s.core.gpr[0]=current^(1ULL<<bit);
    state_v2_t before=s;CHECK(steps[n](ARG(&s))==5);
    CHECK(!memcmp(&s,&before,sizeof(s)) && read_bank(6)==current);
  }
  for(unsigned b=0;b<7;++b)write_bank(b,initial[b]);
  return 0;
})C",
      false, O, {}, true);
}

TEST_F(X64ContinuationSourceTest, NativePageGlobalWritesInvalidateGlobalTlb) {
  auto O = controlsOptions();
  O.NativePageGlobalAccess = true;
  run({0x0f, 0x22, 0xe0}, {0x1000}, R"C(
int main(void) {
  uint64_t cr3,cr4;__asm__ volatile("mov %%cr3,%0;mov %%cr4,%1":"=r"(cr3),"=r"(cr4)::"memory");
  memset((void*)0x6000,0,0x4000);
  *(uint64_t*)0x6000=0x7003;*(uint64_t*)0x7000=0x8003;
  *(uint64_t*)0x8000=0x83;*(uint64_t*)0x8008=0x9003;
  *(uint64_t*)0x9000=0xa103;
  *(volatile uint64_t*)0xa000=0x123456789abcdef0ULL;
  *(volatile uint64_t*)0xb000=0xfedcba9876543210ULL;
  uint64_t root=0x6000,off=cr4&~128ULL,on=off|128;
  __asm__ volatile("mov %0,%%cr4;mov %1,%%cr3;mov %2,%%cr4"::"r"(off),"r"(root),"r"(on):"memory","cc");
  CHECK(*(volatile uint64_t*)0x200000==*(volatile uint64_t*)0xa000);
  *(volatile uint64_t*)0x9000=0xb103;
  state_v2_t s=seed_v2(0x1000);s.core.gpr[0]=off;
  CHECK(neverd_step_v2_1000(ARG(&s))==0);
  CHECK(*(volatile uint64_t*)0x200000==*(volatile uint64_t*)0xb000);
  s.core.pc=0x1000;s.core.gpr[0]=on;
  CHECK(neverd_step_v2_1000(ARG(&s))==0);
  __asm__ volatile("mov %0,%%cr4;mov %1,%%cr3;mov %2,%%cr4"::"r"(off),"r"(cr3),"r"(cr4):"memory","cc");
  return 0;
})C",
      false, O, {}, true);
}

TEST_F(X64ContinuationSourceTest, NativeDebugAliasesKeepTheirBoundary) {
  auto O = controlsOptions();
  O.NativeDebugRegisterAccess = true;
  run({0x0f, 0x21, 0xe0, 0x0f, 0x23, 0xe8}, {0x1000, 0x1003}, R"C(
int main(void) {
  state_v2_t s=seed_v2(0x1000),before=s;
  CHECK(neverd_step_v2_1000(ARG(&s))==4 && !memcmp(&s,&before,sizeof(s)));
  s.core.pc=0x1003;before=s;
  CHECK(neverd_step_v2_1003(ARG(&s))==4 && !memcmp(&s,&before,sizeof(s)));
  return 0;
})C",
      true, O);
}
