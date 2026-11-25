//===- PPC64ELFv2Fixture.inc - Independent ELFv2 input words --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// The caller establishes r2 = 0x30000 and branches to callee+8. The callee
// establishes the same TOC at its global entry and uses it at the local one.
constexpr va_t Text = 0x10000, Data = 0x30000, Callee = Text + 64;
constexpr uint32_t Code[] = {0x3c4c0002, 0x38420000, 0x7c0802a6, 0xf821ffe1,
                             0xf8010030, 0x38630001, 0x48000031, 0x60000000,
                             0xe8820000, 0x7c632214, 0x38210020, 0xe8010010,
                             0x7c0803a6, 0x4e800020, 0x60000000, 0x60000000,
                             0x3c4c0002, 0x3842ffc0, 0xe8820000, 0x4e800020};
constexpr unsigned SeparateEntries = 96, ClobberingSingleEntry = 32,
                   ReservedEntry = 224;
constexpr uint32_t ChangeTOC = 0x38420008, BranchToGlobal = 0x48000029,
                   ReadUnknownRegister = 0x38430000, Return = 0x4e800020;
// Link to the appended stub, which calculates callee's global entry by PC.
constexpr uint32_t CallStub = 0x48000039, NonRelativePrefix = 0x0603ffff;
constexpr uint32_t GlobalStub[] = {0x0613ffff, 0x3980fff0, 0x7d8903a6,
                                   0x4e800420};
constexpr uint32_t LoadTOCLiteral = 0xe84cfff8, AddEntryToTOC = 0x7c426214;
