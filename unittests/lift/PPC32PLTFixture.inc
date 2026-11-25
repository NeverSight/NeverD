//===- PPC32PLTFixture.inc - Independently assembled PPC32 PLT inputs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// The two calls branch to the same lwz/mtctr/bctr veneer. The second call
// advances r30 to the next slot. An absolute veneer follows the PIC veneer.
constexpr va_t Text = 0x10000, PLT = 0x30000, Rela = 0x40000;
constexpr uint32_t Words[] = {
    0x7c0802a6, 0x9421fff0, 0x90010014, 0x93c10008, 0x429f0005, 0x7fc802a6,
    0x3fde0002, 0x3bdeffec, 0x48000021, 0x3bde0004, 0x48000019, 0x83c10008,
    0x80010014, 0x38210010, 0x7c0803a6, 0x4e800020, 0x817e0000, 0x7d6903a6,
    0x4e800420, 0x3d600003, 0x816b0000, 0x7d6903a6, 0x4e800420};
constexpr va_t PICStub = Text + 64, AbsoluteStub = Text + 76;
constexpr size_t FinalGOTWord = 7, LastInstruction = 22;
constexpr uint32_t UseArgument = 0x3bc30000, BranchLinkCTR = 0x4e800421;
constexpr size_t LastShort = 8191;
constexpr unsigned ClassicHeader = 72, ShortStride = 8, LongStride = 16;
