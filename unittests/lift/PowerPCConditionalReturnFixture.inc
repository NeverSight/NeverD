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
constexpr uint32_t BeqCTR = 0x4d820420;  // bcctr 12,2,0
constexpr uint32_t Zero = 0x38600000;    // li r3,0
constexpr uint32_t EQ = 0x3c602000;      // lis r3,0x2000
constexpr uint32_t SetCR = 0x7c6ff120;   // mtcr r3
constexpr uint32_t SetCTR = 0x7c8903a6;  // mtctr r4
constexpr uint32_t LinkNext = 0x42020005;     // bcl 16,2,.+4
constexpr uint32_t LinkBack = 0x4182fff9;     // bcl 12,2,.-8
constexpr uint32_t AbsoluteBack = 0x4182fffb; // bcla 12,2,-8
constexpr uint32_t Unsupported[] = {
    0x4d821020, // bclr with reserved BH=2.
    0x4da20020, // bclr with reserved CR prediction.
    0x4d820c20, // bcctr with reserved BH=1.
    0x4d821421, // bcctrl with reserved BH=2.
    0x4c020420, // bcctr cannot decrement CTR.
    0x4e000421, // bcctrl cannot test CTR.
    0x4d824021, // bclrl with a reserved XL bit.
};
