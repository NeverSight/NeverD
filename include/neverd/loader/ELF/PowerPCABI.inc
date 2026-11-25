//===- PowerPCABI.inc - PowerPC ELF integer ABI layout ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Define the PowerPC ELF descriptor, register and linkage-area layout.
///
//===----------------------------------------------------------------------===//
// Power Architecture 64-bit ELF ABI, Function Descriptors and Stack Frame.
// GNU ld also accepts descriptors that overlap their unused environment word.

inline constexpr char DescriptorSection[] = ".opd";
inline constexpr char TOCSection[] = ".toc";
inline constexpr unsigned TOCBias = 0x8000;
inline constexpr unsigned ELFv1 = 1;
inline constexpr unsigned ELFv2 = 2;
inline constexpr unsigned VLE = 0x10000000;        // PF_PPC_VLE and SHF_PPC_VLE
inline constexpr unsigned DynamicOPD = 0x70000001; // DT_PPC64_OPD
inline constexpr unsigned DynamicOPDSize = 0x70000002; // DT_PPC64_OPDSZ
inline constexpr unsigned InstructionAlignment = 4;
inline constexpr char ELFv1Name[] = "elfv1";
inline constexpr char ELFv2Name[] = "elfv2";
inline constexpr unsigned SymbolVisibilityMask = 0x3; // ELF64_ST_VISIBILITY
inline constexpr unsigned WordSize = 8;
inline constexpr unsigned EntryOffset = 0;
inline constexpr unsigned TOCOffset = WordSize;
inline constexpr unsigned EnvironmentOffset = 2 * WordSize;
inline constexpr unsigned DescriptorSize = 3 * WordSize;
inline constexpr unsigned TOCRegister = 2;
inline constexpr unsigned EnvironmentRegister = 11;
inline constexpr unsigned PPC32StackArgumentBase = 8;
inline constexpr unsigned ELFv1StackArgumentBase = 112;
inline constexpr unsigned ELFv2StackArgumentBase = 96;
inline constexpr unsigned TOCSaveOffset = 40;
inline constexpr unsigned DynamicGlink = 0x70000000; // DT_PPC64_GLINK
inline constexpr unsigned GlinkFirstEntryOffset = 32;
inline constexpr unsigned GlinkShortEntries = 32768;
inline constexpr unsigned GlinkShortEntrySize = 8;
inline constexpr unsigned GlinkLongEntrySize = 12;

// Integer instructions in ELFv1 linker-generated linkage code. DS loads
// include both low opcode bits in the mask to exclude update and word loads.
inline constexpr uint32_t DFormMask = 0xffff0000;
inline constexpr uint32_t DSFormMask = 0xffff0003;
inline constexpr uint32_t BranchMask = 0xfc000003;
inline constexpr uint32_t BranchDisplacementMask = 0x03fffffc;
inline constexpr unsigned BranchDisplacementBits = 26;
inline constexpr unsigned ImmediateBits = 16;
inline constexpr uint32_t Branch = 0x48000000;
inline constexpr uint32_t BranchLink = 0x48000001;
inline constexpr uint32_t SaveTOC = 0xf8410000 | TOCSaveOffset;
inline constexpr uint32_t RestoreTOC = 0xe8410000 | TOCSaveOffset;
inline constexpr uint32_t AddisR11R2 = 0x3d620000;
inline constexpr uint32_t AddiR11R11 = 0x396b0000;
inline constexpr uint32_t AddisR2R2 = 0x3c420000;
inline constexpr uint32_t AddiR2R2 = 0x38420000;
inline constexpr uint32_t LoadR12R2 = 0xe9820000;
inline constexpr uint32_t LoadR12R11 = 0xe98b0000;
inline constexpr uint32_t LoadR2R2 = 0xe8420000;
inline constexpr uint32_t LoadR2R11 = 0xe84b0000;
inline constexpr uint32_t LoadR11R2 = 0xe9620000;
inline constexpr uint32_t LoadR11R11 = 0xe96b0000;
inline constexpr uint32_t MoveR12ToCTR = 0x7d8903a6;
inline constexpr uint32_t BranchCTR = 0x4e800420;
inline constexpr uint32_t XorR11R12R12 = 0x7d8b6278;
inline constexpr uint32_t AddR2R2R11 = 0x7c425a14;
inline constexpr uint32_t XorR2R12R12 = 0x7d826278;
inline constexpr uint32_t AddR11R11R2 = 0x7d6b1214;
inline constexpr uint32_t CompareR2Zero = 0x28220000;
inline constexpr uint32_t BranchCTRNotEqual = 0x4ce20420;
