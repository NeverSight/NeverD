//===- DoubleWordSumFixture.inc - ELF32 register-pair arithmetic ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Words assembled by llvm-mc, stored in target byte order by LinkedImage.
namespace doubleword {
constexpr uint32_t RVLow = 0x00c50533, RVHigh = 0x00d585b3;
constexpr uint32_t RVCarry = 0x00c53633, RVUnrelated = 0x00d53633;
constexpr uint32_t RVAddCarry = 0x00c585b3, RVStore = 0x00b72023;
constexpr uint32_t RVReturn = 0x00008067;
constexpr uint32_t RVSum[] = {RVHigh, RVLow, RVCarry, RVAddCarry, RVReturn};
// sltu a2,a0,a3 compares the low sum to a high input word.
constexpr uint32_t RVWrongCarry[] = {RVHigh, RVLow, RVUnrelated, RVAddCarry,
                                     RVReturn};
constexpr uint32_t RVStored[] = {RVHigh,     RVLow,   RVCarry,
                                 RVAddCarry, RVStore, RVReturn};
constexpr uint32_t RVAlloc = 0xff010113, RVRelease = 0x01010113;
constexpr uint32_t RVSpillA0 = 0x00a12623, RVReloadA0 = 0x00c12503;
constexpr uint32_t RVOverwriteByte = 0x00e106a3; // sb a4,13(sp)
constexpr uint32_t RVSpilled[] = {RVAlloc,    RVSpillA0, RVReloadA0,
                                  RVHigh,     RVLow,     RVCarry,
                                  RVAddCarry, RVRelease, RVReturn};
constexpr uint32_t RVPartialSpill[] = {
    RVAlloc, RVSpillA0, RVOverwriteByte, RVReloadA0, RVHigh,
    RVLow,   RVCarry,   RVAddCarry,      RVRelease,  RVReturn};

constexpr uint32_t PPCAddC4 = 0x7c862014, PPCAddE3 = 0x7c651914;
constexpr uint32_t PPCAddC3 = 0x7c651814, PPCAddE4 = 0x7c862114;
constexpr uint32_t PPCAdd4 = 0x7c862214, PPCAdd3 = 0x7c651a14;
constexpr uint32_t PPCStore3 = 0x90670000, PPCStore4 = 0x90870000;
constexpr uint32_t PPCReturn = 0x4e800020;
constexpr uint32_t BESum[] = {PPCAddC4, PPCAddE3, PPCReturn};
constexpr uint32_t LESum[] = {PPCAddC3, PPCAddE4, PPCReturn};
// Plain add leaves XER.CA unchanged for the following adde.
constexpr uint32_t BEIncomingCarry[] = {PPCAdd4, PPCAddE3, PPCReturn};
constexpr uint32_t LEIncomingCarry[] = {PPCAdd3, PPCAddE4, PPCReturn};
constexpr uint32_t BEStored[] = {PPCAddC4, PPCAddE3, PPCStore3, PPCReturn};
constexpr uint32_t LEStored[] = {PPCAddC3, PPCAddE4, PPCStore4, PPCReturn};
} // namespace doubleword
