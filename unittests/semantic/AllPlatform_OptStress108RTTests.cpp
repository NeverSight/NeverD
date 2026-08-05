//===- AllPlatform_OptStress108RTTests.cpp - checksum / matrix rodata shapes =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * crc8   - table-driven CRC-8 (poly 0x07) reduction a nibble at a time over
//              a rodata message, self-indexed into a 16-entry rodata table
//              (`crc=(crc<<4)^tab[(crc>>4)&0xF]`).  Pins a state-carrying self-
//              indexed rodata gather (the index is the running CRC, not the
//              loop counter).
//   * matmul - 6x6 integer matrix multiply of two rodata matrices,
//              `C[i][j]=sum_k A[i*6+k]*B[k*6+j]`.  Pins the classic row-stride /
//              column-stride 2D indexed dot product reading two rodata arrays.
//   * binsrch- binary search of a sorted rodata key table for a runtime key with
//              a parallel rodata payload gather on hit (`mid=(lo+hi)>>1`).  Pins
//              a log-step index-halving, variable-trip rodata access at a
//              computed midpoint (distinct from a linear threshold scan).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress108RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress108RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress108RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress108RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress108RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress108RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress108RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress108RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress108TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // nibble-at-a-time table-driven CRC-8: self-indexed 16-entry rodata gather.
    {p+"_crc8",
     "static const unsigned char "+p+"_crctab[16]={\n"
     "0x00,0x07,0x0e,0x09,0x1c,0x1b,0x12,0x15, 0x38,0x3f,0x36,0x31,0x24,0x23,0x2a,0x2d};\n"
     "static const unsigned char "+p+"_msg[40]={\n"
     "0x31,0x9a,0x4c,0xe7,0x05,0xbd,0x72,0x18, 0x8f,0x23,0xd6,0x4a,0x91,0x0c,0xfe,0x57,\n"
     "0x6b,0xa3,0x2e,0xd0,0x14,0x88,0x3d,0xc9, 0x60,0xf5,0x1b,0xa7,0x42,0xce,0x09,0x96,\n"
     "0x7d,0xe1,0x35,0xb8,0x4f,0xd2,0x26,0xab};\n"
     +t+" "+p+"_crc8("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned crc=(s>>5)&0xFFu;\n"
     "    for(int i=0;i<40;i++){ unsigned b=("+p+"_msg[i]^(s>>(i&7)))&0xFFu;\n"
     "      crc^=b;\n"
     "      crc=((crc<<4)^"+p+"_crctab[(crc>>4)&0xFu])&0xFFu;\n"
     "      crc=((crc<<4)^"+p+"_crctab[(crc>>4)&0xFu])&0xFFu;\n"
     "      acc=acc*131u+crc; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC8u}, "OptStress108", 2},

    // 6x6 integer matrix multiply of two rodata matrices (row/col dot product).
    {p+"_matmul",
     "static const unsigned char "+p+"_ma[36]={\n"
     "3,7,1,9,4,2, 6,0,8,5,3,7, 2,9,4,1,6,8, 5,3,7,2,0,9, 1,8,6,4,2,5, 9,3,0,7,1,6};\n"
     "static const unsigned char "+p+"_mb[36]={\n"
     "8,2,5,1,7,3, 0,9,4,6,2,8, 5,3,1,7,9,0, 4,6,2,8,1,5, 7,1,9,3,5,2, 2,8,6,0,4,7};\n"
     +t+" "+p+"_matmul("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<6;i++) for(int j=0;j<6;j++){ unsigned sum=0;\n"
     "      for(int k=0;k<6;k++) sum+=(unsigned)"+p+"_ma[i*6+k]*(unsigned)"+p+"_mb[k*6+j];\n"
     "      sum^=(s>>((i+j)&15))&0xFu;\n"
     "      acc=acc*131u+sum; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Du}, "OptStress108", 2},

    // binary search of a sorted rodata key table + parallel payload gather.
    {p+"_binsrch",
     "static const unsigned char "+p+"_keys[32]={\n"
     "2,9,15,22,28,35,41,48, 54,61,67,74,80,87,93,100,\n"
     "106,113,119,126,132,139,145,152, 158,165,171,178,184,191,197,204};\n"
     "static const unsigned char "+p+"_vals[32]={\n"
     "17,3,28,9,41,6,33,52, 11,46,7,38,23,60,14,49, 5,31,57,12,44,8,26,53, 19,40,2,35,58,21,47,10};\n"
     +t+" "+p+"_binsrch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<32;q++){ unsigned key=(s>>(q&15))&0xFFu;\n"
     "      int lo=0, hi=31; unsigned found=0;\n"
     "      while(lo<=hi){ int mid=(lo+hi)>>1; unsigned mv="+p+"_keys[mid];\n"
     "        if(mv==key){ found="+p+"_vals[mid]+1u; break; }\n"
     "        else if(mv<key) lo=mid+1; else hi=mid-1; }\n"
     "      acc=acc*131u+found+(unsigned)lo; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB5u}, "OptStress108", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress108TC("x64o108", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress108TC("x86o108", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress108TC("a64o108", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress108TC("armo108", "int");

INSTANTIATE_TEST_SUITE_P(OptStress108, X64OptStress108RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress108, X86OptStress108RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress108, A64OptStress108RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress108, ARM32OptStress108RT, ::testing::ValuesIn(kARM), rtTCName);
