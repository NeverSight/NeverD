//===- PowerPCPrefixed.inc - Power ISA 3.1 prefix fields ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed-point prefix formats, fetch block and signed displacement fields.
///
//===----------------------------------------------------------------------===//

inline constexpr unsigned FetchBlockBytes = 64;
inline constexpr unsigned InstructionBytes = 8;
inline constexpr unsigned DisplacementBits = 34;
inline constexpr unsigned LowImmediateBits = 16;
inline constexpr uint32_t LowImmediateMask = (1u << LowImmediateBits) - 1;
inline constexpr uint32_t HighImmediateMask =
    (1u << (DisplacementBits - LowImmediateBits)) - 1;
inline constexpr uint32_t Relative = 0x00100000;
inline constexpr uint32_t MLS = 0x06000000;
inline constexpr uint32_t EightLS = 0x04000000;
inline constexpr uint32_t PrefixMask = ~HighImmediateMask;
inline constexpr uint32_t RelativeMLS = MLS | Relative;
inline constexpr uint32_t RelativeEightLS = EightLS | Relative;
