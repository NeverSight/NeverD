//===- RISCVPLTFixture.inc - Linked RISC-V psABI instruction words --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Assembled for a GOT at PLT0 - 0x1190. Both entry loads have negative
// displacements. Moving the entire image leaves these words unchanged.
//
//===----------------------------------------------------------------------===//

constexpr va_t PLT0 = 0x21100;
constexpr va_t GOT = 0x1ff70;
constexpr uint8_t RepeatedCompressedByte = 0x41; // 0x4141: c.li sp,16.

constexpr uint32_t Words32[] = {
    0xfffff397, 0x41c30333, 0xe703ae03, 0xfd430313, 0xe7038293, 0x00235313,
    0x0042a283, 0x000e0067, 0xfffffe17, 0xe58e2e03, 0x000e0367, 0x00000013,
    0xfffffe17, 0xe4ce2e03, 0x000e0367, 0x00000013,
};
constexpr uint32_t Words64[] = {
    0xfffff397, 0x41c30333, 0xe703be03, 0xfd430313, 0xe7038293, 0x00135313,
    0x0082b283, 0x000e0067, 0xfffffe17, 0xe60e3e03, 0x000e0367, 0x00000013,
    0xfffffe17, 0xe58e3e03, 0x000e0367, 0x00000013,
};
// The caller starts 32 bytes before PLT0. The AUIPC at +4 and JALR
// target the first PLT entry 60 bytes ahead of that AUIPC.
constexpr uint32_t TailCaller[] = {0x00350513, 0x00000317, 0x03c30067};
