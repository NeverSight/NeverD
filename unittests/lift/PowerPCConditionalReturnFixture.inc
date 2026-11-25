//===- PowerPCConditionalReturnFixture.inc - Encoded branch fixtures ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

constexpr uint32_t BeqLR = 0x4d820020;   // beqlr
constexpr uint32_t SetLR = 0x7c8803a6;   // mtlr r4
constexpr uint32_t Compare = 0x2c030000; // cmpwi r3,0
constexpr uint32_t Skip = 0x41820008;    // beq .+8
constexpr uint32_t Ret = 0x4e800020;     // blr
constexpr uint32_t Unsupported[] = {
    BeqLR | 1,        // Conditional call through LR.
    BeqLR | 0x1000,   // Reserved BH encoding.
    BeqLR | 0x200000, // Reserved prediction hint.
    BeqLR | 0x400,    // Conditional transfer through CTR.
};
