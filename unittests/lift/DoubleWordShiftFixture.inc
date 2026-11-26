//===- DoubleWordShiftFixture.inc - ELF32 register-pair shifts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

namespace wordshift {
// Clang -O2 RV32IMAC left shift. Two adjacent compressed instructions
// occupy one little-endian word in the ELF fixture.
constexpr uint32_t RVLeft[] = {0x02067693, 0xca11ee81, 0x40c006b3,
                               0x00d556b3, 0x00c51533, 0x00c595b3,
                               0x80828dd5, 0x00c515b3, 0x80824501};
// RV32I has four-byte instruction alignment and an explicit return check.
constexpr uint32_t RVBaseLeft[] = {
    0x02067693, 0x02069063, 0x00060c63, 0x40c006b3, 0x00d556b3, 0x00c51533,
    0x00c595b3, 0x00d5e5b3, 0x00008067, 0x00c515b3, 0x00000513, 0x00008067};
// The same RV32I leaf with xori/sltiu/xor flipping bit 32 only at count 17.
constexpr uint32_t RVWrongAt17[] = {
    0x02067693, 0x02069663, 0x02060263, 0x40c006b3, 0x00d556b3,
    0x00c51533, 0x00c595b3, 0x00d5e5b3, 0x01164713, 0x00173713,
    0x00e5c5b3, 0x00008067, 0x00c515b3, 0x00000513, 0x00008067};

// PPC32 big-endian ABI. The small-count arm shifts into r3 from r4.
// The large-count arm returns the shifted r4 in r3 and zero in r4.
constexpr uint32_t BELeft[] = {0x70a60020, 0x40820028, 0x28050000, 0x4d820020,
                               0x54a606fe, 0x20e60020, 0x7c873c30, 0x7c633030,
                               0x7c842830, 0x7c633b78, 0x4e800020, 0x3865ffe0,
                               0x7c831830, 0x38800000, 0x4e800020};
constexpr uint32_t ShiftHighAgain = 0x00c595b3; // sll a1,a1,a2
constexpr uint32_t FillOne = 0x38800001;        // li r4,1
constexpr uint32_t CrossFromHigh = 0x7c673c30;  // srw r7,r3,r7
} // namespace wordshift
