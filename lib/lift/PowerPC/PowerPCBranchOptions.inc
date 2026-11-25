//===- PowerPCBranchOptions.inc - Power ISA BO and BH fields --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

constexpr unsigned IgnoreCR = 0x10;
constexpr unsigned BranchIfCRSet = 0x08;
constexpr unsigned IgnoreCTR = 0x04;
constexpr unsigned BranchIfCTRZero = 0x02;
constexpr unsigned PredictionTaken = 0x01;
constexpr uint32_t ReservedXLBits = 0xe000;
constexpr uint32_t HintMask = 0x1800;
constexpr uint32_t ReservedHint = 0x1000;
