//===- CycleVMX.inc - Assembled counter and VMX instructions -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

namespace cycle_fixture {
struct CounterWords {
  riscv::Counter Kind;
  uint32_t Low, High, Discard, Last; // rd=a0, a1, zero, a2 respectively.
};
constexpr CounterWords Counters[] = {
    {riscv::Counter::Cycle, 0xc0002573, 0xc80025f3, 0xc0002073, 0xc8002673},
    {riscv::Counter::Time, 0xc0102573, 0xc81025f3, 0xc0102073, 0xc8102673},
    {riscv::Counter::Instret, 0xc0202573, 0xc82025f3, 0xc0202073, 0xc8202673},
};
constexpr uint32_t Other[] = {
    0xc0302573, // csrr a0,hpmcounter3
    0xc0052573, // csrrs a0,cycle,a0 (attempted write)
    0xc0001573, // csrrw a0,cycle,zero (attempted write)
    0xc0006573, // csrrsi a0,cycle,0
};
constexpr uint32_t ReadFCSR = 0x00302673;    // frcsr a2
constexpr uint32_t Retry = 0xfec59ae3;       // bne a1,a2,.-12
constexpr uint32_t Equal = 0xfec58ae3;       // beq a1,a2,.-12
constexpr uint32_t Return = 0x00008067;      // ret
constexpr uint32_t ReplaceHigh = 0x00100593; // li a1,1
} // namespace cycle_fixture

namespace vmx_fixture {
struct VectorBinary {
  uint32_t Word;
  unsigned Dst, A, B, LaneBytes;
  bool Subtract = false;
};
constexpr VectorBinary Operations[] = {
    {0x10431000, 2, 3, 2, 1},          // vaddubm v2,v3,v2 (clang)
    {0x1000f800, 0, 0, 31, 1},         // vaddubm v0,v0,v31
    {0x13fff800, 31, 31, 31, 1},       // vaddubm v31,v31,v31
    {0x1040f800, 2, 0, 31, 1},         // vaddubm v2,v0,v31
    {0x10431040, 2, 3, 2, 2},          // vadduhm v2,v3,v2
    {0x1000f840, 0, 0, 31, 2},         // vadduhm v0,v0,v31
    {0x13fff840, 31, 31, 31, 2},       // vadduhm v31,v31,v31
    {0x10431080, 2, 3, 2, 4},          // vadduwm v2,v3,v2
    {0x1000f880, 0, 0, 31, 4},         // vadduwm v0,v0,v31
    {0x13fff880, 31, 31, 31, 4},       // vadduwm v31,v31,v31
    {0x10431400, 2, 3, 2, 1, true},    // vsububm v2,v3,v2
    {0x1000fc00, 0, 0, 31, 1, true},   // vsububm v0,v0,v31
    {0x13fffc00, 31, 31, 31, 1, true}, // vsububm v31,v31,v31
    {0x10431440, 2, 3, 2, 2, true},    // vsubuhm v2,v3,v2
    {0x1000fc40, 0, 0, 31, 2, true},   // vsubuhm v0,v0,v31
    {0x13fffc40, 31, 31, 31, 2, true}, // vsubuhm v31,v31,v31
    {0x10431480, 2, 3, 2, 4, true},    // vsubuwm v2,v3,v2
    {0x1000fc80, 0, 0, 31, 4, true},   // vsubuwm v0,v0,v31
    {0x13fffc80, 31, 31, 31, 4, true}, // vsubuwm v31,v31,v31
};
constexpr uint32_t Other[] = {
    0x10431001, // same register fields, different low bit
    0x10431200, // vaddubs (saturating)
    0x10431240, // vadduhs (saturating halfword)
    0x10431280, // vadduws (saturating word)
    0x104310c0, // vaddudm (doubleword lanes)
    0x10431600, // vsububs (saturating)
    0x10431640, // vsubuhs
    0x10431680, // vsubuws
    0x104314c0, // vsubudm (doubleword lanes)
    0x10431580, // vsubcuw (carry-out)
};
constexpr uint32_t AddGPR = 0x7c632214;       // add r3,r3,r4
constexpr uint32_t Return = 0x4e800020;       // blr
constexpr uint32_t UnusedFirst = 0x10432000;  // vaddubm v2,v3,v4
constexpr uint32_t UnusedMiddle = 0x10422000; // vaddubm v2,v2,v4
constexpr uint32_t ReturnsGPR[] = {Operations[0].Word, AddGPR, Return};
constexpr uint32_t EarlierGPR[] = {AddGPR, Operations[0].Word, Return};
} // namespace vmx_fixture
