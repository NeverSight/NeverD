//===- PowerPCConditionalBranchFixture.inc - Conditional transfers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

// Encoded with Clang's PowerPC assembler. The branch register is intentionally
// unaligned in the test so that the comparison also observes the target mask.
constexpr struct {
  uint32_t Word;
  NdOp Transfer;
  bool ViaCTR;
} ConditionalBranches[] = {
    {0x41820019, NdOp::CALL, false},       // bcl 12,2,.+24
    {0x4082fff9, NdOp::CALL, false},       // bcl 4,2,.-8
    {0x41821403, NdOp::CALL, false},       // bcla 12,2,0x1400
    {0x42020011, NdOp::CALL, false},       // bcl 16,2,.+16
    {0x42420011, NdOp::CALL, false},       // bcl 18,2,.+16
    {0x41020011, NdOp::CALL, false},       // bcl 8,2,.+16
    {0x40420011, NdOp::CALL, false},       // bcl 2,2,.+16
    {0x41e20011, NdOp::CALL, false},       // btl+ 2,.+16
    {0x4d820021, NdOp::INDIR_CALL, false}, // bclrl 12,2,0
    {0x4c820821, NdOp::INDIR_CALL, false}, // bclrl 4,2,1
    {0x4d021821, NdOp::INDIR_CALL, false}, // bclrl 8,2,3
    {0x4f020021, NdOp::INDIR_CALL, false}, // bclrl 24,2,0
    {0x4d820420, NdOp::INDIR_BR, true},    // bcctr 12,2,0
    {0x4c821c20, NdOp::INDIR_BR, true},    // bcctr 4,2,3
    {0x4d821c21, NdOp::INDIR_CALL, true},  // bcctrl 12,2,3
    {0x4c820421, NdOp::INDIR_CALL, true},  // bcctrl 4,2,0
    {0x42020005, NdOp::COPY, false},       // bcl 16,2,.+4
    {0x41820005, NdOp::COPY, false},       // bcl 12,2,.+4
};
