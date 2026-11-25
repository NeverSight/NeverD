//===- RISCVPLT.inc - RISC-V ELF psABI default PLT instructions -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Included by the RISC-V linkage reader. The default PLT header occupies
// eight instructions. Each following entry occupies four, even with RVC.
//
//===----------------------------------------------------------------------===//

constexpr unsigned Word = sizeof(uint32_t);
constexpr unsigned HeaderSize = 8 * Word;
constexpr unsigned EntrySize = 4 * Word;
constexpr unsigned LowBits = 12;
constexpr uint32_t UFormMask = 0x00000fff;
constexpr uint32_t IFormMask = 0x000fffff;
constexpr uint32_t AuipcT2 = 0x397;
constexpr uint32_t SubT1T1T3 = 0x41c30333;
constexpr unsigned SubHighByteOffset = 2 * Word - 1;
constexpr char SubHighByte = static_cast<char>(SubT1T1T3 >> 24);
constexpr uint32_t LwT3T2 = 0x3ae03;
constexpr uint32_t LdT3T2 = 0x3be03;
constexpr uint32_t AdjustT1 = 0xfd430313; // addi t1,t1,-(HeaderSize + 12)
constexpr uint32_t AddiT0T2 = 0x38293;
constexpr uint32_t SrliT1RV32 = 0x00235313;
constexpr uint32_t SrliT1RV64 = 0x00135313;
constexpr uint32_t LwMap = 0x0042a283; // lw t0,4(t0)
constexpr uint32_t LdMap = 0x0082b283; // ld t0,8(t0)
constexpr uint32_t JumpT3 = 0x000e0067;
constexpr uint32_t AuipcT3 = 0xe17;
constexpr uint32_t LwT3T3 = 0xe2e03;
constexpr uint32_t LdT3T3 = 0xe3e03;
constexpr uint32_t LinkT1T3 = 0x000e0367;
constexpr uint32_t Nop = 0x00000013;
