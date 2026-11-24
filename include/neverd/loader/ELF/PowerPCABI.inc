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
