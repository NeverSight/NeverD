//===- CycleVMX.inc - Assembled counter and VMX instructions -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

namespace cycle_fixture {
constexpr uint32_t Low = 0xc0002573;     // rdcycle a0
constexpr uint32_t High = 0xc80025f3;    // rdcycleh a1
constexpr uint32_t Discard = 0xc0002073; // rdcycle zero
constexpr uint32_t Other[] = {
    0xc0102573, // rdtime a0
    0xc0052573, // csrrs a0,cycle,a0 (attempted write)
    0xc0001573, // csrrw a0,cycle,zero (attempted write)
    0xc0006573, // csrrsi a0,cycle,0
};
constexpr uint32_t Last = 0xc8002673;        // rdcycleh a2
constexpr uint32_t Retry = 0xfec59ae3;       // bne a1,a2,.-12
constexpr uint32_t Equal = 0xfec58ae3;       // beq a1,a2,.-12
constexpr uint32_t Return = 0x00008067;      // ret
constexpr uint32_t ReplaceHigh = 0x00100593; // li a1,1
constexpr uint32_t Read64[] = {High, Low, Last, Retry, Return};
constexpr uint32_t WrongEdge[] = {High, Low, Last, Equal, Return};
constexpr uint32_t Overwritten[] = {High,  Low,         Last,
                                    Retry, ReplaceHigh, Return};
} // namespace cycle_fixture

namespace vmx_fixture {
struct VectorAdd {
  uint32_t Word;
  unsigned Dst, A, B;
};
constexpr VectorAdd Adds[] = {
    {0x10431000, 2, 3, 2},    // vaddubm v2,v3,v2 (clang)
    {0x1000f800, 0, 0, 31},   // vaddubm v0,v0,v31
    {0x13fff800, 31, 31, 31}, // vaddubm v31,v31,v31
    {0x1040f800, 2, 0, 31},   // vaddubm v2,v0,v31
};
constexpr uint32_t Other[] = {
    0x10431001, // same register fields, different low bit
    0x10431200, // vaddubs (saturating)
    0x10431040, // vadduhm (halfword lanes)
};
constexpr uint32_t AddGPR = 0x7c632214;       // add r3,r3,r4
constexpr uint32_t Return = 0x4e800020;       // blr
constexpr uint32_t UnusedFirst = 0x10432000;  // vaddubm v2,v3,v4
constexpr uint32_t UnusedMiddle = 0x10422000; // vaddubm v2,v2,v4
constexpr uint32_t ReturnsGPR[] = {Adds[0].Word, AddGPR, Return};
constexpr uint32_t EarlierGPR[] = {AddGPR, Adds[0].Word, Return};
} // namespace vmx_fixture
